package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"regexp"
	"sync"
	"time"
)

type PushSubscription struct {
	ICCID        string `json:"iccid"`
	DeviceID     string `json:"device_id"`
	Token        string `json:"token"`
	VoIPToken    string `json:"voip_token"`
	Enabled      bool   `json:"enabled"`
	CallsEnabled bool   `json:"calls_enabled"`
}
type PushEvent struct {
	ID       string    `json:"id"`
	Kind     string    `json:"kind"`
	Count    int       `json:"count,omitempty"`
	CallID   string    `json:"call_id,omitempty"`
	CallUUID string    `json:"call_uuid,omitempty"`
	Created  time.Time `json:"created"`
	Next     time.Time `json:"next"`
	Attempts int       `json:"attempts"`
}
type pushState struct {
	Subscription PushSubscription `json:"subscription"`
	Seen         map[string]bool  `json:"seen"`
	Calls        map[string]bool  `json:"calls"`
	Queue        []PushEvent      `json:"queue"`
}
type PushDelivery struct {
	Kind     string `json:"kind"`
	EventID  string `json:"event_id"`
	Token    string `json:"token"`
	SentAt   int64  `json:"sent_at"`
	Count    int    `json:"count,omitempty"`
	CallID   string `json:"call_id,omitempty"`
	CallUUID string `json:"call_uuid,omitempty"`
}
type PushService struct {
	mu           sync.Mutex
	dir, path    string
	state        pushState
	loadErr      error
	lastError    string
	accepted     int
	lastAccepted string
	inFlight     bool
	configured   bool
	deliver      func(PushDelivery) error
}

func loadPush(dir string) *PushService {
	p := &PushService{dir: dir, path: filepath.Join(dir, "push-state.json")}
	if b, e := os.ReadFile(p.path); e == nil {
		p.loadErr = json.Unmarshal(b, &p.state)
	} else if !os.IsNotExist(e) {
		p.loadErr = e
	}
	if p.state.Seen == nil {
		p.state.Seen = map[string]bool{}
	}
	if p.loadErr != nil {
		p.lastError = "通知设置无法读取，原文件已保留"
	}
	if p.state.Calls == nil {
		p.state.Calls = map[string]bool{}
	}
	p.deliver, p.configured = newPushTransport(dir)
	return p
}
func messagePushHash(key string) string {
	v := sha256.Sum256([]byte(key))
	return hex.EncodeToString(v[:])
}
func callPushUUID(id string) string {
	v := sha256.Sum256([]byte("module-call:" + id))
	v[6] = (v[6] & 15) | 0x40
	v[8] = (v[8] & 63) | 0x80
	s := hex.EncodeToString(v[:16])
	return s[:8] + "-" + s[8:12] + "-" + s[12:16] + "-" + s[16:20] + "-" + s[20:]
}
func newPushID() string { return callPushUUID(randomHex(32)) }
func (p *PushService) mutateLocked(fn func()) error {
	if p.loadErr != nil {
		return errors.New("通知设置无法读取，原文件已保留")
	}
	old, _ := json.Marshal(p.state)
	fn()
	next, e := json.Marshal(p.state)
	if e == nil {
		e = atomicWrite(p.path, next)
	}
	if e != nil {
		var restored pushState
		_ = json.Unmarshal(old, &restored)
		p.state = restored
		p.lastError = "通知设置保存失败"
		return errors.New(p.lastError)
	}
	return nil
}

var pushTokenPattern = regexp.MustCompile(`^[a-f0-9]{64,512}$`)

func (p *PushService) register(s PushSubscription, messages []*Message) error {
	if s.ICCID == "" || len(s.DeviceID) < 1 || len(s.DeviceID) > 80 {
		return errors.New("缺少设备或 SIM 标识")
	}
	if s.Enabled && (!pushTokenPattern.MatchString(s.Token) || len(s.Token)%2 != 0) {
		return errors.New("等待苹果短信推送令牌")
	}
	if s.CallsEnabled && (!pushTokenPattern.MatchString(s.VoIPToken) || len(s.VoIPToken)%2 != 0) {
		return errors.New("等待苹果来电推送令牌")
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.loadErr != nil {
		return errors.New("通知设置无法读取，原文件已保留")
	}
	if s == p.state.Subscription {
		return nil
	}
	return p.mutateLocked(func() {
		changed := p.state.Subscription.ICCID != s.ICCID || p.state.Subscription.DeviceID != s.DeviceID
		if changed {
			p.state.Calls = map[string]bool{}
			p.state.Queue = nil
		}
		if changed || (!p.state.Subscription.Enabled && s.Enabled) {
			p.state.Seen = map[string]bool{}
			for _, m := range messages {
				p.state.Seen[messagePushHash(m.Key)] = true
			}
		}
		p.state.Subscription = s
		p.lastError = ""
		if !s.Enabled {
			q := p.state.Queue[:0]
			for _, e := range p.state.Queue {
				if e.Kind == "call" {
					q = append(q, e)
				}
			}
			p.state.Queue = q
		}
		if !s.CallsEnabled {
			q := p.state.Queue[:0]
			for _, e := range p.state.Queue {
				if e.Kind != "call" {
					q = append(q, e)
				}
			}
			p.state.Queue = q
		}
	})
}
func (p *PushService) bindCard(card string) {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.state.Subscription.ICCID == "" || p.state.Subscription.ICCID == card {
		return
	}
	_ = p.mutateLocked(func() {
		p.state.Subscription = PushSubscription{}
		p.state.Queue = nil
		p.state.Seen = map[string]bool{}
		p.state.Calls = map[string]bool{}
	})
	p.lastError = "SIM 已更换，请重新开启通知"
}
func (p *PushService) observe(messages []*Message, calls CallSnapshot, now time.Time) error {
	p.mu.Lock()
	defer p.mu.Unlock()
	s := p.state.Subscription
	if s.ICCID == "" || (!s.Enabled && !s.CallsEnabled) {
		return nil
	}
	before, _ := json.Marshal(p.state)
	seen := map[string]bool{}
	count := 0
	for _, m := range messages {
		h := messagePushHash(m.Key)
		seen[h] = true
		if s.Enabled && m.Direction == "in" && !p.state.Seen[h] {
			count++
		}
	}
	ringing := map[string]bool{}
	var added []PushEvent
	for _, c := range calls.Calls {
		if c.Direction == "in" && (c.State == "incoming" || c.State == "waiting") {
			ringing[c.ID] = true
			if s.CallsEnabled && !p.state.Calls[c.ID] {
				added = append(added, PushEvent{ID: newPushID(), Kind: "call", CallID: c.ID, CallUUID: callPushUUID(c.ID), Created: now, Next: now})
				p.state.Calls[c.ID] = true
			}
		}
	}
	if calls.Error != "" {
		ringing = p.state.Calls
	} // an uncertain read must not invent a hangup
	q := []PushEvent{}
	for _, e := range p.state.Queue {
		if e.Kind != "call" || ringing[e.CallID] {
			q = append(q, e)
		}
	}
	p.state.Queue = append(added, q...)
	if count > 0 && len(p.state.Queue) < 32 {
		p.state.Queue = append(p.state.Queue, PushEvent{ID: newPushID(), Kind: "sms", Count: count, Created: now, Next: now})
	} else if count > 0 {
		seen = p.state.Seen
	}
	p.state.Seen = seen
	if calls.Error == "" {
		p.state.Calls = ringing
	}
	after, _ := json.Marshal(p.state)
	if string(before) == string(after) {
		return nil
	}
	// Persist the dedupe marker and event atomically before allowing a send.
	var original pushState
	_ = json.Unmarshal(before, &original)
	p.state = original
	return p.mutateLocked(func() { var next pushState; _ = json.Unmarshal(after, &next); p.state = next })
}
func (p *PushService) pending() bool { p.mu.Lock(); defer p.mu.Unlock(); return len(p.state.Queue) > 0 }
func (p *PushService) sendNext(card string, now time.Time) {
	p.mu.Lock()
	if p.inFlight || len(p.state.Queue) == 0 {
		p.mu.Unlock()
		return
	}
	if card == "" {
		p.lastError = "等待实时确认模块 SIM"
		p.mu.Unlock()
		return
	}
	if card != p.state.Subscription.ICCID {
		_ = p.mutateLocked(func() {
			p.state.Queue = nil
			p.state.Subscription = PushSubscription{}
		})
		p.lastError = "SIM 已更换，请重新开启通知"
		p.mu.Unlock()
		return
	}
	event := p.state.Queue[0]
	if now.Before(event.Next) {
		p.mu.Unlock()
		return
	}
	ttl := 5 * time.Minute
	if event.Kind == "call" {
		ttl = 20 * time.Second
	}
	if now.Sub(event.Created) > ttl {
		_ = p.mutateLocked(func() { p.state.Queue = p.state.Queue[1:] })
		p.mu.Unlock()
		return
	}
	token := p.state.Subscription.Token
	if event.Kind == "call" {
		token = p.state.Subscription.VoIPToken
	}
	p.inFlight = true
	p.mu.Unlock()
	err := p.deliver(PushDelivery{Kind: event.Kind, EventID: event.ID, Token: token, SentAt: now.Unix(), Count: event.Count, CallID: event.CallID, CallUUID: event.CallUUID})
	p.mu.Lock()
	defer p.mu.Unlock()
	p.inFlight = false
	for i, e := range p.state.Queue {
		if e.ID != event.ID {
			continue
		}
		_ = p.mutateLocked(func() {
			if err == nil {
				p.state.Queue = append(p.state.Queue[:i], p.state.Queue[i+1:]...)
				p.accepted++
				p.lastAccepted = now.UTC().Format(time.RFC3339)
				p.lastError = ""
				return
			}
			p.lastError = err.Error()
			var permanent *pushPermanentError
			if errors.As(err, &permanent) || e.Attempts >= 4 {
				p.state.Queue = append(p.state.Queue[:i], p.state.Queue[i+1:]...)
			} else {
				p.state.Queue[i].Attempts++
				p.state.Queue[i].Next = now.Add(time.Duration(5<<e.Attempts) * time.Second)
			}
		})
		break
	}
}
func (p *PushService) status() map[string]any {
	p.mu.Lock()
	defer p.mu.Unlock()
	s := p.state.Subscription
	return map[string]any{"configured": p.configured, "registered": s.ICCID != "", "device_id": s.DeviceID, "sms_enabled": s.Enabled, "calls_enabled": s.CallsEnabled, "queued": len(p.state.Queue), "accepted": p.accepted, "last_accepted": p.lastAccepted, "error": p.lastError}
}
func (a *App) pushLoop() {
	for {
		now := applicationTime.Now()
		_ = a.push.observe(a.store.List(), a.calls.Snapshot(), now)
		if a.push.pending() {
			a.opMu.Lock()
			card, e := a.freshSIMIdentity()
			a.opMu.Unlock()
			if e != nil {
				card = ""
			}
			a.push.sendNext(card, applicationTime.Now())
		}
		time.Sleep(time.Second)
	}
}
func (a *App) handlePushStatus(w *Resp, r *Req) error { return writeJSON(w, 200, a.push.status()) }
func (a *App) handlePushRegister(w *Resp, r *Req) error {
	var req struct {
		PushSubscription
		ExpectedICCID string `json:"expected_iccid"`
		Environment   string `json:"environment"`
	}
	if json.Unmarshal(r.Body, &req) != nil {
		return writeJSON(w, 400, map[string]string{"error": "通知设置格式错误"})
	}
	if req.Environment != "development" {
		return writeJSON(w, 400, map[string]string{"error": "推送环境不匹配"})
	}
	if (req.Enabled || req.CallsEnabled) && !a.push.configured {
		return writeJSON(w, 409, map[string]string{"error": "后台推送服务尚未配置，前台提醒仍可使用"})
	}
	a.opMu.Lock()
	defer a.opMu.Unlock()
	card, e := a.freshSIMIdentity()
	if e != nil || card != req.ExpectedICCID || !a.auth.Check(cookieValue(r.Header["cookie"], tokenName), "iccid:"+card) {
		return writeJSON(w, 409, map[string]string{"error": "SIM 已变化，请重新连接后开启通知"})
	}
	req.ICCID = card
	if e := a.push.register(req.PushSubscription, a.store.List()); e != nil {
		return writeJSON(w, 400, map[string]string{"error": e.Error()})
	}
	return writeJSON(w, 200, a.push.status())
}
func (a *App) handlePushTest(w *Resp, r *Req) error {
	a.push.mu.Lock()
	defer a.push.mu.Unlock()
	if !a.push.configured || !a.push.state.Subscription.Enabled {
		return writeJSON(w, 409, map[string]string{"error": "请先开启后台短信通知"})
	}
	for _, e := range a.push.state.Queue {
		if e.Kind == "test" {
			return writeJSON(w, 200, map[string]bool{"queued": true})
		}
	}
	now := applicationTime.Now()
	e := a.push.mutateLocked(func() {
		a.push.state.Queue = append(a.push.state.Queue, PushEvent{ID: newPushID(), Kind: "test", Created: now, Next: now.Add(8 * time.Second)})
	})
	if e != nil {
		return writeJSON(w, 500, map[string]string{"error": e.Error()})
	}
	return writeJSON(w, 200, map[string]bool{"queued": true})
}
