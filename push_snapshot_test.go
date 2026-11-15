package main

import (
	"encoding/json"
	"sync"
	"testing"
	"time"
)

func TestPushSnapshotCopiesOnlyLockedMessageValues(t *testing.T) {
	original := &Message{Key: "synthetic-old", Direction: "in", Body: "synthetic body"}
	s := &Store{Msgs: map[string]*Message{"entry": original}}
	snapshot := s.pushSnapshot()
	s.mu.Lock()
	original.Key, original.Direction = "synthetic-new", "out"
	delete(s.Msgs, "entry")
	s.mu.Unlock()
	if len(snapshot) != 1 || snapshot[0].key != "synthetic-old" || snapshot[0].direction != "in" {
		t.Fatal("push snapshot retained a mutable message instead of copied values")
	}
	snapshot[0].key = "synthetic-snapshot-change"
	if original.Key != "synthetic-new" {
		t.Fatal("changing the push snapshot changed a stored message")
	}
}

func TestPushSnapshotConcurrentMessageUpdates(t *testing.T) {
	s := &Store{Msgs: map[string]*Message{"entry": {Key: "key-in", Direction: "in"}}}
	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		for i := 0; i < 1000; i++ {
			direction := "in"
			if i%2 != 0 {
				direction = "out"
			}
			s.mu.Lock()
			s.Msgs["entry"].Key = "key-" + direction
			s.Msgs["entry"].Direction = direction
			s.mu.Unlock()
		}
	}()
	for i := 0; i < 1000; i++ {
		snapshot := s.pushSnapshot()
		if len(snapshot) != 1 || snapshot[0].key != "key-"+snapshot[0].direction {
			t.Error("push snapshot mixed fields from different locked updates")
			break
		}
	}
	wg.Wait()
}

func TestPushObservationIgnoresMessageOrderAndKeepsDuplicateCounts(t *testing.T) {
	messages := []*Message{
		{Key: "old", Direction: "in"},
		{Key: "synthetic-a", Direction: "in"},
		{Key: "synthetic-b", Direction: "in"},
		{Key: "synthetic-a", Direction: "in"},
		{Key: "synthetic-a", Direction: "out"},
		{Key: "synthetic-out", Direction: "out"},
	}
	reversed := make([]*Message, len(messages))
	for i := range messages {
		reversed[len(messages)-1-i] = messages[i]
	}
	a, b := pushFixture(t), pushFixture(t)
	now := time.Unix(1794700800, 0)
	if err := a.observe(messages, CallSnapshot{}, now); err != nil {
		t.Fatal(err)
	}
	if err := b.observeRecords(pushRecords(reversed), CallSnapshot{}, now); err != nil {
		t.Fatal(err)
	}
	if len(a.state.Queue) != 1 || a.state.Queue[0].Count != 3 || len(a.state.Seen) != 4 {
		t.Fatal("incoming duplicate keys changed the original count/set behavior")
	}
	// Random event identifiers differ between services; all event payloads and
	// persisted dedupe markers must be independent of message enumeration order.
	a.state.Queue[0].ID, b.state.Queue[0].ID = "synthetic-event", "synthetic-event"
	left, _ := json.Marshal(a.state)
	right, _ := json.Marshal(b.state)
	if string(left) != string(right) {
		t.Fatal("message enumeration order changed notification state")
	}
	if err := b.observeRecords(pushRecords(messages), CallSnapshot{}, now); err != nil {
		t.Fatal(err)
	}
	if len(b.state.Queue) != 1 {
		t.Fatal("changing snapshot order replayed an incoming notification")
	}
}

func TestPushLoopSnapshotIsReleasedBeforeCallSnapshotAndPersistence(t *testing.T) {
	p := pushFixture(t)
	s := &Store{Msgs: map[string]*Message{"fresh": {Key: "synthetic-fresh", Direction: "in"}}}
	p.path = t.TempDir()
	err := p.observeCurrent(s.pushSnapshot, func() CallSnapshot {
		// A second snapshot producer may use the Store after the first returned.
		s.mu.Lock()
		delete(s.Msgs, "fresh")
		s.mu.Unlock()
		return CallSnapshot{}
	}, time.Now())
	if err == nil || len(p.state.Queue) != 0 || p.state.Seen[messagePushHash("synthetic-fresh")] {
		t.Fatal("lightweight snapshot changed failed-persistence rollback")
	}
}
