package main

import (
	"bufio"
	"bytes"
	"encoding/json"
	"errors"
	"strings"
	"testing"
	"time"
)

func browserLoginFixture(t *testing.T) (*App, string, *string) {
	t.Helper()
	card := "8900000000000000001"
	dir := t.TempDir()
	a := &App{auth: LoadAuth(dir)}
	a.pin = newPINController(func(cmd string, _ time.Duration) ([]string, error) {
		if card == "" {
			return nil, errors.New("card removed")
		}
		if cmd == "AT+QCCID" {
			return []string{"+QCCID: " + card}, nil
		}
		return []string{"+CPIN: READY"}, nil
	}, dir)
	token, err := a.auth.Setup("iccid:"+card, "", "", "test-password", true)
	if err != nil {
		t.Fatal(err)
	}
	return a, token, &card
}
func browserRequest(t *testing.T, h Handler, cookie, body string) (string, map[string]any) {
	t.Helper()
	var b bytes.Buffer
	err := h(&Resp{bw: bufio.NewWriter(&b)}, &Req{Method: "POST", Body: []byte(body), Header: map[string]string{"cookie": tokenName + "=" + cookie}})
	if err != nil {
		t.Fatal(err)
	}
	raw := b.String()
	parts := strings.SplitN(raw, "\r\n\r\n", 2)
	var data map[string]any
	if len(parts) != 2 || json.Unmarshal([]byte(parts[1]), &data) != nil {
		t.Fatal("invalid response")
	}
	return raw, data
}
func issueBrowserTicket(t *testing.T, a *App, token string) string {
	t.Helper()
	raw, data := browserRequest(t, a.handleBrowserLoginTicket, token, `{}`)
	if !strings.Contains(raw, "200 OK") {
		t.Fatal("cannot issue browser login", data)
	}
	return data["ticket"].(string)
}
func TestBrowserLoginRequiresAuthenticationAndPersistsNewCookie(t *testing.T) {
	a, parent, card := browserLoginFixture(t)
	if raw, _ := browserRequest(t, a.handleBrowserLoginTicket, "", `{}`); !strings.Contains(raw, "401") {
		t.Fatal("anonymous ticket issued")
	}
	ticket := issueBrowserTicket(t, a, parent)
	raw, _ := browserRequest(t, a.handleBrowserLoginRedeem, "", `{"ticket":"`+ticket+`"}`)
	if !strings.Contains(raw, "200 OK") || !strings.Contains(raw, "HttpOnly") || !strings.Contains(raw, "Max-Age=31536000") {
		t.Fatal("missing remembered cookie")
	}
	start := strings.Index(raw, "Set-Cookie: ") + len("Set-Cookie: ")
	child := cookieValue(strings.SplitN(raw[start:], "\r\n", 2)[0], tokenName)
	if child == parent || !LoadAuth(strings.TrimSuffix(a.auth.path, "/auth.json")).Check(child, "iccid:"+*card) {
		t.Fatal("browser login not independently persisted")
	}
	if raw, _ := browserRequest(t, a.handleBrowserLoginRedeem, "", `{"ticket":"`+ticket+`"}`); !strings.Contains(raw, "401") {
		t.Fatal("ticket reused")
	}
}
func TestBrowserLoginRejectsExpiredRevokedChangedAndMissingSIM(t *testing.T) {
	for _, mode := range []string{"expired", "logout", "password", "swapped", "removed", "restart"} {
		t.Run(mode, func(t *testing.T) {
			a, parent, card := browserLoginFixture(t)
			ticket := issueBrowserTicket(t, a, parent)
			switch mode {
			case "expired":
				g := a.auth.browserTickets[ticket]
				g.expires = time.Now().Add(-time.Second)
				a.auth.browserTickets[ticket] = g
			case "logout":
				if err := a.auth.Logout(parent); err != nil {
					t.Fatal(err)
				}
			case "password":
				if err := a.auth.ChangePassword("iccid:"+*card, "test-password", "replacement"); err != nil {
					t.Fatal(err)
				}
			case "swapped":
				*card = "8900000000000000002"
			case "removed":
				*card = ""
			case "restart":
				a.auth = LoadAuth(strings.TrimSuffix(a.auth.path, "/auth.json"))
			}
			raw, _ := browserRequest(t, a.handleBrowserLoginRedeem, "", `{"ticket":"`+ticket+`"}`)
			if strings.Contains(raw, "200 OK") || strings.Contains(raw, "Set-Cookie:") {
				t.Fatal("invalid handoff accepted")
			}
		})
	}
}
func TestBrowserLoginRejectsStaleParentSIMAndStorageFailure(t *testing.T) {
	a, parent, card := browserLoginFixture(t)
	*card = "8900000000000000002"
	if raw, _ := browserRequest(t, a.handleBrowserLoginTicket, parent, `{}`); !strings.Contains(raw, "401") {
		t.Fatal("old session authorizes changed card")
	}
	*card = "8900000000000000001"
	if _, err := a.freshSIMIdentity(); err != nil {
		t.Fatal(err)
	}
	parent, err := a.auth.Login("iccid:"+*card, "test-password", true)
	if err != nil {
		t.Fatal(err)
	}
	ticket := issueBrowserTicket(t, a, parent)
	before := len(a.auth.Tokens)
	a.auth.path = t.TempDir()
	raw, _ := browserRequest(t, a.handleBrowserLoginRedeem, "", `{"ticket":"`+ticket+`"}`)
	if strings.Contains(raw, "200 OK") || strings.Contains(raw, "Set-Cookie:") || len(a.auth.Tokens) != before {
		t.Fatal("persistence failure claimed login")
	}
}

func TestBrowserCookieDoesNotReturnAfterSIMSwapAndServiceRestart(t *testing.T) {
	a, parent, card := browserLoginFixture(t)
	if _, err := a.freshSIMIdentity(); err != nil {
		t.Fatal(err)
	}
	dir := strings.TrimSuffix(a.auth.path, "/auth.json")
	*card = "8900000000000000002"
	a.auth = LoadAuth(dir)
	if _, err := a.freshSIMIdentity(); err != nil {
		t.Fatal(err)
	}
	*card = "8900000000000000001"
	a.auth = LoadAuth(dir)
	if _, err := a.freshSIMIdentity(); err != nil {
		t.Fatal(err)
	}
	if a.auth.Check(parent, "iccid:"+*card) {
		t.Fatal("old browser cookie returned after changing SIMs")
	}
	if _, err := a.auth.Login("iccid:"+*card, "test-password", true); err != nil {
		t.Fatal("SIM change must not erase the registered password")
	}
}
