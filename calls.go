package main

import (
	"encoding/csv"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"
)

type VoiceCall struct {
	ID        string `json:"id"`
	Index     int    `json:"index"`
	Number    string `json:"number"`
	Direction string `json:"direction"`
	State     string `json:"state"`
	RequestID string `json:"request_id,omitempty"`
}

type CallSnapshot struct {
	Calls     []VoiceCall `json:"calls"`
	Pending   bool        `json:"pending"`
	UpdatedAt string      `json:"updated_at"`
	Error     string      `json:"error,omitempty"`
}

type dialAttempt struct {
	RequestID string `json:"request_id"`
	Number    string `json:"number"`
}

// One controller owns voice commands. CLCC mode=1 entries belong to the data
// connection and must never become calls or be disconnected with ATH.
type CallController struct {
	mu               sync.Mutex
	command          func(string, time.Duration) ([]string, error)
	dir              string
	snapshot         CallSnapshot
	updated          time.Time
	pendingAt        time.Time
	pendingRequestID string
	attempts         []dialAttempt
	loadErr          error
}

func newCallController(command func(string, time.Duration) ([]string, error), dir string) *CallController {
	c := &CallController{command: command, dir: dir, snapshot: CallSnapshot{Calls: []VoiceCall{}}}
	if b, err := os.ReadFile(filepath.Join(dir, "call-attempts.json")); err == nil {
		c.loadErr = json.Unmarshal(b, &c.attempts)
	} else if !os.IsNotExist(err) {
		c.loadErr = err
	}
	return c
}

var dialNumberPattern = regexp.MustCompile(`^\+?[0-9]{3,20}$`)

func parseVoiceCalls(lines []string) ([]VoiceCall, error) {
	out := []VoiceCall{}
	states := []string{"active", "held", "dialing", "alerting", "incoming", "waiting"}
	for _, line := range lines {
		if !strings.HasPrefix(line, "+CLCC:") {
			continue
		}
		r := csv.NewReader(strings.NewReader(strings.TrimSpace(strings.TrimPrefix(line, "+CLCC:"))))
		r.TrimLeadingSpace = true
		f, err := r.Read()
		if err != nil || len(f) < 5 {
			return nil, errors.New("无法解析模块呼叫列表")
		}
		n := make([]int, 5)
		for i := range n {
			n[i], err = strconv.Atoi(strings.TrimSpace(f[i]))
			if err != nil {
				return nil, errors.New("模块呼叫列表包含无效字段")
			}
		}
		if n[0] < 1 || n[1] < 0 || n[1] > 1 || n[2] < 0 || n[2] >= len(states) {
			return nil, errors.New("模块呼叫状态无效")
		}
		if n[3] != 0 {
			continue
		}
		call := VoiceCall{Index: n[0], State: states[n[2]], Direction: "out"}
		if n[1] == 1 {
			call.Direction = "in"
		}
		if len(f) > 5 {
			call.Number = f[5]
		}
		out = append(out, call)
	}
	return out, nil
}

func (c *CallController) copyLocked() CallSnapshot {
	s := c.snapshot
	s.Calls = append([]VoiceCall{}, s.Calls...)
	return s
}

func (c *CallController) Snapshot() CallSnapshot {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.copyLocked()
}

func (c *CallController) Busy() bool {
	if c == nil {
		return false
	}
	s := c.Snapshot()
	return s.Pending || len(s.Calls) != 0
}

func (c *CallController) refreshLocked() (CallSnapshot, error) {
	lines, err := c.command("AT+CLCC", 3*time.Second)
	var calls []VoiceCall
	if err == nil {
		calls, err = parseVoiceCalls(lines)
	}
	if err != nil {
		c.snapshot.Error = "无法确认电话状态，请检查模块连接"
		return c.copyLocked(), err
	}
	for i := range calls {
		for _, old := range c.snapshot.Calls {
			if old.Index == calls[i].Index && old.Direction == calls[i].Direction && old.Number == calls[i].Number {
				calls[i].ID = old.ID
				calls[i].RequestID = old.RequestID
				break
			}
		}
		if calls[i].ID == "" {
			calls[i].ID = randomHex(12)
			if calls[i].Direction == "out" && !c.pendingAt.IsZero() {
				calls[i].RequestID = c.pendingRequestID
			}
		}
	}
	c.snapshot.Calls = calls
	c.snapshot.Error = ""
	if len(calls) > 0 || (!c.pendingAt.IsZero() && time.Since(c.pendingAt) > 15*time.Second) {
		c.snapshot.Pending = false
		c.pendingAt = time.Time{}
		c.pendingRequestID = ""
	}
	c.updated = time.Now()
	c.snapshot.UpdatedAt = applicationTime.Now().UTC().Format(time.RFC3339)
	return c.copyLocked(), nil
}

func (c *CallController) Refresh() (CallSnapshot, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.refreshLocked()
}

func (c *CallController) Loop() {
	for {
		_, _ = c.Refresh()
		time.Sleep(time.Second)
	}
}

func (c *CallController) Dial(number, requestID string) (CallSnapshot, error) {
	if !dialNumberPattern.MatchString(number) || !requestIDPattern.MatchString(requestID) {
		return c.Snapshot(), errors.New("请输入有效电话号码和请求标识")
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	for _, a := range c.attempts {
		if a.RequestID == requestID {
			if a.Number != number {
				return c.copyLocked(), errors.New("请求标识已经用于另一个号码")
			}
			return c.copyLocked(), errors.New("该拨号请求已处理，请刷新通话状态；不会重复拨号")
		}
	}
	s, err := c.refreshLocked()
	if err != nil {
		return s, errors.New("电话状态无法确认，未拨号")
	}
	if len(s.Calls) > 0 || s.Pending {
		return s, errors.New("已有电话或拨号正在进行")
	}
	if c.loadErr != nil {
		return s, errors.New("拨号保护记录无法读取，未拨号")
	}
	// Persist before ATD so an HTTP retry or service restart cannot duplicate it.
	attempts := append(append([]dialAttempt{}, c.attempts...), dialAttempt{requestID, number})
	if len(attempts) > 64 {
		attempts = attempts[len(attempts)-64:]
	}
	b, _ := json.Marshal(attempts)
	if err := atomicWrite(filepath.Join(c.dir, "call-attempts.json"), b); err != nil {
		return s, errors.New("拨号保护记录未保存，未拨号")
	}
	c.attempts = attempts
	c.snapshot.Pending = true
	c.pendingAt = time.Now()
	c.pendingRequestID = requestID
	_, err = c.command("ATD"+number+";", 10*time.Second)
	if err != nil {
		c.snapshot.Error = "拨号结果尚未确认，请刷新状态，勿重复拨号"
		return c.copyLocked(), errors.New(c.snapshot.Error)
	}
	return c.copyLocked(), nil
}

func (c *CallController) action(id, command string, allowed ...string) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	s, err := c.refreshLocked()
	if err != nil {
		return errors.New("无法确认当前电话，未执行操作")
	}
	for _, call := range s.Calls {
		if call.ID != id {
			continue
		}
		for _, state := range allowed {
			if call.State == state {
				// CHUP ends voice calls only. First version supports one voice call;
				// avoid silently ending another call when a second line is waiting.
				if len(s.Calls) > 1 {
					return errors.New("检测到多路电话，请在模块的原电话终端处理")
				}
				if _, e := c.command(command, 5*time.Second); e != nil {
					return errors.New("模块未确认操作结果，请刷新电话状态")
				}
				return nil
			}
		}
		return errors.New("当前电话状态不支持此操作")
	}
	return errors.New("这通电话已结束或状态已变化，请刷新")
}

func (c *CallController) Answer(id string) error { return c.action(id, "ATA", "incoming", "waiting") }
func (c *CallController) Hangup(id string) error {
	return c.action(id, "AT+CHUP", "active", "held", "dialing", "alerting", "incoming", "waiting")
}
func (c *CallController) DTMF(id, digit string) error {
	if len(digit) != 1 || !strings.Contains("0123456789*#ABCD", digit) {
		return errors.New("无效的按键")
	}
	return c.action(id, fmt.Sprintf("AT+VTS=\"%s\"", digit), "active")
}
