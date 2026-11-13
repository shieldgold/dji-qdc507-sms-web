package main

import (
	"fmt"
	"time"
)

// Transfer metadata only: ADB payloads remain in the separate stage logs.
type transferEvent struct {
	Time        string `json:"time"`
	Operation   string `json:"operation"`
	Requested   int    `json:"requested"`
	Transferred int    `json:"transferred"`
	DeadlineMS  int64  `json:"deadline_ms,omitempty"`
	ElapsedUS   int64  `json:"elapsed_us"`
	Error       string `json:"error,omitempty"`
}

type tracedTransport struct {
	transport
	save func(transferEvent) error
}

func (t *tracedTransport) record(operation string, start time.Time, requested, transferred int, deadline time.Duration, err error) error {
	e := transferEvent{Time: start.Format(time.RFC3339Nano), Operation: operation, Requested: requested, Transferred: transferred, DeadlineMS: deadline.Milliseconds(), ElapsedUS: time.Since(start).Microseconds()}
	if err != nil {
		e.Error = err.Error()
	}
	if saveErr := t.save(e); saveErr != nil {
		return fmt.Errorf("persist USB transfer metadata: %v; transport error: %w", saveErr, err)
	}
	return err
}

func (t *tracedTransport) Read(p []byte, d time.Duration) (int, error) {
	start := time.Now()
	n, err := t.transport.Read(p, d)
	return n, t.record("read", start, len(p), n, d, err)
}

func (t *tracedTransport) Write(p []byte) error {
	start := time.Now()
	err := t.transport.Write(p)
	n := len(p)
	if err != nil {
		n = 0
	}
	return t.record("write", start, len(p), n, 0, err)
}
