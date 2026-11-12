package main

import (
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestAtomicWritePreservesOriginalOnFailure(t *testing.T) {
	dir := t.TempDir()
	p := filepath.Join(dir, "state.json")
	if e := atomicWrite(p, []byte("old")); e != nil {
		t.Fatal(e)
	}
	if e := os.Mkdir(p+".tmp", 0700); e != nil {
		t.Fatal(e)
	}
	if e := atomicWrite(p, []byte("new")); e == nil {
		t.Fatal("failed to reject occupied temporary file")
	}
	b, _ := os.ReadFile(p)
	if string(b) != "old" {
		t.Fatal("original corrupted")
	}
}
func TestClientClockValidatesAndAdvances(t *testing.T) {
	var c AppClock
	if e := c.Sync(time.Date(1980, 1, 1, 0, 0, 0, 0, time.UTC).UnixMilli()); e == nil {
		t.Fatal("invalid time accepted")
	}
	ref := time.Date(2026, 11, 12, 15, 1, 0, 0, time.UTC)
	if e := c.Sync(ref.UnixMilli()); e != nil {
		t.Fatal(e)
	}
	if delta := c.Now().Sub(ref); delta < 0 || delta > time.Second {
		t.Fatal("clock not anchored")
	}
}
