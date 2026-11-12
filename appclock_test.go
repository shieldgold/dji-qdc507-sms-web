package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestApplicationClockPersistsAndReloadsAcrossServiceRestart(t *testing.T) {
	dir := t.TempDir()
	var first AppClock
	first.Load(dir)
	want := time.Date(2026, 11, 13, 3, 0, 0, 0, time.UTC)
	if err := first.Sync(want.UnixMilli()); err != nil {
		t.Fatal(err)
	}
	var next AppClock
	next.Load(dir)
	if delta := next.Now().Sub(want); delta < 0 || delta > time.Second {
		t.Fatalf("lost calibration: %v", delta)
	}
	info, err := os.Stat(filepath.Join(dir, "clock.json"))
	if err != nil || info.Mode().Perm() != 0600 {
		t.Fatal("unsafe clock state")
	}
}

func TestApplicationClockHandlesModemClockResetWithoutInventingFutureYears(t *testing.T) {
	dir := t.TempDir()
	want := time.Now().UTC()
	b, _ := json.Marshal(savedAppClock{UTC: want, System: time.Now().Add(100 * 24 * time.Hour)})
	os.WriteFile(filepath.Join(dir, "clock.json"), b, 0600)
	var c AppClock
	c.Load(dir)
	if delta := c.Now().Sub(want); delta < 0 || delta > time.Second {
		t.Fatalf("invalid reboot estimate %v", delta)
	}
	if c.Sync(time.Date(1970, 1, 1, 0, 0, 0, 0, time.UTC).UnixMilli()) == nil {
		t.Fatal("accepted invalid clock")
	}
}
