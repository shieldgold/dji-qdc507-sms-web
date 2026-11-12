package main

import (
	"errors"
	"strings"
	"time"
)

// readCurrentICCID never uses the cached identity. A missing/unreadable card must
// never inherit the previous card's saved PIN or login password.
func readCurrentICCID(command func(string, time.Duration) ([]string, error)) (string, error) {
	for _, cmd := range []string{"AT+QCCID", "AT+CCID", "AT+ICCID"} {
		lines, err := command(cmd, 3*time.Second)
		if err != nil {
			continue
		}
		for _, line := range lines {
			p := strings.SplitN(line, ":", 2)
			if len(p) != 2 || (p[0] != "+QCCID" && p[0] != "+CCID" && p[0] != "+ICCID") {
				continue
			}
			id := strings.Trim(strings.TrimSpace(p[1]), "\"")
			if len(id) >= 15 && len(id) <= 22 && strings.Trim(id, "0123456789") == "" {
				return id, nil
			}
		}
	}
	return "", errors.New("未能实时识别 SIM 卡，未使用保存的凭据")
}

func (a *App) freshSIMIdentity() (string, error) {
	id, err := readCurrentICCID(a.pin.command)
	a.identMu.Lock()
	defer a.identMu.Unlock()
	if err != nil {
		a.ident = SimIdentity{Key: "unknown"}
		a.identAt = time.Time{}
		return "", err
	}
	if a.auth != nil {
		if err := a.auth.bindIdentity("iccid:" + id); err != nil {
			return "", err
		}
	}
	if a.ident.ICCID != id {
		a.ident = SimIdentity{ICCID: id, Key: "iccid:" + id}
	}
	if a.push != nil {
		a.push.bindCard(id)
	}
	a.identAt = time.Now()
	return id, nil
}

// Public metadata, like /api/auth/status. No credential or session is returned.
func (a *App) handleSIMBinding(w *Resp, r *Req) error {
	a.opMu.Lock()
	defer a.opMu.Unlock()
	id, err := a.freshSIMIdentity()
	if err != nil {
		return writeJSON(w, 503, map[string]string{"error": err.Error()})
	}
	return writeJSON(w, 200, map[string]any{"iccid": id, "sim": a.pin.Status(), "binding_version": 1})
}
