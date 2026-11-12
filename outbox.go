package main

import (
	"errors"
	"fmt"
	"regexp"
	"strings"
	"sync"
	"time"
)

type SendRequest struct {
	RequestID string `json:"request_id"`
	Number    string `json:"number"`
	Text      string `json:"text"`
}

type SubmissionError struct {
	Uncertain bool
	Err       error
}

func (e *SubmissionError) Error() string { return e.Err.Error() }
func (e *SubmissionError) Unwrap() error { return e.Err }

// A request is written before contacting the modem. Retrying the same ID only
// reads that record, including after a restart or an uncertain network outcome.
type Outbox struct {
	mu     sync.Mutex
	store  *Store
	active bool
	run    func([]string, func(int)) error
}

var requestIDPattern = regexp.MustCompile(`^[A-Za-z0-9_-]{8,80}$`)

func NewOutbox(s *Store, run func([]string, func(int)) error) *Outbox {
	q := &Outbox{store: s, run: run}
	changed := false
	s.mu.Lock()
	for _, m := range s.Msgs {
		if m.SendState == "queued" || m.SendState == "sending" {
			m.SendState = "uncertain"
			m.Status = "结果待确认"
			m.SendError = "服务曾中断，请核对接收端后再决定是否重新发送"
			changed = true
		}
	}
	s.mu.Unlock()
	if changed {
		_ = s.Save()
	}
	return q
}
func (q *Outbox) Status(id string) *Message { return q.store.Get("out:" + id) }
func (q *Outbox) Busy() bool                { q.mu.Lock(); defer q.mu.Unlock(); return q.active }
func (q *Outbox) Enqueue(r SendRequest) (*Message, error) {
	if !requestIDPattern.MatchString(r.RequestID) {
		return nil, errors.New("缺少有效的发送请求标识，请刷新后重试")
	}
	r.Number = strings.TrimSpace(r.Number)
	pdus, err := buildSubmit(r.Number, r.Text, int(time.Now().UnixNano()%255)+1)
	if err != nil {
		return nil, err
	}
	q.mu.Lock()
	defer q.mu.Unlock()
	if old := q.Status(r.RequestID); old != nil {
		if old.Sender != r.Number || old.Body != r.Text {
			return nil, errors.New("请求标识已用于另一条短信，请重新创建短信")
		}
		return old, nil
	}
	if q.active {
		return nil, errors.New("上一条短信仍在提交，请等待结果后再发送")
	}
	m := &Message{Key: "out:" + r.RequestID, RequestID: r.RequestID, Sender: r.Number, Body: r.Text, Time: applicationTime.Now().UTC().Format(time.RFC3339), Direction: "out", Status: "等待发送", SendState: "queued", Parts: len(pdus)}
	q.store.mu.Lock()
	q.store.Msgs[m.Key] = m
	q.store.mu.Unlock()
	if err := q.store.Save(); err != nil {
		q.store.mu.Lock()
		delete(q.store.Msgs, m.Key)
		q.store.mu.Unlock()
		return nil, errors.New("无法保存发送记录，短信尚未提交")
	}
	q.active = true
	go q.process(m.Key, pdus)
	return q.store.Get(m.Key), nil
}
func (q *Outbox) update(key string, f func(*Message)) {
	q.store.mu.Lock()
	if m := q.store.Msgs[key]; m != nil {
		f(m)
	}
	q.store.mu.Unlock()
	// The initial queued record remains the restart guard even if this update fails.
	_ = q.store.Save()
}
func (q *Outbox) process(key string, pdus []string) {
	defer func() { q.mu.Lock(); q.active = false; q.mu.Unlock() }()
	q.update(key, func(m *Message) { m.SendState = "sending"; m.Status = "正在提交" })
	sent := 0
	err := q.run(pdus, func(n int) { sent = n; q.update(key, func(m *Message) { m.SentParts = n }) })
	q.update(key, func(m *Message) {
		if err == nil {
			m.SendState = "submitted"
			m.Status = "已提交，送达未确认"
			m.SentParts = len(pdus)
			return
		}
		var failure *SubmissionError
		switch {
		case sent > 0:
			m.SendState = "partial"
			m.Status = "部分已提交，结果待确认"
		case errors.As(err, &failure) && failure.Uncertain:
			m.SendState = "uncertain"
			m.Status = "结果待确认"
		default:
			m.SendState = "failed"
			m.Status = "提交失败"
		}
		m.SendError = fmt.Sprintf("%s（已确认提交 %d/%d 条分片）", err.Error(), sent, len(pdus))
	})
}
func (a *App) runOutgoing(pdus []string, progress func(int)) error {
	a.opMu.Lock()
	defer a.opMu.Unlock()
	if err := a.ensureSMSLocked(); err != nil {
		return err
	}
	for i, p := range pdus {
		if _, err := a.modem.SendPDU(p, false); err != nil {
			return err
		}
		progress(i + 1)
	}
	return nil
}
func (a *App) handleSendStatus(w *Resp, r *Req) error {
	id := queryValue(r.Query, "request_id")
	if !requestIDPattern.MatchString(id) {
		return writeJSON(w, 400, map[string]string{"error": "无效请求标识"})
	}
	m := a.outbox.Status(id)
	if m == nil {
		return writeJSON(w, 404, map[string]string{"error": "尚未找到该发送记录，请先刷新列表，勿直接重发"})
	}
	return writeJSON(w, 200, m)
}
