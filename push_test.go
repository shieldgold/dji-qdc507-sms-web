package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"strings"
	"sync"
	"testing"
	"time"
)

func pushFixture(t *testing.T) *PushService {
	t.Helper()
	p := loadPush(t.TempDir())
	if err := p.register(PushSubscription{ICCID: "card-a", DeviceID: "phone-a", Token: strings.Repeat("a", 64), VoIPToken: strings.Repeat("b", 64), Enabled: true, CallsEnabled: true}, []*Message{{Key: "old", Direction: "in"}}); err != nil {
		t.Fatal(err)
	}
	return p
}
func TestPushCallUUIDMatchesTheIOSContract(t *testing.T) {
	if callPushUUID("abcd1234abcd1234abcd1234") != "0162a0a6-3d46-4427-95df-6cea76f87699" {
		t.Fatal("call identity differs from iOS")
	}
}
func TestPushOnlyNewIncomingMessagesAndPersistsDedupe(t *testing.T) {
	p := pushFixture(t)
	old := &Message{Key: "old", Direction: "in"}
	fresh := &Message{Key: "new", Direction: "in", Body: "private verification 123456", Sender: "13800000000"}
	out := &Message{Key: "out", Direction: "out"}
	if err := p.observe([]*Message{old, fresh, out}, CallSnapshot{}, time.Now()); err != nil {
		t.Fatal(err)
	}
	if len(p.state.Queue) != 1 || p.state.Queue[0].Count != 1 {
		t.Fatal("wrong alerts")
	}
	data, _ := os.ReadFile(p.path)
	if strings.Contains(string(data), fresh.Body) || strings.Contains(string(data), fresh.Sender) {
		t.Fatal("push archive leaked message content")
	}
	p = loadPush(p.dir)
	p.observe([]*Message{old, fresh, out}, CallSnapshot{}, time.Now())
	if len(p.state.Queue) != 1 {
		t.Fatal("notification repeated after restart")
	}
}
func TestPushOneCallAlertAndDropEndedCall(t *testing.T) {
	p := pushFixture(t)
	call := VoiceCall{ID: strings.Repeat("c", 24), Direction: "in", State: "incoming", Number: "13800000000"}
	p.observe(nil, CallSnapshot{Calls: []VoiceCall{call}}, time.Now())
	p.observe(nil, CallSnapshot{Calls: []VoiceCall{call}}, time.Now())
	if len(p.state.Queue) != 1 || p.state.Queue[0].Kind != "call" {
		t.Fatal("duplicate ringing notification")
	}
	p.observe(nil, CallSnapshot{}, time.Now())
	if len(p.state.Queue) != 0 {
		t.Fatal("stale ringing notification kept")
	}
}
func TestPushSIMSwapNeverDeliversToOldPhone(t *testing.T) {
	p := pushFixture(t)
	p.observe([]*Message{{Key: "fresh", Direction: "in"}}, CallSnapshot{}, time.Now())
	calls := 0
	p.deliver = func(PushDelivery) error { calls++; return nil }
	p.sendNext("card-b", time.Now())
	if calls != 0 || p.state.Subscription.Enabled || len(p.state.Queue) != 0 {
		t.Fatal("cross-SIM notification")
	}
}
func TestPushRetriesTransportFailureWithSameID(t *testing.T) {
	p := pushFixture(t)
	p.observe([]*Message{{Key: "fresh", Direction: "in"}}, CallSnapshot{}, time.Now())
	id := p.state.Queue[0].ID
	n := 0
	p.deliver = func(d PushDelivery) error {
		n++
		if d.EventID != id {
			t.Fatal("changed id on retry")
		}
		return errors.New("offline")
	}
	now := time.Now()
	p.sendNext("card-a", now)
	p.sendNext("card-a", now)
	if n != 1 || len(p.state.Queue) != 1 {
		t.Fatal("retry loop")
	}
	p.deliver = func(PushDelivery) error { return nil }
	p.sendNext("card-a", now.Add(20*time.Second))
	if len(p.state.Queue) != 0 {
		t.Fatal("acknowledged notification still queued")
	}
}

func TestPushChangedObservationKeepsPersistedWallClockSemantics(t *testing.T) {
	p := pushFixture(t)
	now := time.Now()
	if now == now.Round(0) {
		t.Fatal("test needs a real monotonic clock reading")
	}
	// sendNext can leave an in-memory retry deadline with a monotonic component.
	// The old changed-state JSON round trip stripped it, as it did new events.
	p.state.Queue = []PushEvent{{ID: "existing", Kind: "sms", Created: now.Add(-time.Second), Next: now.Add(time.Second)}}
	if err := p.observe([]*Message{{Key: "fresh", Direction: "in"}}, CallSnapshot{}, now); err != nil {
		t.Fatal(err)
	}
	if len(p.state.Queue) != 2 {
		t.Fatal("expected existing and newly queued SMS")
	}
	for _, event := range p.state.Queue {
		if event.Created != event.Created.Round(0) || event.Next != event.Next.Round(0) {
			t.Fatal("changed observation retained monotonic deadlines absent from the persisted-state behavior")
		}
	}
}
func TestPushWriteFailureCannotClaimRegistration(t *testing.T) {
	p := loadPush(t.TempDir())
	p.path = t.TempDir()
	if err := p.register(PushSubscription{ICCID: "a", DeviceID: "x", Token: strings.Repeat("a", 64), Enabled: true}, nil); err == nil || p.state.Subscription.Enabled {
		t.Fatal("registration without persistence")
	}
}

func TestPushSwitchesRemoveQueuedAlertsAndReenableIgnoresHistory(t *testing.T) {
	p := pushFixture(t)
	messages := []*Message{{Key: "new", Direction: "in"}}
	call := VoiceCall{ID: strings.Repeat("c", 24), Direction: "in", State: "incoming"}
	p.observe(messages, CallSnapshot{Calls: []VoiceCall{call}}, time.Now())
	s := p.state.Subscription
	s.Enabled = false
	if err := p.register(s, messages); err != nil {
		t.Fatal(err)
	}
	if len(p.state.Queue) != 1 || p.state.Queue[0].Kind != "call" {
		t.Fatal("SMS was not removed")
	}
	s.Enabled = true
	p.register(s, messages)
	p.observe(messages, CallSnapshot{Calls: []VoiceCall{call}}, time.Now())
	if len(p.state.Queue) != 1 {
		t.Fatal("reenabling replayed history or duplicated ringing call")
	}
	s.Enabled = false
	s.CallsEnabled = false
	p.register(s, messages)
	if len(p.state.Queue) != 0 {
		t.Fatal("disabled notifications still queued")
	}
}

func TestPushUnknownCardCannotSendAndExpiredCallIsDropped(t *testing.T) {
	p := pushFixture(t)
	now := time.Now()
	p.observe(nil, CallSnapshot{Calls: []VoiceCall{{ID: strings.Repeat("c", 24), Direction: "in", State: "incoming"}}}, now)
	calls := 0
	p.deliver = func(PushDelivery) error { calls++; return nil }
	p.sendNext("", now)
	if calls != 0 || len(p.state.Queue) != 1 {
		t.Fatal("unverified card delivered")
	}
	p.sendNext("card-a", now.Add(21*time.Second))
	if calls != 0 || len(p.state.Queue) != 0 {
		t.Fatal("expired call delivered")
	}
}

func TestPushObservingSIMSwapWithoutEventsRevokesOldRegistration(t *testing.T) {
	p := pushFixture(t)
	p.bindCard("card-b")
	p = loadPush(p.dir)
	p.bindCard("card-a")
	p.observe([]*Message{{Key: "fresh", Direction: "in"}}, CallSnapshot{}, time.Now())
	if p.state.Subscription.ICCID != "" || len(p.state.Queue) != 0 {
		t.Fatal("old subscription revived after swapping back")
	}
}

func TestPushCorruptStateIsPreservedAndTokenRotationKeepsPendingEvent(t *testing.T) {
	p := pushFixture(t)
	p.observe([]*Message{{Key: "fresh", Direction: "in"}}, CallSnapshot{}, time.Now())
	id := p.state.Queue[0].ID
	s := p.state.Subscription
	s.Token = strings.Repeat("f", 64)
	if err := p.register(s, nil); err != nil {
		t.Fatal(err)
	}
	p.deliver = func(d PushDelivery) error {
		if d.Token != s.Token || d.EventID != id {
			t.Fatal("event lost or old token reused")
		}
		return nil
	}
	p.sendNext("card-a", time.Now())
	if len(p.state.Queue) != 0 {
		t.Fatal("accepted event left queued")
	}
	os.WriteFile(p.path, []byte("broken"), 0600)
	p = loadPush(p.dir)
	if p.register(s, nil) == nil {
		t.Fatal("corrupt state overwritten")
	}
	b, _ := os.ReadFile(p.path)
	if string(b) != "broken" {
		t.Fatal("original evidence lost")
	}
}

func TestPushUnchangedObservationAvoidsRepeatedJSONAllocations(t *testing.T) {
	p := pushFixture(t)
	messages := make([]*Message, 300)
	for i := range messages {
		messages[i] = &Message{Key: fmt.Sprintf("synthetic-incoming-%03d", i), Direction: "in"}
	}
	now := time.Now()
	if err := p.observe(messages, CallSnapshot{}, now); err != nil {
		t.Fatal(err)
	}
	allocs := testing.AllocsPerRun(100, func() {
		if err := p.observe(messages, CallSnapshot{}, now); err != nil {
			t.Fatal(err)
		}
	})
	// Hashing the keys may allocate; repeatedly serializing the entire dedupe
	// state must not add a further allocation for every persisted map entry.
	if allocs > 1000 {
		t.Fatalf("unchanged observation allocates %.0f objects; want <= 1000", allocs)
	}
	if len(p.state.Queue) != 1 || p.state.Queue[0].Count != len(messages) {
		t.Fatal("unchanged observation changed the pending alert")
	}
}

func TestPushSnapshotGateMatchesSubscription(t *testing.T) {
	for _, tc := range []struct {
		name          string
		subscription  PushSubscription
		wantSnapshots bool
	}{
		{"unregistered", PushSubscription{Enabled: true}, false},
		{"disabled", PushSubscription{ICCID: "synthetic"}, false},
		{"sms", PushSubscription{ICCID: "synthetic", Enabled: true}, true},
		{"calls", PushSubscription{ICCID: "synthetic", CallsEnabled: true}, true},
	} {
		t.Run(tc.name, func(t *testing.T) {
			p := loadPush(t.TempDir())
			p.state.Subscription = tc.subscription
			messagesRead, callsRead := false, false
			err := p.observeCurrent(func() []pushMessage { messagesRead = true; return nil }, func() CallSnapshot { callsRead = true; return CallSnapshot{} }, time.Now())
			if err != nil {
				t.Fatal(err)
			}
			if messagesRead != tc.wantSnapshots || callsRead != tc.wantSnapshots {
				t.Fatal("snapshot gate differs from notification switches")
			}
		})
	}
}

func TestPushSnapshotGateRechecksConcurrentDisable(t *testing.T) {
	p := pushFixture(t)
	fresh := []*Message{{Key: "synthetic-new", Direction: "in"}}
	err := p.observeCurrent(func() []pushMessage {
		s := p.state.Subscription
		s.Enabled, s.CallsEnabled = false, false
		if err := p.register(s, fresh); err != nil {
			t.Fatal(err)
		}
		return pushRecords(fresh)
	}, func() CallSnapshot {
		return CallSnapshot{Calls: []VoiceCall{{ID: "synthetic-call", Direction: "in", State: "incoming"}}}
	}, time.Now())
	if err != nil {
		t.Fatal(err)
	}
	if len(p.state.Queue) != 0 {
		t.Fatal("disabled subscription queued an alert after snapshotting")
	}
}

func TestPushSnapshotGateEnableKeepsRegistrationBaseline(t *testing.T) {
	p := loadPush(t.TempDir())
	history := []*Message{{Key: "synthetic-old", Direction: "in"}}
	if err := p.observeCurrent(func() []pushMessage { t.Fatal("disabled subscription read messages"); return nil }, func() CallSnapshot { t.Fatal("disabled subscription read calls"); return CallSnapshot{} }, time.Now()); err != nil {
		t.Fatal(err)
	}
	s := PushSubscription{ICCID: "synthetic-card", DeviceID: "synthetic-phone", Token: strings.Repeat("a", 64), Enabled: true}
	if err := p.register(s, history); err != nil {
		t.Fatal(err)
	}
	messages := append(history, &Message{Key: "synthetic-after-enable", Direction: "in"})
	if err := p.observeCurrent(func() []pushMessage { return pushRecords(messages) }, func() CallSnapshot { return CallSnapshot{} }, time.Now()); err != nil {
		t.Fatal(err)
	}
	if len(p.state.Queue) != 1 || p.state.Queue[0].Count != 1 {
		t.Fatal("enabling replayed history or lost the new incoming alert")
	}
}

func TestPushSnapshotGateDoesNotDiscardPendingTest(t *testing.T) {
	p := loadPush(t.TempDir())
	p.state.Queue = []PushEvent{{ID: "synthetic-test", Kind: "test", Created: time.Now(), Next: time.Now()}}
	if err := p.observeCurrent(func() []pushMessage { t.Fatal("disabled subscription read messages"); return nil }, func() CallSnapshot { t.Fatal("disabled subscription read calls"); return CallSnapshot{} }, time.Now()); err != nil {
		t.Fatal(err)
	}
	if !p.pending() {
		t.Fatal("observation gate discarded a pending test notification")
	}
}

func TestPushObservationFailureRestoresDedupeCallsAndQueue(t *testing.T) {
	p := pushFixture(t)
	before, _ := json.Marshal(p.state)
	p.path = t.TempDir()
	err := p.observe([]*Message{{Key: "synthetic-fresh", Direction: "in"}}, CallSnapshot{Calls: []VoiceCall{{ID: "synthetic-call", Direction: "in", State: "incoming"}}}, time.Now())
	if err == nil {
		t.Fatal("observation claimed persistence after a write failure")
	}
	after, _ := json.Marshal(p.state)
	if string(before) != string(after) {
		t.Fatal("failed persistence changed dedupe, call or queue state")
	}
}

func TestPushUnchangedObservationDoesNotWrite(t *testing.T) {
	p := pushFixture(t)
	messages := []*Message{{Key: "synthetic-fresh", Direction: "in"}}
	if err := p.observe(messages, CallSnapshot{}, time.Now()); err != nil {
		t.Fatal(err)
	}
	p.path = t.TempDir()
	if err := p.observe(messages, CallSnapshot{}, time.Now()); err != nil {
		t.Fatal("unchanged observation tried to persist")
	}
}

func TestPushConcurrentSnapshotsAndRegistration(t *testing.T) {
	p := pushFixture(t)
	s := p.state.Subscription
	history := []*Message{{Key: "old", Direction: "in"}}
	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		for i := 0; i < 100; i++ {
			if err := p.observeCurrent(func() []pushMessage { return pushRecords(history) }, func() CallSnapshot { return CallSnapshot{} }, time.Now()); err != nil {
				t.Error(err)
				return
			}
		}
	}()
	for i := 0; i < 20; i++ {
		s.Enabled, s.CallsEnabled = i%2 == 0, i%2 != 0
		if err := p.register(s, history); err != nil {
			t.Fatal(err)
		}
	}
	wg.Wait()
	s.Enabled, s.CallsEnabled = true, true
	if err := p.register(s, history); err != nil {
		t.Fatal(err)
	}
	messages := append(history, &Message{Key: "synthetic-after-registration", Direction: "in"})
	if err := p.observeCurrent(func() []pushMessage { return pushRecords(messages) }, func() CallSnapshot { return CallSnapshot{} }, time.Now()); err != nil {
		t.Fatal(err)
	}
	if len(p.state.Queue) != 1 || p.state.Queue[0].Count != 1 {
		t.Fatal("concurrent registration lost or replayed an incoming message")
	}
}

func TestPushQueueFullLeavesNewMessageRetryable(t *testing.T) {
	p := pushFixture(t)
	for i := 0; i < 32; i++ {
		p.state.Queue = append(p.state.Queue, PushEvent{ID: fmt.Sprintf("synthetic-event-%d", i), Kind: "sms"})
	}
	messages := []*Message{{Key: "synthetic-overflow", Direction: "in"}}
	if err := p.observe(messages, CallSnapshot{}, time.Now()); err != nil {
		t.Fatal(err)
	}
	if len(p.state.Queue) != 32 || p.state.Seen[messagePushHash(messages[0].Key)] {
		t.Fatal("full queue acknowledged an alert it could not enqueue")
	}
	p.state.Queue = nil
	if err := p.observe(messages, CallSnapshot{}, time.Now()); err != nil {
		t.Fatal(err)
	}
	if len(p.state.Queue) != 1 || p.state.Queue[0].Count != 1 {
		t.Fatal("message could not be queued after capacity became available")
	}
}

func TestPushUncertainCallsRetainRingingDedupe(t *testing.T) {
	p := pushFixture(t)
	old := VoiceCall{ID: "synthetic-old-call", Direction: "in", State: "incoming"}
	fresh := VoiceCall{ID: "synthetic-new-call", Direction: "in", State: "incoming"}
	if err := p.observe(nil, CallSnapshot{Calls: []VoiceCall{old}}, time.Now()); err != nil {
		t.Fatal(err)
	}
	if err := p.observe(nil, CallSnapshot{Calls: []VoiceCall{fresh, fresh}, Error: "synthetic uncertain read"}, time.Now()); err != nil {
		t.Fatal(err)
	}
	if len(p.state.Queue) != 2 || !p.state.Calls[old.ID] || !p.state.Calls[fresh.ID] {
		t.Fatal("uncertain call read lost an existing alert or duplicated the fresh call")
	}
	if err := p.observe(nil, CallSnapshot{Error: "synthetic uncertain read"}, time.Now()); err != nil {
		t.Fatal(err)
	}
	if len(p.state.Queue) != 2 {
		t.Fatal("uncertain empty call read invented a hangup")
	}
	if err := p.observe(nil, CallSnapshot{}, time.Now()); err != nil {
		t.Fatal(err)
	}
	if len(p.state.Queue) != 0 || len(p.state.Calls) != 0 {
		t.Fatal("confirmed call end retained stale call alerts")
	}
}

func TestPushEnabledLoopAllocationBudget(t *testing.T) {
	p, s := benchmarkPushState(300, true)
	calls := &CallController{}
	now := time.Unix(1794700800, 0)
	allocs := testing.AllocsPerRun(100, func() {
		if err := p.observeCurrent(s.pushSnapshot, calls.Snapshot, now); err != nil {
			t.Fatal(err)
		}
	})
	if allocs > 400 {
		t.Fatalf("enabled push loop allocates %.0f objects per 300-message observation; want <=400", allocs)
	}
}

func TestMessagePushHashMatchesOriginalBytes(t *testing.T) {
	for _, key := range []string{"", "a", "synthetic-key", strings.Repeat("a", 31), strings.Repeat("a", 32), strings.Repeat("a", 64), strings.Repeat("a", 65), strings.Repeat("a", 1000), "synthetic|中文😀|\x00\r\n"} {
		v := sha256.Sum256([]byte(key))
		want := hex.EncodeToString(v[:])
		if got := messagePushHash(key); got != want {
			t.Fatalf("push hash changed for synthetic key of %d bytes", len(key))
		}
	}
}

func TestMessagePushHashAllocationBudget(t *testing.T) {
	key := "synthetic-message-key"
	allocs := testing.AllocsPerRun(1000, func() { pushBenchmarkHash = messagePushHash(key) })
	if allocs > 1 {
		t.Fatalf("push hash allocates %.0f objects; want <=1", allocs)
	}
}
