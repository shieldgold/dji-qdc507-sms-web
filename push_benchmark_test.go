package main

import (
	"fmt"
	"testing"
	"time"
)

// Synthetic messages only: do not load an on-device archive for profiling.
func benchmarkPushState(n int, enabled bool) (*PushService, *Store) {
	s := &Store{Msgs: make(map[string]*Message, n)}
	p := &PushService{state: pushState{Seen: map[string]bool{}, Calls: map[string]bool{}}}
	if enabled {
		p.state.Subscription = PushSubscription{ICCID: "synthetic-card", DeviceID: "synthetic-phone", Enabled: true}
	}
	for i := 0; i < n; i++ {
		key := fmt.Sprintf("synthetic-message-%03d", i)
		s.Msgs[key] = &Message{Key: key, Sender: "synthetic", Body: "synthetic body", Time: fmt.Sprintf("2026-11-15 08:%02d:%02d", i/60, i%60), Direction: "in"}
		p.state.Seen[messagePushHash(key)] = true
	}
	return p, s
}

func BenchmarkPushObserveUnchanged(b *testing.B) {
	for _, n := range []int{0, 10, 100, 300} {
		b.Run(fmt.Sprint(n), func(b *testing.B) {
			p, s := benchmarkPushState(n, true)
			messages := s.List()
			now := time.Unix(1794700800, 0)
			b.ReportAllocs()
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				if err := p.observe(messages, CallSnapshot{}, now); err != nil {
					b.Fatal(err)
				}
			}
		})
	}
}

func BenchmarkPushLoopObservation(b *testing.B) {
	for _, n := range []int{0, 10, 100, 300} {
		for _, enabled := range []bool{false, true} {
			b.Run(fmt.Sprintf("%d/enabled=%t", n, enabled), func(b *testing.B) {
				p, s := benchmarkPushState(n, enabled)
				calls := &CallController{}
				now := time.Unix(1794700800, 0)
				b.ReportAllocs()
				b.ResetTimer()
				for i := 0; i < b.N; i++ {
					if err := p.observeCurrent(s.pushSnapshot, calls.Snapshot, now); err != nil {
						b.Fatal(err)
					}
				}
			})
		}
	}
}

var pushBenchmarkHash string

func BenchmarkMessagePushHash(b *testing.B) {
	b.ReportAllocs()
	for i := 0; i < b.N; i++ {
		pushBenchmarkHash = messagePushHash("synthetic-message-key")
	}
}
