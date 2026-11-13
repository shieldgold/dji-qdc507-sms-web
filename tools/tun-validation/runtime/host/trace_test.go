package main

import (
	"errors"
	"testing"
	"time"
)

func TestTraceRetainsPartialReadAndError(t *testing.T) {
	wireError := errors.New("aborted")
	f := &traceFake{readError: wireError}
	var events []transferEvent
	tr := &tracedTransport{transport: f, save: func(e transferEvent) error { events = append(events, e); return nil }}
	b := make([]byte, 24)
	n, err := tr.Read(b, time.Second)
	if n != 3 || !errors.Is(err, wireError) || string(b[:n]) != "abc" {
		t.Fatalf("partial read changed: %d %q %v", n, b, err)
	}
	if len(events) != 1 || events[0].Transferred != 3 || events[0].Requested != 24 || events[0].Error != "aborted" || events[0].Operation != "read" {
		t.Fatalf("lost transfer evidence: %+v", events)
	}
}

func TestTraceFailureStopsCommandBeforeAcknowledgement(t *testing.T) {
	f := &fakeWire{}
	f.service = func(_ string, f *fakeWire) { f.enqueue(okay, 7, 1, nil) }
	err := newClient(&tracedTransport{transport: f, save: func(e transferEvent) error {
		if e.Operation == "read" {
			return errors.New("disk full")
		}
		return nil
	}}).shell("true", time.Second, func([]byte) error { t.Fatal("output delivered after trace failure"); return nil }, "", "")
	if err == nil || f.opens != 0 {
		t.Fatalf("command executed without durable trace: %v, opens=%d", err, f.opens)
	}
}

type traceFake struct{ readError error }

func (f *traceFake) Read(p []byte, _ time.Duration) (int, error) { return copy(p, "abc"), f.readError }
func (*traceFake) Write([]byte) error                            { return nil }
func (*traceFake) Close() error                                  { return nil }
