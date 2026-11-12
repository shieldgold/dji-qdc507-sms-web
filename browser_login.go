package main

import (
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"os"
	"time"
)

// Handoffs are held only in memory, bound to the issuing session and live SIM.
// The browser receives its own persistent HttpOnly cookie, never the app's token.
type browserLoginTicket struct {
	parent   string
	identity string
	expires  time.Time
}

func (a *AuthStore) browserTicket(parent, identity string) (string, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	session := a.Tokens[parent]
	if session == nil || session.Identity != identity || session.Expires < time.Now().Unix() {
		return "", errors.New("请先在 App 登录当前模块 SIM 卡")
	}
	if a.browserTickets == nil {
		a.browserTickets = make(map[string]browserLoginTicket)
	}
	for key, ticket := range a.browserTickets {
		if time.Now().After(ticket.expires) || ticket.parent == parent {
			delete(a.browserTickets, key)
		}
	}
	if len(a.browserTickets) >= 8 {
		return "", errors.New("免密请求过多，请稍后重试")
	}
	random := make([]byte, 32)
	f, err := os.Open("/dev/urandom")
	if err != nil {
		return "", errors.New("暂时无法生成免密链接")
	}
	_, err = io.ReadFull(f, random)
	_ = f.Close()
	if err != nil {
		return "", errors.New("暂时无法生成免密链接")
	}
	key := hex.EncodeToString(random)
	a.browserTickets[key] = browserLoginTicket{parent: parent, identity: identity, expires: time.Now().Add(30 * time.Second)}
	return key, nil
}

func (a *AuthStore) redeemBrowserTicket(key, identity string) (string, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	ticket, ok := a.browserTickets[key]
	delete(a.browserTickets, key) // consume even if the card or issuing session changed
	parent := a.Tokens[ticket.parent]
	if !ok || time.Now().After(ticket.expires) || ticket.identity != identity || parent == nil || parent.Identity != identity || parent.Expires < time.Now().Unix() {
		return "", errors.New("免密链接已过期或 SIM 已变化，请返回 App 重新打开")
	}
	token, _ := a.newTokenLocked(identity, true)
	if err := a.saveLocked(); err != nil {
		delete(a.Tokens, token)
		return "", err
	}
	return token, nil
}

func (a *App) handleBrowserLoginTicket(w *Resp, r *Req) error {
	a.opMu.Lock()
	defer a.opMu.Unlock()
	card, err := a.freshSIMIdentity()
	if err != nil {
		return writeJSON(w, 409, map[string]string{"error": err.Error()})
	}
	ticket, err := a.auth.browserTicket(cookieValue(r.Header["cookie"], tokenName), "iccid:"+card)
	if err != nil {
		return writeJSON(w, 401, map[string]string{"error": err.Error()})
	}
	return writeJSON(w, 200, map[string]any{"ticket": ticket, "expires_in": 30})
}

func (a *App) handleBrowserLoginRedeem(w *Resp, r *Req) error {
	var body struct {
		Ticket string `json:"ticket"`
	}
	if json.Unmarshal(r.Body, &body) != nil || len(body.Ticket) != 64 {
		return writeJSON(w, 400, map[string]string{"error": "无效的免密链接，请返回 App 重新打开"})
	}
	a.opMu.Lock()
	defer a.opMu.Unlock()
	card, err := a.freshSIMIdentity()
	if err != nil {
		return writeJSON(w, 409, map[string]string{"error": err.Error()})
	}
	token, err := a.auth.redeemBrowserTicket(body.Ticket, "iccid:"+card)
	if err != nil {
		return writeJSON(w, 401, map[string]string{"error": err.Error()})
	}
	w.AddCookie(cookieHeader(token, int(rememberTTL.Seconds())))
	return writeJSON(w, 200, map[string]bool{"ok": true, "remember": true})
}
