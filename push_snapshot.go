package main

// pushMessage holds only immutable string values needed for push dedupe/counts.
// It never exposes a pointer to a mutable Store message.
type pushMessage struct {
	key       string
	direction string
}

// pushSnapshot copies values while holding the Store read lock. Push observation
// uses sets/counts, so sorting and copying message bodies/statuses are unnecessary.
func (s *Store) pushSnapshot() []pushMessage {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([]pushMessage, 0, len(s.Msgs))
	for _, m := range s.Msgs {
		out = append(out, pushMessage{key: m.Key, direction: m.Direction})
	}
	return out
}

func pushRecords(messages []*Message) []pushMessage {
	out := make([]pushMessage, len(messages))
	for i, m := range messages {
		out[i] = pushMessage{key: m.Key, direction: m.Direction}
	}
	return out
}
