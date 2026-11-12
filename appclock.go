package main

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"sync"
	"time"
)

// Use an authenticated browser clock when the modem has no valid wall clock.
// This never changes the modem's system clock or SIM settings.
type AppClock struct {
	mu       sync.Mutex
	anchor   time.Time
	received time.Time
	path     string
}

type savedAppClock struct {
	UTC    time.Time `json:"utc"`
	System time.Time `json:"system"`
}

// Retain a TLS-valid time estimate after a process/module restart. The push
// sender refreshes this from Apple's TLS-authenticated Date header.
func (c *AppClock) Load(dir string) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.path = filepath.Join(dir, "clock.json")
	var saved savedAppClock
	b, e := os.ReadFile(c.path)
	if e != nil || json.Unmarshal(b, &saved) != nil || saved.UTC.Year() < 2024 || saved.UTC.Year() > 2100 {
		return
	}
	now := time.Now()
	elapsed := now.Sub(saved.System)
	if elapsed < 0 || elapsed > 30*24*time.Hour {
		elapsed = 0
	}
	c.anchor = saved.UTC.Add(elapsed)
	c.received = now
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
	if c.path != "" {
		b, _ := json.Marshal(savedAppClock{UTC: t, System: time.Now()})
		if err := atomicWrite(c.path, b); err != nil {
			return errors.New("通知时间未能保存，请重试")
		}
	}
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
