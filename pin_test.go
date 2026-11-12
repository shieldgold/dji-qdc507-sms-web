package main

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

type pinModem struct {
	state  string
	writes int
	fail   bool
}

func (m *pinModem) command(cmd string, _ time.Duration) ([]string, error) {
	if cmd == "AT+CPIN?" {
		return []string{"+CPIN: " + m.state, "OK"}, nil
	}
	if strings.HasPrefix(cmd, "AT+CPIN=") {
		m.writes++
		if m.fail {
			return nil, errors.New("timeout echo " + cmd)
		}
		m.state = "READY"
		return []string{"OK"}, nil
	}
	return nil, errors.New("unexpected command")
}

func TestPINUnlockOnlyExactSIMLock(t *testing.T) {
	for _, state := range []string{"READY", "SIM PUK", "SIM PIN2", "SIM PUK2", "UNKNOWN"} {
		t.Run(state, func(t *testing.T) {
			m := &pinModem{state: state}
			p := newPINController(m.command, t.TempDir())
			_, _ = p.Unlock("1234", "request-1")
			if m.writes != 0 {
				t.Fatal("attempted PIN for wrong state")
			}
		})
	}
}

func TestPINConcurrentRequestsSubmitOnce(t *testing.T) {
	m := &pinModem{state: "SIM PIN"}
	p := newPINController(m.command, t.TempDir())
	var wg sync.WaitGroup
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func() { defer wg.Done(); _, _ = p.Unlock("1234", "request-1") }()
	}
	wg.Wait()
	if m.writes != 1 {
		t.Fatalf("PIN writes=%d", m.writes)
	}
	if p.Status().State != "READY" {
		t.Fatal("SIM should be ready")
	}
}

func TestPINFailureSurvivesRestartWithoutSecret(t *testing.T) {
	dir := t.TempDir()
	m := &pinModem{state: "SIM PIN", fail: true}
	p := newPINController(m.command, dir)
	_, err := p.Unlock("1234", "request-1")
	if err == nil || strings.Contains(err.Error(), "1234") {
		t.Fatal("failure must be reported without PIN")
	}
	p = newPINController(m.command, dir)
	_, _ = p.Unlock("1234", "request-2")
	if m.writes != 1 || !p.Status().Blocked {
		t.Fatal("must not retry an uncertain submission")
	}
	files, _ := os.ReadDir(dir)
	for _, f := range files {
		b, _ := os.ReadFile(filepath.Join(dir, f.Name()))
		if strings.Contains(string(b), "1234") {
			t.Fatal("PIN persisted")
		}
	}
	if err := p.AllowRetry(false); err == nil {
		t.Fatal("retry needs explicit acknowledgement")
	}
	if err := p.AllowRetry(true); err != nil {
		t.Fatal(err)
	}
	m.fail = false
	if _, err := p.Unlock("1234", "request-3"); err != nil {
		t.Fatal(err)
	}
	if m.writes != 2 {
		t.Fatal("explicit retry did not run once")
	}
}

func TestPINRejectsMalformedInputAndUnavailableGuard(t *testing.T) {
	m := &pinModem{state: "SIM PIN"}
	p := newPINController(m.command, t.TempDir())
	for _, pin := range []string{"", "123", "123456789", "12\"3", "１２３４"} {
		if _, err := p.Unlock(pin, "req"); err == nil {
			t.Fatalf("accepted invalid PIN %q", pin)
		}
	}
	if m.writes != 0 {
		t.Fatal("invalid input reached modem")
	}
	dir := t.TempDir()
	f := filepath.Join(dir, "file")
	_ = os.WriteFile(f, []byte("x"), 0600)
	p = newPINController(m.command, filepath.Join(f, "invalid"))
	if _, err := p.Unlock("1234", "req"); err == nil || m.writes != 0 {
		t.Fatal("must not submit when guard cannot be saved")
	}
}
