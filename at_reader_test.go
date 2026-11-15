package main

import (
	"bufio"
	"bytes"
	"errors"
	"fmt"
	"math/rand"
	"os"
	"runtime"
	"strconv"
	"strings"
	"testing"
	"time"
)

func setupPort(t *testing.T, fail string) (*Modem, <-chan error) {
	t.Helper()
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { r.Close(); w.Close() })
	m := &Modem{f: w, lines: make(chan string, 4)}
	done := make(chan error, 1)
	go func() {
		reader := bufio.NewReader(r)
		commands := []string{"AT", "ATE0"}
		if fail == "AT+CMGF=0" {
			commands = append(commands, "AT+CMGF=0")
		}
		for _, command := range commands {
			line, err := reader.ReadString('\r')
			if err != nil {
				done <- err
				return
			}
			if line != command+"\r" {
				done <- fmt.Errorf("unexpected synthetic command %q", line)
				return
			}
			if command == fail {
				m.lines <- "ERROR"
				done <- nil
				return
			}
			m.lines <- "OK"
		}
		done <- nil
	}()
	return m, done
}

func TestModemSetupClosesFailedInitialPort(t *testing.T) {
	for _, command := range []string{"AT", "ATE0"} {
		t.Run(command, func(t *testing.T) {
			m, done := setupPort(t, command)
			if err := m.Setup(); err == nil {
				t.Fatal("initial handshake failure was ignored")
			}
			if err := <-done; err != nil {
				t.Fatal(err)
			}
			if _, err := m.f.Stat(); !errors.Is(err, os.ErrClosed) {
				t.Fatal("failed initialization retained its descriptor and reader")
			}
		})
	}
}

func TestModemSetupKeepsSuccessfulPort(t *testing.T) {
	m, done := setupPort(t, "")
	if err := m.Setup(); err != nil {
		t.Fatal(err)
	}
	if err := <-done; err != nil {
		t.Fatal(err)
	}
	if _, err := m.f.Stat(); err != nil {
		t.Fatal("successful handshake closed the modem")
	}
}

func TestModemSMSSetupErrorKeepsPortForPINRecovery(t *testing.T) {
	m, done := setupPort(t, "AT+CMGF=0")
	if err := m.SetupSMS(); err == nil {
		t.Fatal("synthetic SMS setup error was ignored")
	}
	if err := <-done; err != nil {
		t.Fatal(err)
	}
	if _, err := m.f.Stat(); err != nil {
		t.Fatal("SMS setup error must leave the port available to PIN recovery")
	}
}

func readerPipe(t testing.TB) (*Modem, *os.File) {
	t.Helper()
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	m := &Modem{f: r, lines: make(chan string, 4096)}
	done := make(chan struct{})
	go func() { m.reader(); close(done) }()
	t.Cleanup(func() {
		w.Close()
		r.Close()
		select {
		case <-done:
		case <-time.After(time.Second):
			t.Error("closed reader did not exit")
		}
	})
	return m, w
}

func readLine(t testing.TB, m *Modem) string {
	t.Helper()
	select {
	case line := <-m.lines:
		return line
	case <-time.After(time.Second):
		t.Fatal("synthetic AT line was not delivered")
		return ""
	}
}

func TestModemReaderLineAndPromptSemantics(t *testing.T) {
	for _, tc := range []struct {
		name, input string
		want        []string
	}{
		{"crlf", "\r\nOK\r\n", []string{"OK"}},
		{"leading_cr", "\r\rPDU\r\n", []string{"\r\rPDU"}},
		{"prompt", "\r\r>", []string{">"}},
		{"prompt_space", "> \r\n+CMGS: 7\r\nOK\r\n", []string{">", " ", "+CMGS: 7", "OK"}},
		{"not_a_prompt", "text> \r\n >\r\n", []string{"text> ", " >"}},
		{"multiple_prompts", ">>\r\n", []string{">", ">"}},
		{"pdu", strings.Repeat("0123456789ABCDEF", 32) + "\r\n", []string{strings.Repeat("0123456789ABCDEF", 32)}},
		{"long_line", strings.Repeat("x", 65536) + "\n", []string{strings.Repeat("x", 65536)}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			m, w := readerPipe(t)
			errch := make(chan error, 1)
			go func() {
				// One-byte writes exercise every boundary, including the prompt
				// which deliberately has no trailing newline.
				for i := range tc.input {
					if _, err := w.Write([]byte{tc.input[i]}); err != nil {
						errch <- err
						return
					}
				}
				errch <- nil
			}()
			for _, expected := range tc.want {
				if got := readLine(t, m); got != expected {
					t.Fatalf("line mismatch: got length %d, want length %d", len(got), len(expected))
				}
			}
			if err := <-errch; err != nil {
				t.Fatal(err)
			}
			select {
			case extra := <-m.lines:
				t.Fatalf("unexpected extra line of length %d", len(extra))
			default:
			}
		})
	}
}

func legacyATLines(input []byte) []string {
	var pending []byte
	var lines []string
	for _, b := range input {
		pending = append(pending, b)
		if b == '\n' {
			line := strings.TrimRight(string(pending), "\r\n")
			pending = pending[:0]
			if line != "" {
				lines = append(lines, line)
			}
			continue
		}
		if s := strings.TrimLeft(string(pending), "\r\n"); s == ">" || s == "> " {
			lines = append(lines, ">")
			pending = pending[:0]
		}
	}
	return lines
}

func TestModemReaderMatchesLegacyFraming(t *testing.T) {
	m, w := readerPipe(t)
	rng := rand.New(rand.NewSource(507))
	alphabet := []byte{'\r', '\n', '>', ' ', 'A', '0', ':', 0, 0xe4, 0xb8, 0xad}
	for sample := 0; sample < 2000; sample++ {
		input := make([]byte, rng.Intn(128))
		for i := range input {
			input[i] = alphabet[rng.Intn(len(alphabet))]
		}
		input = append(input, []byte("\n__case_end__\n")...)
		want := legacyATLines(input)
		if _, err := w.Write(input); err != nil {
			t.Fatal(err)
		}
		for i, line := range want {
			if got := readLine(t, m); got != line {
				t.Fatalf("legacy framing differs in synthetic sample %d line %d", sample, i)
			}
		}
	}
}

func TestModemReaderDeliveredLinesDoNotAliasScratch(t *testing.T) {
	m, w := readerPipe(t)
	w.Write([]byte("first message\r\n"))
	first := readLine(t, m)
	w.Write([]byte("other message\r\n"))
	if second := readLine(t, m); first != "first message" || second != "other message" {
		t.Fatal("reading another response corrupted an already delivered line")
	}
}

func TestModemReaderAllocationBudget(t *testing.T) {
	m, w := readerPipe(t)
	line := append(bytes.Repeat([]byte("A"), 256), '\r', '\n')
	allocs := testing.AllocsPerRun(100, func() {
		if _, err := w.Write(line); err != nil {
			t.Fatal(err)
		}
		if got := <-m.lines; len(got) != 256 {
			t.Fatal("synthetic AT response changed")
		}
	})
	if allocs > 4 {
		t.Fatalf("AT reader allocates %.0f times per 258-byte line; budget is 4", allocs)
	}
}

func TestModemReaderCopiesOnlyLineContent(t *testing.T) {
	m, w := readerPipe(t)
	line := append(bytes.Repeat([]byte("A"), 256), '\r', '\n')
	w.Write(line)
	<-m.lines
	var before, after runtime.MemStats
	runtime.ReadMemStats(&before)
	for i := 0; i < 1000; i++ {
		if _, err := w.Write(line); err != nil {
			t.Fatal(err)
		}
		if len(<-m.lines) != 256 {
			t.Fatal("synthetic line changed")
		}
	}
	runtime.ReadMemStats(&after)
	if perLine := (after.TotalAlloc - before.TotalAlloc) / 1000; perLine > 272 {
		t.Fatalf("reader allocates %d bytes per 256-byte content line; budget 272", perLine)
	}
}

func TestModemReaderReleasesOversizedLineBuffer(t *testing.T) {
	for _, end := range []string{"\n", ">"} {
		t.Run(strconv.Quote(end), func(t *testing.T) {
			m, w := readerPipe(t)
			w.Write([]byte("warmup\n"))
			<-m.lines
			runtime.GC()
			var before, after runtime.MemStats
			runtime.ReadMemStats(&before)
			func() {
				value := byte('x')
				if end == ">" {
					value = '\r'
				}
				line := append(bytes.Repeat([]byte{value}, 1<<20), end...)
				errors := make(chan error, 1)
				go func() { _, err := w.Write(line); errors <- err }()
				got := readLine(t, m)
				if end == ">" && got != ">" || end == "\n" && len(got) != 1<<20 {
					t.Fatal("oversized input was truncated or reinterpreted")
				}
				if err := <-errors; err != nil {
					t.Fatal(err)
				}
			}()
			// Move the reader past the delivered string's stack liveness point.
			w.Write([]byte("next\n"))
			if got := readLine(t, m); got != "next" {
				t.Fatal("reader did not continue after oversized input")
			}
			runtime.GC()
			runtime.GC()
			runtime.ReadMemStats(&after)
			if after.HeapAlloc > before.HeapAlloc+512*1024 {
				t.Fatalf("reader retained oversized input: heap grew %d bytes", after.HeapAlloc-before.HeapAlloc)
			}
		})
	}
}

func BenchmarkModemReader(b *testing.B) {
	for _, width := range []int{32, 256, 1024} {
		b.Run(strconv.Itoa(width), func(b *testing.B) {
			m, w := readerPipe(b)
			line := append(bytes.Repeat([]byte("A"), width), '\r', '\n')
			w.Write([]byte("warmup\n"))
			<-m.lines
			b.SetBytes(int64(len(line)))
			b.ReportAllocs()
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				if _, err := w.Write(line); err != nil {
					b.Fatal(err)
				}
				if len(<-m.lines) != width {
					b.Fatal("synthetic line changed")
				}
			}
			b.StopTimer()
		})
	}
}

func TestModemReaderStopsWhenFileIsClosed(t *testing.T) {
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	defer w.Close()
	m := &Modem{f: r, lines: make(chan string, 4)}
	done := make(chan struct{})
	go func() { m.reader(); close(done) }()
	if err := r.Close(); err != nil {
		t.Fatal(err)
	}
	select {
	case <-done:
	case <-time.After(500 * time.Millisecond):
		t.Fatal("reader retained a retry goroutine after its file was explicitly closed")
	}
}
