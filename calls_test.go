package main

import (
	"errors"
	"reflect"
	"strings"
	"testing"
	"time"
)

func TestVoiceCallsExcludePacketData(t *testing.T) {
	calls, err := parseVoiceCalls([]string{`+CLCC: 1,0,0,1,0,"",129`, `+CLCC: 2,0,0,1,0,"",129`, `+CLCC: 3,1,4,0,0,"+8613800000000",145`, "OK"})
	if err != nil || len(calls) != 1 || calls[0].Index != 3 || calls[0].State != "incoming" || calls[0].Direction != "in" {
		t.Fatalf("voice call selection = %+v, %v", calls, err)
	}
	if _, err = parseVoiceCalls([]string{`+CLCC: bad,1,4,0,0`, "OK"}); err == nil {
		t.Fatal("malformed call state must not be interpreted as idle")
	}
}

func TestDialRejectsInjectionAndDuplicatesAfterTimeout(t *testing.T) {
	var commands []string
	command := func(s string, _ time.Duration) ([]string, error) {
		commands = append(commands, s)
		if strings.HasPrefix(s, "ATD") {
			return nil, errors.New("timeout")
		}
		return []string{"OK"}, nil
	}
	c := newCallController(command, t.TempDir())
	for _, n := range []string{"", "123;ATH", "10086\rATZ", "*123#", "+123+456"} {
		if _, e := c.Dial(n, "invalid-number"); e == nil {
			t.Fatalf("accepted %q", n)
		}
	}
	if len(commands) != 0 {
		t.Fatal("invalid numbers reached modem")
	}
	_, _ = c.Dial("10086", "dial-once-001")
	_, _ = c.Dial("10086", "dial-once-001")
	count := 0
	for _, s := range commands {
		if strings.HasPrefix(s, "ATD") {
			count++
		}
	}
	if count != 1 {
		t.Fatalf("dial submitted %d times", count)
	}
	// A response lost during a process restart must not cause another dial.
	restarted := newCallController(command, c.dir)
	_, _ = restarted.Dial("10086", "dial-once-001")
	for _, s := range commands {
		if strings.HasPrefix(s, "ATD") {
			count--
		}
	}
	if count != 0 {
		t.Fatal("restart resubmitted a dial")
	}
}

func TestHangupUsesVoiceOnlyCommandAndRejectsStaleID(t *testing.T) {
	lines := []string{`+CLCC: 1,0,0,1,0`, `+CLCC: 3,0,0,0,0,"10086",129`, "OK"}
	var writes []string
	c := newCallController(func(s string, _ time.Duration) ([]string, error) {
		if s == "AT+CLCC" {
			return lines, nil
		}
		writes = append(writes, s)
		return []string{"OK"}, nil
	}, t.TempDir())
	s, e := c.Refresh()
	if e != nil || len(s.Calls) != 1 {
		t.Fatalf("%+v %v", s, e)
	}
	id := s.Calls[0].ID
	if e := c.Hangup("old-id"); e == nil {
		t.Fatal("accepted stale ID")
	}
	if len(writes) != 0 {
		t.Fatal("stale ID changed modem")
	}
	if e := c.Hangup(id); e != nil {
		t.Fatal(e)
	}
	if !reflect.DeepEqual(writes, []string{"AT+CHUP"}) {
		t.Fatalf("unsafe command %v", writes)
	}
	lines = []string{"OK"}
	_, _ = c.Refresh()
	lines = []string{`+CLCC: 3,1,4,0,0,"10086",129`, "OK"}
	s, _ = c.Refresh()
	if s.Calls[0].ID == id {
		t.Fatal("reused call index must receive new ID")
	}
}

func TestCallQueryFailurePreservesActiveState(t *testing.T) {
	fail := false
	c := newCallController(func(string, time.Duration) ([]string, error) {
		if fail {
			return nil, errors.New("offline")
		}
		return []string{`+CLCC: 3,0,0,0,0,"10086",129`, "OK"}, nil
	}, t.TempDir())
	_, _ = c.Refresh()
	fail = true
	s, err := c.Refresh()
	if err == nil || len(s.Calls) != 1 || !c.Busy() || s.Error == "" {
		t.Fatalf("lost active state: %+v %v", s, err)
	}
}

func TestAnswerAndDTMFRequireMatchingCallState(t *testing.T) {
	lines := []string{`+CLCC: 3,1,4,0,0,"10086",129`, "OK"}
	var writes []string
	c := newCallController(func(s string, _ time.Duration) ([]string, error) {
		if s == "AT+CLCC" {
			return lines, nil
		}
		writes = append(writes, s)
		return []string{"OK"}, nil
	}, t.TempDir())
	s, _ := c.Refresh()
	id := s.Calls[0].ID
	if e := c.DTMF(id, "1"); e == nil {
		t.Fatal("DTMF allowed while ringing")
	}
	if e := c.Answer(id); e != nil {
		t.Fatal(e)
	}
	lines = []string{`+CLCC: 3,1,0,0,0,"10086",129`, "OK"}
	if e := c.DTMF(id, "1\rATH"); e == nil {
		t.Fatal("accepted injected DTMF")
	}
	if e := c.DTMF(id, "#"); e != nil {
		t.Fatal(e)
	}
	if !reflect.DeepEqual(writes, []string{"ATA", `AT+VTS="#"`}) {
		t.Fatalf("%v", writes)
	}
}

func TestUncertainDialIsBoundToTheObservedOutgoingCall(t *testing.T) {
	lines := []string{"OK"}
	c := newCallController(func(s string, _ time.Duration) ([]string, error) {
		if strings.HasPrefix(s, "ATD") {
			return nil, errors.New("lost response")
		}
		return lines, nil
	}, t.TempDir())
	_, _ = c.Dial("10086", "request-with-lost-response")
	lines = []string{`+CLCC: 3,0,2,0,0,"10086",129`, "OK"}
	state, _ := c.Refresh()
	if state.Calls[0].RequestID != "request-with-lost-response" {
		t.Fatal("lost dial ownership")
	}
	state, _ = c.Refresh()
	if state.Calls[0].RequestID != "request-with-lost-response" {
		t.Fatal("ownership lost on poll")
	}
	lines = []string{"OK"}
	_, _ = c.Refresh()
	lines = []string{`+CLCC: 3,1,4,0,0,"10086",129`, "OK"}
	state, _ = c.Refresh()
	if state.Calls[0].RequestID != "" {
		t.Fatal("outgoing request adopted an unrelated incoming call")
	}
}
