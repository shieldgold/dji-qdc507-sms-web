package main

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestRunnerRecordsRemoteNonzeroAsFailure(t *testing.T) {
	root := t.TempDir()
	f := &fakeWire{}
	f.service = func(s string, f *fakeWire) {
		if !strings.HasPrefix(s, "shell:") {
			t.Fatalf("service %q", s)
		}
		f.enqueue(okay, 7, 1, nil)
		f.enqueue(wrte, 7, 1, []byte("before\n__QDC_R1_fail_END__ RC=23\n"))
		f.enqueue(clse, 7, 1, nil)
	}
	result := runRequest(newClient(f), request{Op: "shell", ID: "fail", Command: "exit 23", Seconds: 1}, "R1", root)
	if result.Status != "failed" || result.RemoteRC == nil || *result.RemoteRC != 23 {
		t.Fatalf("%+v", result)
	}
	b, e := os.ReadFile(filepath.Join(root, "fail.raw"))
	if e != nil || !bytes.Contains(b, []byte("before")) {
		t.Fatalf("%q %v", b, e)
	}
}
func TestRunnerKeepsFileOnTimeout(t *testing.T) {
	root := t.TempDir()
	f := &fakeWire{}
	f.service = func(_ string, f *fakeWire) { f.enqueue(okay, 7, 1, nil); f.enqueue(wrte, 7, 1, []byte("persist-me")) }
	result := runRequest(newClient(f), request{Op: "shell", ID: "timeout", Command: "hang", Seconds: 0.03}, "R1", root)
	b, e := os.ReadFile(filepath.Join(root, "timeout.raw"))
	if result.Status != "unknown" || e != nil || string(b) != "persist-me" {
		t.Fatalf("%+v file=%q err=%v", result, b, e)
	}
}
func TestRunnerNeverOverwritesPriorStage(t *testing.T) {
	root := t.TempDir()
	os.WriteFile(filepath.Join(root, "duplicate.raw"), []byte("old evidence"), 0600)
	f := &fakeWire{}
	result := runRequest(newClient(f), request{Op: "shell", ID: "duplicate", Command: "true", Seconds: 1}, "R1", root)
	b, _ := os.ReadFile(filepath.Join(root, "duplicate.raw"))
	if result.Status == "ok" || string(b) != "old evidence" || f.opens != 0 {
		t.Fatalf("%+v file=%q", result, b)
	}
}
