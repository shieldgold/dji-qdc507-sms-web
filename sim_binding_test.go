package main

import (
	"bufio"
	"bytes"
	"errors"
	"strings"
	"testing"
	"time"
)

func TestFreshICCIDNeverFallsBackToCachedCard(t *testing.T) {
	a := &App{pin: newPINController(func(string, time.Duration) ([]string, error) { return nil, errors.New("no card") }, t.TempDir()), ident: SimIdentity{ICCID: "8900000000000000001", Key: "iccid:old"}}
	if id, err := a.freshSIMIdentity(); err == nil || id != "" || a.ident.Key != "unknown" {
		t.Fatal("reused cached card")
	}
}
func TestBoundPINRejectsChangedCardBeforeConsumingAttempt(t *testing.T) {
	old, current := "8900000000000000001", "8900000000000000002"
	modem := &pinModem{state: "SIM PIN"}
	command := func(cmd string, timeout time.Duration) ([]string, error) {
		if cmd == "AT+QCCID" {
			return []string{"+QCCID: " + current}, nil
		}
		return modem.command(cmd, timeout)
	}
	dir := t.TempDir()
	a := &App{pin: newPINController(command, dir), auth: LoadAuth(dir), ident: SimIdentity{ICCID: old, Key: "iccid:" + old}, identAt: time.Now()}
	tok, err := a.auth.Setup("iccid:"+old, "", "", "test-password", false)
	if err != nil {
		t.Fatal(err)
	}
	invoke := func(expected string) string {
		var b bytes.Buffer
		w := &Resp{bw: bufio.NewWriter(&b)}
		r := &Req{Body: []byte(`{"pin":"1234","request_id":"test","expected_iccid":"` + expected + `"}`), Header: map[string]string{"cookie": tokenName + "=" + tok}}
		if err := a.handlePINUnlock(w, r); err != nil {
			t.Fatal(err)
		}
		return b.String()
	}
	if response := invoke(old); !strings.Contains(response, "409") {
		t.Fatal("changed card not rejected")
	}
	if response := invoke(current); !strings.Contains(response, "401") {
		t.Fatal("old session authorized new card")
	}
	if modem.writes != 0 || a.pin.Status().Blocked {
		t.Fatal("rejected request consumed attempt")
	}
}
func TestBoundPINAcceptsSameCardAndDoesNotPersistSecret(t *testing.T) {
	id := "8900000000000000001"
	modem := &pinModem{state: "SIM PIN"}
	command := func(cmd string, timeout time.Duration) ([]string, error) {
		if cmd == "AT+QCCID" {
			return []string{"+QCCID: " + id}, nil
		}
		return modem.command(cmd, timeout)
	}
	dir := t.TempDir()
	a := &App{pin: newPINController(command, dir), auth: LoadAuth(dir)}
	token, err := a.auth.Setup("iccid:"+id, "", "", "test-password", false)
	if err != nil {
		t.Fatal(err)
	}
	var b bytes.Buffer
	r := &Req{Body: []byte(`{"pin":"1234","request_id":"test","expected_iccid":"` + id + `"}`), Header: map[string]string{"cookie": tokenName + "=" + token}}
	if err := a.handlePINUnlock(&Resp{bw: bufio.NewWriter(&b)}, r); err != nil {
		t.Fatal(err)
	}
	if modem.writes != 1 || !strings.Contains(b.String(), `"READY"`) {
		t.Fatal("same-card unlock failed")
	}
}
func TestBoundLoginDoesNotUsePasswordForAnotherCard(t *testing.T) {
	dir := t.TempDir()
	a := &App{pin: newPINController(func(string, time.Duration) ([]string, error) { return []string{"+QCCID: 8900000000000000002"}, nil }, dir), auth: LoadAuth(dir)}
	var b bytes.Buffer
	r := &Req{Body: []byte(`{"password":"unused-secret","expected_iccid":"8900000000000000001","remember":true}`)}
	if err := a.handleAuthLogin(&Resp{bw: bufio.NewWriter(&b)}, r); err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(b.String(), "409") || len(a.auth.Tokens) != 0 || strings.Contains(b.String(), "unused-secret") {
		t.Fatal("cross-card login accepted or leaked password")
	}
}
