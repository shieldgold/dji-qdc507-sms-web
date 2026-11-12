package main

import (
	"bufio"
	"bytes"
	"strings"
	"testing"
	"time"
)

func TestLockedSIMWebAccessAndUnlockRoute(t *testing.T) {
	m := &pinModem{state: "SIM PIN"}
	dir := t.TempDir()
	a := &App{auth: LoadAuth(dir), pin: newPINController(m.command, dir), ident: SimIdentity{ICCID: "8900000000000000001", Key: "iccid:test"}, identAt: time.Now()}
	tok, err := a.auth.Setup(a.ident.Key, "", "", "test-password", false)
	if err != nil {
		t.Fatal(err)
	}
	call := func(body, cookie string) string {
		var b bytes.Buffer
		w := &Resp{bw: bufio.NewWriter(&b)}
		r := &Req{Body: []byte(body), Header: map[string]string{"cookie": cookie}}
		if err := a.requireAuth(a.handlePINUnlock)(w, r); err != nil {
			t.Fatal(err)
		}
		return b.String()
	}
	if r := call(`{"pin":"1234","request_id":"one"}`, ""); !strings.Contains(r, "401") {
		t.Fatal("PIN endpoint must require login")
	}
	if m.writes != 0 {
		t.Fatal("unauthenticated PIN submission")
	}
	if r := call(`{"pin":"1234","request_id":"one"}`, tokenName+"="+tok); !strings.Contains(r, `"READY"`) {
		t.Fatal("authenticated unlock failed:", r)
	}
}

func TestListenAddressOnlyUSBOrLoopback(t *testing.T) {
	for _, host := range []string{"0.0.0.0", "", "::", "8.8.8.8"} {
		if _, err := listenAddress(host, "8787"); err == nil {
			t.Fatalf("unsafe bind accepted: %q", host)
		}
	}
	for _, host := range []string{"192.168.225.1", "127.0.0.1"} {
		if _, err := listenAddress(host, "8787"); err != nil {
			t.Fatal(err)
		}
	}
}
