package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"testing"
	"time"
)

// Keep the complete login -> PIN -> export/restore -> logout path behind the
// real HTTP parser/writer. The only modem is the in-memory PIN test fixture.
func TestHTTPAppAuthPINAndBackupRoundTrip(t *testing.T) {
	dir := t.TempDir()
	modem := &pinModem{state: "SIM PIN"}
	a := &App{
		auth:    LoadAuth(dir),
		pin:     newPINController(modem.command, dir),
		store:   LoadStore(dir),
		ident:   SimIdentity{ICCID: "8900000000000000001", Key: "iccid:http-test"},
		identAt: time.Now(),
	}
	a.outbox = &Outbox{store: a.store}
	if _, err := a.auth.Setup(a.ident.Key, "", "", "test-password", false); err != nil {
		t.Fatal(err)
	}
	a.store.Msgs["keep"] = &Message{Key: "keep", Sender: "10010", Body: "原有短信", Direction: "in"}
	if err := a.store.Save(); err != nil {
		t.Fatal(err)
	}
	routes := map[string]Handler{
		"POST /api/auth/login":  a.handleAuthLogin,
		"POST /api/auth/logout": a.handleAuthLogout,
		"POST /api/sim/unlock":  a.requireAuth(a.handlePINUnlock),
		"GET /api/export":       a.requireAuth(a.handleExport),
		"POST /api/restore":     a.requireAuth(a.handleRestore),
	}
	request := func(method, path, cookie, body string) (*http.Response, []byte) {
		t.Helper()
		raw := fmt.Sprintf("%s %s HTTP/1.1\r\nHost: localhost\r\nCoOkIe: %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s", method, path, cookie, len(body), body)
		output, _ := httpMemoryRequest(raw, routes, nil, 7)
		return parseHTTPMemoryResponse(t, output)
	}
	unlock := `{"pin":"1234","request_id":"http-pin-1"}`
	if resp, _ := request("POST", "/api/sim/unlock", "", unlock); resp.StatusCode != 401 || modem.writes != 0 {
		t.Fatal("anonymous HTTP request reached the PIN controller")
	}
	if resp, _ := request("POST", "/api/auth/login", "", `{"password":"wrong-password"}`); resp.StatusCode != 401 || len(resp.Cookies()) != 0 {
		t.Fatal("failed HTTP login issued a cookie")
	}
	resp, _ := request("POST", "/api/auth/login", "", `{"password":"test-password","remember":true}`)
	cookies := resp.Cookies()
	if resp.StatusCode != 200 || len(cookies) != 1 {
		t.Fatalf("login status=%s cookies=%d", resp.Status, len(cookies))
	}
	c := cookies[0]
	if c.Name != tokenName || c.Value == "" || !c.HttpOnly || c.Path != "/" || c.SameSite != http.SameSiteLaxMode || c.MaxAge != int(rememberTTL.Seconds()) {
		t.Fatalf("HTTP login cookie policy changed: %+v", c)
	}
	cookie := c.Name + "=" + c.Value
	if resp, _ := request("POST", "/api/sim/unlock", cookie, unlock); resp.StatusCode != 200 || modem.writes != 1 {
		t.Fatal("authenticated HTTP PIN unlock failed")
	}
	if resp, _ := request("POST", "/api/sim/unlock", cookie, unlock); resp.StatusCode != 200 || modem.writes != 1 {
		t.Fatal("HTTP retry submitted the PIN twice")
	}
	resp, exported := request("GET", "/api/export?download=1", cookie, "")
	var backup SMSBackup
	if resp.StatusCode != 200 || resp.Header.Get("Content-Disposition") != "attachment; filename=\"module-sms-backup.json\"" || json.Unmarshal(exported, &backup) != nil || len(backup.Messages) != 1 {
		t.Fatal("HTTP export headers or backup content changed")
	}
	backup.Messages = nil
	for i := 0; i < 16; i++ {
		backup.Messages = append(backup.Messages, &Message{
			Key: fmt.Sprintf("import-%d", i), Sender: "10010", Body: strings.Repeat("备份", 600),
			Direction: "out", SendState: "sending", Index: i + 1,
		})
	}
	toRestore, err := json.Marshal(backup)
	if err != nil || len(toRestore) <= 32768 {
		t.Fatal("restore fixture must exercise the larger restore body limit")
	}
	resp, result := request("POST", "/api/restore", cookie, string(toRestore))
	var added struct {
		Added int `json:"added"`
	}
	if resp.StatusCode != 200 || json.Unmarshal(result, &added) != nil || added.Added != 16 {
		t.Fatalf("HTTP restore failed: %s %s", resp.Status, result)
	}
	if a.store.Get("keep") == nil || len(a.store.List()) != 17 || a.store.Get("import-0").Index != 0 || a.store.Get("import-0").SendState != "uncertain" {
		t.Fatal("HTTP restore removed an original or retained a live modem/send reference")
	}
	resp, exported = request("GET", "/api/export", cookie, "")
	if resp.StatusCode != 200 || json.Unmarshal(exported, &backup) != nil || len(backup.Messages) != 17 {
		t.Fatal("large HTTP export was truncated after restore")
	}
	resp, result = request("POST", "/api/restore", cookie, string(exported))
	if resp.StatusCode != 200 || json.Unmarshal(result, &added) != nil || added.Added != 0 {
		t.Fatal("HTTP restore of an exported backup was not idempotent")
	}
	if resp, _ := request("POST", "/api/auth/logout", cookie, `{}`); resp.StatusCode != 200 || !strings.Contains(resp.Header.Get("Set-Cookie"), "Max-Age=0") {
		t.Fatal("HTTP logout did not clear the cookie")
	}
	if resp, _ := request("GET", "/api/export", cookie, ""); resp.StatusCode != 401 {
		t.Fatal("logged-out HTTP cookie still authorizes backup export")
	}
	if resp, _ := request("POST", "/api/restore", cookie, string(toRestore)); resp.StatusCode != 401 || len(a.store.List()) != 17 || modem.writes != 1 {
		t.Fatal("logged-out HTTP request changed backup or PIN state")
	}
}
