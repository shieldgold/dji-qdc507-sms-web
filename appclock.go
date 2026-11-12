package main

import (
	"encoding/json"
	"errors"
	"sync"
	"time"
)

// Use an authenticated browser clock when the modem has no valid wall clock.
// This never changes the modem's system clock or SIM settings.
type AppClock struct {
	mu       sync.Mutex
	anchor   time.Time
	received time.Time
}

var applicationTime AppClock

func (c *AppClock) Now() time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	if !c.anchor.IsZero() {
		return c.anchor.Add(time.Since(c.received))
	}
	return time.Now()
}
func (c *AppClock) Sync(ms int64) error {
	t := time.UnixMilli(ms)
	if t.Year() < 2024 || t.Year() > 2100 {
		return errors.New("设备时间无效")
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	c.anchor = t
	c.received = time.Now()
	return nil
}
func handleClientTime(w *Resp, r *Req) error {
	var v struct {
		UnixMS int64 `json:"unix_ms"`
	}
	if json.Unmarshal(r.Body, &v) != nil {
		return writeJSON(w, 400, map[string]string{"error": "时间格式错误"})
	}
	if err := applicationTime.Sync(v.UnixMS); err != nil {
		return writeJSON(w, 400, map[string]string{"error": err.Error()})
	}
	return writeJSON(w, 200, map[string]bool{"ok": true})
}
