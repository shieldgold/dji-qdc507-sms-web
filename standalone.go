package main

import (
	"encoding/json"
	"errors"
	"net"
	"strconv"
)

func listenAddress(host, port string) (string, error) {
	if host != "192.168.225.1" && host != "127.0.0.1" {
		return "", errors.New("只允许监听模块 USB 地址 192.168.225.1 或本机 127.0.0.1")
	}
	n, err := strconv.Atoi(port)
	if err != nil || n < 1 || n > 65535 {
		return "", errors.New("无效 HTTP 端口")
	}
	return net.JoinHostPort(host, port), nil
}

func (a *App) handlePINStatus(w *Resp, r *Req) error {
	return writeJSON(w, 200, a.pin.Status())
}

func (a *App) handlePINUnlock(w *Resp, r *Req) error {
	var req struct {
		PIN           string `json:"pin"`
		ExpectedICCID string `json:"expected_iccid"`
		RequestID     string `json:"request_id"`
	}
	if json.Unmarshal(r.Body, &req) != nil {
		return writeJSON(w, 400, map[string]string{"error": "请求格式错误"})
	}
	a.opMu.Lock()
	defer a.opMu.Unlock()
	if req.ExpectedICCID != "" {
		current, err := a.freshSIMIdentity()
		if err != nil || current != req.ExpectedICCID {
			return writeJSON(w, 409, map[string]string{"error": "SIM 卡已变化或无法确认，未提交 PIN"})
		}
		// The outer authentication check may have used a cached SIM identity.
		if !a.auth.Check(cookieValue(r.Header["cookie"], tokenName), "iccid:"+current) {
			return writeJSON(w, 401, map[string]string{"error": "请先登录当前 SIM 卡"})
		}
	}
	status, err := a.pin.Unlock(req.PIN, req.RequestID)
	if err != nil {
		return writeJSON(w, 400, map[string]any{"error": err.Error(), "sim": status})
	}
	a.smsConfigured = false
	return writeJSON(w, 200, status)
}

func (a *App) handlePINRetry(w *Resp, r *Req) error {
	var req struct {
		Confirmed bool `json:"confirmed"`
	}
	if json.Unmarshal(r.Body, &req) != nil {
		return writeJSON(w, 400, map[string]string{"error": "请求格式错误"})
	}
	a.opMu.Lock()
	defer a.opMu.Unlock()
	if err := a.pin.AllowRetry(req.Confirmed); err != nil {
		return writeJSON(w, 400, map[string]string{"error": err.Error()})
	}
	return writeJSON(w, 200, a.pin.Status())
}

func (a *App) ensureSMSLocked() error {
	if s := a.pin.Status(); s.State != "READY" {
		a.smsConfigured = false
		return errors.New("SIM 尚未就绪，请先解锁 PIN 或检查 SIM 卡")
	}
	if !a.smsConfigured {
		if err := a.modem.SetupSMS(); err != nil {
			return err
		}
		a.smsConfigured = true
	}
	return nil
}
