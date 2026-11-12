package main

import (
	"errors"
	"os"
	"strings"
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
