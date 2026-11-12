package main

import (
	"errors"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func waitOutbox(t *testing.T, q *Outbox, id string) *Message {
	t.Helper()
	for deadline := time.Now().Add(time.Second); time.Now().Before(deadline); {
		m := q.Status(id)
		if m != nil && m.SendState != "sending" && m.SendState != "queued" {
			return m
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatal("send did not finish")
	return nil
}
func TestOutboxPersistsBeforeSubmitAndDeduplicates(t *testing.T) {
	dir := t.TempDir()
	s := LoadStore(dir)
	var calls int32
	release := make(chan struct{})
	q := NewOutbox(s, func(p []string, progress func(int)) error {
		saved := LoadStore(dir).List()
		if len(saved) != 1 {
			t.Error("outgoing intent not persisted before modem")
		}
		atomic.AddInt32(&calls, 1)
		<-release
		progress(len(p))
		return nil
	})
	req := SendRequest{RequestID: "request-0001", Number: "10010", Text: "test"}
	if _, e := q.Enqueue(req); e != nil {
		t.Fatal(e)
	}
	if _, e := q.Enqueue(req); e != nil {
		t.Fatal(e)
	}
	different := req
	different.Text = "other"
	if _, e := q.Enqueue(different); e == nil {
		t.Fatal("id reuse with different body accepted")
	}
	close(release)
	m := waitOutbox(t, q, req.RequestID)
	if m.SendState != "submitted" || atomic.LoadInt32(&calls) != 1 {
		t.Fatalf("state %s calls %d", m.SendState, calls)
	}
	q2 := NewOutbox(LoadStore(dir), func([]string, func(int)) error { t.Error("resent persisted request"); return nil })
	if _, e := q2.Enqueue(req); e != nil {
		t.Fatal(e)
	}
}
func TestOutboxUncertainAndPartialNeverAutoRetry(t *testing.T) {
	for _, sent := range []int{0, 1} {
		t.Run(string(rune('0'+sent)), func(t *testing.T) {
			s := LoadStore(t.TempDir())
			n := 0
			q := NewOutbox(s, func(_ []string, progress func(int)) error {
				n++
				progress(sent)
				return &SubmissionError{Uncertain: true, Err: errors.New("timeout")}
			})
			req := SendRequest{RequestID: "request-partial", Number: "10010", Text: strings.Repeat("中", 100)}
			if _, e := q.Enqueue(req); e != nil {
				t.Fatal(e)
			}
			m := waitOutbox(t, q, req.RequestID)
			want := "uncertain"
			if sent > 0 {
				want = "partial"
			}
			if m.SendState != want || m.SentParts != sent {
				t.Fatalf("%+v", m)
			}
			if _, e := q.Enqueue(req); e != nil {
				t.Fatal(e)
			}
			if n != 1 {
				t.Fatal("repeated send")
			}
		})
	}
}
func TestOutboxRecoversInterruptedSendWithoutResending(t *testing.T) {
	s := LoadStore(t.TempDir())
	s.Msgs["out:request-crash"] = &Message{Key: "out:request-crash", RequestID: "request-crash", Direction: "out", SendState: "sending", Sender: "10010", Body: "test"}
	if e := s.Save(); e != nil {
		t.Fatal(e)
	}
	q := NewOutbox(s, func([]string, func(int)) error { t.Fatal("unexpected send"); return nil })
	if q.Status("request-crash").SendState != "uncertain" {
		t.Fatal("crash should remain uncertain")
	}
}
func TestSubmitValidationLimits(t *testing.T) {
	for _, number := range []string{"+", strings.Repeat("1", 21), "10010;ATD"} {
		if _, e := buildSubmit(number, "test", 1); e == nil {
			t.Errorf("accepted number %q", number)
		}
	}
	if _, e := buildSubmit("10010", strings.Repeat("中", 1000), 1); e == nil {
		t.Error("unbounded multipart accepted")
	}
}
func TestStoreListReturnsSnapshots(t *testing.T) {
	s := LoadStore(t.TempDir())
	s.Msgs["a"] = &Message{Key: "a", Body: "original"}
	s.List()[0].Body = "changed"
	if s.List()[0].Body != "original" {
		t.Fatal("returned mutable store pointer")
	}
}
