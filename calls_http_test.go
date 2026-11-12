package main

import (
	"bufio"
	"bytes"
	"io"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestCallRoutesRequireLoginAndSMSMaintenanceGuards(t *testing.T) {
	dir := t.TempDir()
	writes := 0
	a := &App{store: LoadStore(dir), auth: LoadAuth(dir), ident: SimIdentity{Key: "iccid:test"}, identAt: time.Now()}
	a.calls = newCallController(func(s string, _ time.Duration) ([]string, error) {
		if strings.HasPrefix(s, "ATD") {
			writes++
		}
		return []string{"OK"}, nil
	}, dir)
	a.outbox = NewOutbox(a.store, nil)
	tok, _ := a.auth.Setup(a.ident.Key, "", "", "test-password", false)
	call := func(cookie string) string {
		var b bytes.Buffer
		_ = a.requireAuth(a.handleCallDial)(&Resp{bw: bufio.NewWriter(&b)}, &Req{Body: []byte(`{"number":"10086","request_id":"dial-http-once"}`), Header: map[string]string{"cookie": cookie}})
		return b.String()
	}
	if r := call(""); !strings.Contains(r, "401") || writes != 0 {
		t.Fatal("unauthenticated dial", r)
	}
	a.maintenance = true
	if r := call(tokenName + "=" + tok); !strings.Contains(r, "409") || writes != 0 {
		t.Fatal("dial during maintenance", r)
	}
	a.maintenance = false
	a.outbox.active = true
	if r := call(tokenName + "=" + tok); !strings.Contains(r, "409") || writes != 0 {
		t.Fatal("dial during SMS submission", r)
	}
}

func TestAudioTicketsSingleUseAndExpiring(t *testing.T) {
	c := newCallController(nil, t.TempDir())
	c.snapshot.Calls = []VoiceCall{{ID: "test-call", State: "dialing"}}
	a := newCallAudio(c, t.TempDir())
	ticket := a.issueTicket()
	if _, ok := a.consumeTicket("wrong"); ok {
		t.Fatal("accepted wrong ticket")
	}
	_, once := a.consumeTicket(ticket)
	_, twice := a.consumeTicket(ticket)
	if !once || twice {
		t.Fatal("ticket must work once")
	}
	ticket = a.issueTicket()
	a.expires = time.Now().Add(-time.Second)
	if _, ok := a.consumeTicket(ticket); ok {
		t.Fatal("accepted expired ticket")
	}
}

func TestAudioTicketRequiresCurrentCall(t *testing.T) {
	c := newCallController(nil, t.TempDir())
	a := newCallAudio(c, t.TempDir())
	if ticket := a.issueTicket(); ticket != "" {
		t.Fatal("issued microphone access without a current call")
	}
}

func TestAudioDisconnectWhileRingingEndsOnlyThatCall(t *testing.T) {
	hungup := make(chan struct{}, 1)
	c := newCallController(func(cmd string, _ time.Duration) ([]string, error) {
		if cmd == "AT+CHUP" {
			hungup <- struct{}{}
		}
		return []string{`+CLCC: 3,0,3,0,0,"10086",129`, "OK"}, nil
	}, t.TempDir())
	_, _ = c.Refresh()
	a := newCallAudio(c, t.TempDir())
	ticket := a.issueTicket()
	server, client := net.Pipe()
	done := make(chan struct{})
	go func() { a.serveConn(server); close(done) }()
	_ = client.SetDeadline(time.Now().Add(2 * time.Second))
	_, _ = io.WriteString(client, "PCM1 "+ticket+"\n")
	b := make([]byte, 3)
	if _, err := io.ReadFull(client, b); err != nil || string(b) != "OK\n" {
		t.Fatalf("handshake: %q %v", b, err)
	}
	client.Close()
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("audio handler leaked")
	}
	select {
	case <-hungup:
	default:
		t.Fatal("disconnected caller left the module ringing")
	}
}

func TestAudioTicketCannotAttachToTheNextCall(t *testing.T) {
	c := newCallController(nil, t.TempDir())
	c.snapshot.Calls = []VoiceCall{{ID: "old", State: "dialing"}}
	a := newCallAudio(c, t.TempDir())
	ticket := a.issueTicket()
	c.snapshot.Calls = []VoiceCall{{ID: "new", State: "active"}}
	if _, ok := a.consumeTicket(ticket); ok {
		t.Fatal("old ticket attached to a new conversation")
	}
}

func TestCalibrationReadinessRejectsDeadOrReusedProcess(t *testing.T) {
	root := t.TempDir()
	proc := filepath.Join(root, "proc", "42")
	if err := os.MkdirAll(proc, 0700); err != nil {
		t.Fatal(err)
	}
	marker := filepath.Join(root, "ready")
	os.WriteFile(marker, []byte("42 1234\n"), 0600)
	os.WriteFile(filepath.Join(proc, "cmdline"), []byte("/usr/bin/alsaucm_test\x00"), 0600)
	stat := "42 (alsaucm_test) S " + strings.Repeat("0 ", 18) + "1234 0\n"
	os.WriteFile(filepath.Join(proc, "stat"), []byte(stat), 0600)
	if !calibrationAlive(marker, filepath.Join(root, "proc")) {
		t.Fatal("live calibrator not ready")
	}
	os.WriteFile(filepath.Join(proc, "stat"), []byte(strings.Replace(stat, "1234", "9999", 1)), 0600)
	if calibrationAlive(marker, filepath.Join(root, "proc")) {
		t.Fatal("reused PID accepted")
	}
	os.Remove(filepath.Join(proc, "stat"))
	if calibrationAlive(marker, filepath.Join(root, "proc")) {
		t.Fatal("dead process accepted")
	}
}

func TestVendorLogsStayBounded(t *testing.T) {
	var out bytes.Buffer
	log := &audioLog{writer: &out, remaining: 64}
	for i := 0; i < 100; i++ {
		if n, e := log.Write([]byte(strings.Repeat("x", 32))); n != 32 || e != nil {
			t.Fatalf("write = %d %v", n, e)
		}
	}
	if out.Len() != 64 {
		t.Fatalf("log grew to %d bytes", out.Len())
	}
}
