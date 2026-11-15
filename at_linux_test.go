package main

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"runtime"
	"strings"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

// A real Linux PTY is needed here: os.File.Stat reporting ErrClosed alone does
// not prove that a blocked syscall released the underlying kernel descriptor.
func modemPTY(t *testing.T) (*os.File, string) {
	t.Helper()
	master, err := os.OpenFile("/dev/ptmx", os.O_RDWR|syscall.O_NOCTTY|syscall.O_NONBLOCK, 0)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { master.Close() })
	var number uint32
	var unlock int32
	control, err := master.SyscallConn()
	if err != nil {
		t.Fatal(err)
	}
	var ioctlErr syscall.Errno
	err = control.Control(func(fd uintptr) {
		_, _, ioctlErr = syscall.Syscall(syscall.SYS_IOCTL, fd, syscall.TIOCSPTLCK, uintptr(unsafe.Pointer(&unlock)))
		if ioctlErr == 0 {
			_, _, ioctlErr = syscall.Syscall(syscall.SYS_IOCTL, fd, syscall.TIOCGPTN, uintptr(unsafe.Pointer(&number)))
		}
	})
	if err != nil || ioctlErr != 0 {
		t.Fatalf("PTY ioctl: %v %v", err, ioctlErr)
	}
	path := fmt.Sprintf("/dev/pts/%d", number)
	slave, err := os.OpenFile(path, os.O_RDWR|syscall.O_NOCTTY|syscall.O_NONBLOCK, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer slave.Close()
	control, err = slave.SyscallConn()
	if err != nil {
		t.Fatal(err)
	}
	var termios syscall.Termios
	err = control.Control(func(fd uintptr) {
		_, _, ioctlErr = syscall.Syscall(syscall.SYS_IOCTL, fd, syscall.TCGETS, uintptr(unsafe.Pointer(&termios)))
		if ioctlErr == 0 {
			termios.Iflag, termios.Oflag, termios.Lflag = 0, 0, 0
			termios.Cflag = (termios.Cflag &^ syscall.CSIZE) | syscall.CS8 | syscall.CLOCAL | syscall.CREAD
			termios.Cc[syscall.VMIN], termios.Cc[syscall.VTIME] = 1, 0
			_, _, ioctlErr = syscall.Syscall(syscall.SYS_IOCTL, fd, syscall.TCSETS, uintptr(unsafe.Pointer(&termios)))
		}
	})
	if err != nil || ioctlErr != 0 {
		t.Fatalf("PTY termios: %v %v", err, ioctlErr)
	}
	return master, path
}

func modemReaders() (all, reading int) {
	stack := make([]byte, 256*1024)
	n := runtime.Stack(stack, true)
	for _, goroutine := range strings.Split(string(stack[:n]), "\n\n") {
		if strings.Contains(goroutine, "(*Modem).reader(") {
			all++
			if strings.Contains(goroutine, "internal/poll.(*FD).Read(") {
				reading++
			}
		}
	}
	return
}

func modemRawFD(t *testing.T, m *Modem) int {
	t.Helper()
	control, err := m.f.SyscallConn()
	if err != nil {
		t.Fatal(err)
	}
	fd := -1
	if err := control.Control(func(raw uintptr) { fd = int(raw) }); err != nil {
		t.Fatal(err)
	}
	return fd
}

func waitModemFDReleased(t *testing.T, fd, baseline int) {
	t.Helper()
	until := time.Now().Add(time.Second)
	for time.Now().Before(until) {
		var stat syscall.Stat_t
		closed := errors.Is(syscall.Fstat(fd, &stat), syscall.EBADF)
		readers, _ := modemReaders()
		if closed && readers <= baseline {
			return
		}
		time.Sleep(5 * time.Millisecond)
	}
	var stat syscall.Stat_t
	readers, _ := modemReaders()
	t.Fatalf("closed modem retained kernel FD or reader: fstat=%v readers=%d baseline=%d", syscall.Fstat(fd, &stat), readers, baseline)
}

func TestOpenModemCloseReleasesBlockedPTY(t *testing.T) {
	master, path := modemPTY(t)
	baseline, _ := modemReaders()
	m, err := OpenModem(path)
	if err != nil {
		t.Fatal(err)
	}
	fd := modemRawFD(t, m)
	t.Cleanup(func() {
		master.Close() // also release the broken blocking implementation after RED
		m.f.Close()
	})
	until := time.Now().Add(time.Second)
	for {
		_, reading := modemReaders()
		if reading > 0 {
			break
		}
		if time.Now().After(until) {
			t.Fatal("modem reader never entered a real PTY read")
		}
		time.Sleep(5 * time.Millisecond)
	}
	closed := make(chan error, 1)
	go func() { closed <- m.f.Close() }()
	select {
	case err := <-closed:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("closing modem blocked behind its kernel PTY read")
	}
	waitModemFDReleased(t, fd, baseline)
}

func TestModemSetupFailureReleasesPTYDescriptor(t *testing.T) {
	master, path := modemPTY(t)
	baseline, _ := modemReaders()
	m, err := OpenModem(path)
	if err != nil {
		t.Fatal(err)
	}
	fd := modemRawFD(t, m)
	t.Cleanup(func() { master.Close(); m.f.Close() })
	responded := make(chan error, 1)
	go func() {
		_, err := bufio.NewReader(master).ReadString('\r')
		if err == nil {
			_, err = master.Write([]byte("\r\nERROR\r\n"))
		}
		responded <- err
	}()
	setup := make(chan error, 1)
	go func() { setup <- m.Setup() }()
	if err := <-responded; err != nil {
		t.Fatal(err)
	}
	select {
	case err := <-setup:
		if err == nil {
			t.Fatal("synthetic initial AT error was accepted")
		}
	case <-time.After(2 * time.Second):
		t.Fatal("failed setup blocked while closing its PTY reader")
	}
	waitModemFDReleased(t, fd, baseline)
}
