package main

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

type PINStatus struct {
	State   string `json:"state"`
	Blocked bool   `json:"blocked"`
}
type PINController struct {
	mu      sync.Mutex
	command func(string, time.Duration) ([]string, error)
	guard   string
}

func newPINController(command func(string, time.Duration) ([]string, error), dir string) *PINController {
	return &PINController{command: command, guard: filepath.Join(dir, "pin-unlock.pending")}
}

func (p *PINController) statusLocked() PINStatus {
	s := PINStatus{State: "UNKNOWN"}
	lines, err := p.command("AT+CPIN?", 5*time.Second)
	if err == nil {
		for _, l := range lines {
			if strings.HasPrefix(l, "+CPIN:") {
				s.State = strings.TrimSpace(strings.TrimPrefix(l, "+CPIN:"))
			}
		}
	}
	if s.State == "READY" {
		_ = os.Remove(p.guard)
	}
	_, err = os.Stat(p.guard)
	s.Blocked = err == nil || !os.IsNotExist(err)
	return s
}

func (p *PINController) Status() PINStatus {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.statusLocked()
}

func (p *PINController) Unlock(pin, requestID string) (PINStatus, error) {
	p.mu.Lock()
	defer p.mu.Unlock()
	if len(pin) < 4 || len(pin) > 8 || strings.Trim(pin, "0123456789") != "" {
		return PINStatus{}, errors.New("PIN 必须为 4–8 位数字")
	}
	if requestID == "" || len(requestID) > 128 {
		return PINStatus{}, errors.New("缺少解锁请求标识")
	}
	s := p.statusLocked()
	if s.State == "READY" {
		return s, nil
	}
	if s.State != "SIM PIN" {
		return s, errors.New("当前不是 SIM PIN 锁定状态，请检查 SIM 状态；不能在这里输入 PUK 或 PIN2")
	}
	if s.Blocked {
		return s, errors.New("上次解锁未确认成功；请核实 PIN 后明确允许再试，程序不会自动重试")
	}
	// Persist the attempt before consuming a SIM retry. Never persist the PIN.
	f, err := os.OpenFile(p.guard, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if err != nil {
		return s, errors.New("无法保存解锁保护记录，未提交 PIN")
	}
	_, err = f.WriteString("attempt pending\n")
	if err == nil {
		err = f.Sync()
	}
	closeErr := f.Close()
	if err == nil {
		err = closeErr
	}
	if err != nil {
		return s, errors.New("解锁保护记录未保存，未提交 PIN")
	}
	// Lower layers may include the echoed command in errors; never return it.
	_, err = p.command("AT+CPIN=\""+pin+"\"", 10*time.Second)
	s = p.statusLocked()
	if s.State == "READY" {
		return s, nil
	}
	if err != nil {
		return s, errors.New("PIN 未确认解锁成功。请核对 PIN，切勿连续尝试；SIM 可能扣除了重试次数")
	}
	return s, errors.New("模块已接受解锁请求，但尚未确认 READY；请刷新状态，暂勿重试")
}

// An uncertain attempt survives reboot and can only be cleared explicitly.
func (p *PINController) AllowRetry(confirmed bool) error {
	p.mu.Lock()
	defer p.mu.Unlock()
	if !confirmed {
		return errors.New("请先确认已核实正确 PIN")
	}
	if s := p.statusLocked(); s.State != "SIM PIN" {
		return errors.New("仅 SIM PIN 状态允许重新授权")
	}
	if err := os.Remove(p.guard); err != nil && !os.IsNotExist(err) {
		return errors.New("无法清除解锁保护记录")
	}
	return nil
}
