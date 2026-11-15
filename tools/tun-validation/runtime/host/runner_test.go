package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestPrivateStateBackupStoresDataWithoutPuttingItInLogs(t *testing.T) {
	data := []byte(`{"synthetic_secret":"STATE_BACKUP_TEST_ONLY"}` + "\n")
	digest := sha256.Sum256(data)
	var q request
	err := json.Unmarshal([]byte(fmt.Sprintf(`{"op":"backup","id":"state","seconds":15,"remote":"/data/smsweb/auth.json","expected_bytes":%d,"expected_sha256":"%x"}`, len(data), digest)), &q)
	if err != nil {
		t.Fatal(err)
	}
	root := t.TempDir()
	f := &fakeWire{}
	f.service = func(s string, f *fakeWire) {
		if s != "shell:exec /bin/busybox base64 /data/smsweb/auth.json" {
			t.Fatalf("unsafe backup command %q", s)
		}
		f.enqueue(okay, 7, 1, nil)
		encoded := base64.StdEncoding.EncodeToString(data) + "\r\n"
		f.enqueue(wrte, 7, 1, []byte(encoded[:5]))
		f.enqueue(wrte, 7, 1, []byte(encoded[5:]))
		f.enqueue(clse, 7, 1, nil)
	}
	r := runRequest(newClient(f), q, "R1", root)
	if r.Status != "ok" {
		t.Fatalf("%+v", r)
	}
	b, err := os.ReadFile(filepath.Join(root, "state.backup.b64"))
	if err != nil {
		t.Fatal(err)
	}
	got, err := base64.StdEncoding.DecodeString(string(b))
	if err != nil || !bytes.Equal(got, data) {
		t.Fatalf("backup mismatch %v", err)
	}
	log, _ := os.ReadFile(filepath.Join(root, "state.raw"))
	if r.Tail != "" || bytes.Contains(log, []byte("STATE_BACKUP_TEST_ONLY")) || bytes.Contains(log, []byte(base64.StdEncoding.EncodeToString(data))) {
		t.Fatal("private state was exposed in stage logs")
	}
	st, _ := os.Stat(filepath.Join(root, "state.backup.b64"))
	if st.Mode().Perm() != 0600 {
		t.Fatal("non-private backup permissions")
	}
}

func TestPrivateStateBackupRefusesUnreviewedPathsAndHashes(t *testing.T) {
	for _, remote := range []string{"/data/smsweb/client.key", "/data/smsweb/auth.json;id", "/etc/passwd"} {
		var q request
		json.Unmarshal([]byte(fmt.Sprintf(`{"op":"backup","id":"state","seconds":15,"remote":%q,"expected_bytes":10,"expected_sha256":"%s"}`, remote, strings.Repeat("a", 64))), &q)
		f := &fakeWire{}
		r := runRequest(newClient(f), q, "R1", t.TempDir())
		if r.Status == "ok" || f.opens != 0 {
			t.Fatalf("unsafe backup request: %+v", r)
		}
	}
	data := []byte("SYNTHETIC_PRIVATE_DATA")
	var q request
	json.Unmarshal([]byte(fmt.Sprintf(`{"op":"backup","id":"state","seconds":15,"remote":"/data/smsweb/auth.json","expected_bytes":%d,"expected_sha256":"%s"}`, len(data), strings.Repeat("0", 64))), &q)
	f := &fakeWire{}
	f.service = func(_ string, f *fakeWire) {
		f.enqueue(okay, 7, 1, nil)
		f.enqueue(wrte, 7, 1, []byte(base64.StdEncoding.EncodeToString(data)))
		f.enqueue(clse, 7, 1, nil)
	}
	root := t.TempDir()
	r := runRequest(newClient(f), q, "R1", root)
	if r.Status == "ok" || r.Tail != "" {
		t.Fatalf("unverified backup accepted: %+v", r)
	}
	log, _ := os.ReadFile(filepath.Join(root, "state.raw"))
	if bytes.Contains(log, data) {
		t.Fatal("private data in log")
	}
}

func TestPrivateStateBackupCannotOverwriteExistingBackup(t *testing.T) {
	root := t.TempDir()
	file := filepath.Join(root, "state.backup.b64")
	os.WriteFile(file, []byte("keep-existing"), 0600)
	var q request
	json.Unmarshal([]byte(fmt.Sprintf(`{"op":"backup","id":"state","seconds":15,"remote":"/data/smsweb/auth.json","expected_bytes":10,"expected_sha256":"%s"}`, strings.Repeat("a", 64))), &q)
	f := &fakeWire{}
	r := runRequest(newClient(f), q, "R1", root)
	got, _ := os.ReadFile(file)
	if r.Status == "ok" || f.opens != 0 || string(got) != "keep-existing" {
		t.Fatalf("overwrote prior backup: %+v", r)
	}
}

func TestProgramBackupKeepsExactDeploymentPrivately(t *testing.T) {
	for _, remote := range []string{"/data/smsweb/smsweb", "/data/smsweb/start.sh"} {
		data := []byte("PROGRAM_BACKUP_SYNTHETIC_ONLY\n")
		digest := sha256.Sum256(data)
		q := request{Op: "backup-program", ID: "program", Seconds: 60, Remote: remote,
			ExpectedBytes: len(data), ExpectedSHA256: fmt.Sprintf("%x", digest)}
		f := &fakeWire{}
		f.service = func(s string, f *fakeWire) {
			if s != "shell:exec /bin/busybox base64 "+remote {
				t.Fatalf("unsafe service %q", s)
			}
			f.enqueue(okay, 7, 1, nil)
			f.enqueue(wrte, 7, 1, []byte(base64.StdEncoding.EncodeToString(data)))
			f.enqueue(clse, 7, 1, nil)
		}
		root := t.TempDir()
		r := runRequest(newClient(f), q, "R1", root)
		if r.Status != "ok" {
			t.Fatalf("%s: %+v", remote, r)
		}
		b, err := os.ReadFile(filepath.Join(root, "program.backup.b64"))
		if err != nil {
			t.Fatal(err)
		}
		decoded, err := base64.StdEncoding.DecodeString(string(b))
		if err != nil || !bytes.Equal(decoded, data) {
			t.Fatalf("wrong backup %v", err)
		}
		log, _ := os.ReadFile(filepath.Join(root, "program.raw"))
		if r.Tail != "" || bytes.Contains(log, data) || bytes.Contains(log, b) {
			t.Fatal("program payload in logs")
		}
	}
}

func TestProgramBackupRejectsOtherPathsBoundsAndFields(t *testing.T) {
	base := request{Op: "backup-program", ID: "program", Seconds: 60, Remote: "/data/smsweb/smsweb",
		ExpectedBytes: 10, ExpectedSHA256: strings.Repeat("a", 64)}
	cases := []request{}
	for _, path := range []string{"/data/smsweb/auth.json", "/data/smsweb/client.key", "/data/smsweb/previous/smsweb", "/data/smsweb/smsweb;id"} {
		q := base
		q.Remote = path
		cases = append(cases, q)
	}
	q := base
	q.ExpectedBytes = (8 << 20) + 1
	cases = append(cases, q)
	q = base
	q.Remote = "/data/smsweb/start.sh"
	q.ExpectedBytes = (64 << 10) + 1
	cases = append(cases, q)
	q = base
	q.Seconds = 15
	cases = append(cases, q)
	q = base
	q.Command = "true"
	cases = append(cases, q)
	for _, q := range cases {
		f := &fakeWire{}
		r := runRequest(newClient(f), q, "R1", t.TempDir())
		if r.Status == "ok" || f.opens != 0 {
			t.Fatalf("unreviewed program backup: %+v", r)
		}
	}
}

func admissionRequest(t *testing.T, threshold int) request {
	t.Helper()
	var q request
	err := json.Unmarshal([]byte(fmt.Sprintf(`{"op":"admission","id":"gate","seconds":25,"min_available_kib":%d}`, threshold)), &q)
	if err != nil {
		t.Fatal(err)
	}
	return q
}

func TestAdmissionDirectExecPreservesFramedEvidence(t *testing.T) {
	f := &fakeWire{}
	f.service = func(s string, f *fakeWire) {
		want := "shell:exec /run/qdc-tun-R1/qdc-ike-guard admit-client 12288 R1 gate"
		if s != want {
			t.Fatalf("unexpected native service %q", s)
		}
		f.enqueue(okay, 7, 1, nil)
		f.enqueue(wrte, 7, 1, []byte("__QDC_R1_gate_BEGIN__\n"+
			"MEM_GATE available_kib=13000 required_kib=12288 sample=1 elapsed=0.001 pid=23\n"+
			"MEM_GATE available_kib=12900 required_kib=12288 sample=2 elapsed=5.010 pid=23\n"+
			"MEM_GATE available_kib=12800 required_kib=12288 sample=3 elapsed=10.020 pid=23\n"+
			"CLIENT_ADMISSION_READY samples=3 interval_seconds=5 required_kib=12288\n"+
			"__QDC_R1_gate_END__ RC=0\n"))
		f.enqueue(clse, 7, 1, nil)
	}
	r := runRequest(newClient(f), admissionRequest(t, 12288), "R1", t.TempDir())
	if r.Status != "ok" || r.RemoteRC == nil || *r.RemoteRC != 0 || f.opens != 1 {
		t.Fatalf("%+v", r)
	}
}

func TestAdmissionRejectsSideEffectsAndUnsupportedLimitsBeforeUSB(t *testing.T) {
	for _, threshold := range []int{0, 6144, 12020, 12289} {
		f := &fakeWire{}
		r := runRequest(newClient(f), admissionRequest(t, threshold), "R1", t.TempDir())
		if r.Status == "ok" || f.opens != 0 {
			t.Fatalf("%+v", r)
		}
	}
	q := admissionRequest(t, 12288)
	q.Command = "touch /data/forbidden"
	f := &fakeWire{}
	r := runRequest(newClient(f), q, "R1", t.TempDir())
	if r.Status == "ok" || f.opens != 0 {
		t.Fatalf("%+v", r)
	}
}

func TestAdmissionMissingSamplesCannotPassWithZeroRC(t *testing.T) {
	f := &fakeWire{}
	f.service = func(_ string, f *fakeWire) {
		f.enqueue(okay, 7, 1, nil)
		f.enqueue(wrte, 7, 1, []byte("__QDC_R1_gate_END__ RC=0\n"))
		f.enqueue(clse, 7, 1, nil)
	}
	r := runRequest(newClient(f), admissionRequest(t, 12288), "R1", t.TempDir())
	if r.Status == "ok" {
		t.Fatalf("incomplete native evidence accepted: %+v", r)
	}
}

func TestAdmissionEvidenceRefusesChangedLimitsIdentityAndTiming(t *testing.T) {
	base := "__QDC_R1_gate_BEGIN__\n" +
		"MEM_GATE available_kib=13000 required_kib=12288 sample=1 elapsed=0.001 pid=23\n" +
		"MEM_GATE available_kib=12900 required_kib=12288 sample=2 elapsed=5.010 pid=23\n" +
		"MEM_GATE available_kib=12800 required_kib=12288 sample=3 elapsed=10.020 pid=23\n" +
		"CLIENT_ADMISSION_READY samples=3 interval_seconds=5 required_kib=12288\n"
	for name, text := range map[string]string{
		"low_memory":      strings.Replace(base, "available_kib=12900", "available_kib=12020", 1),
		"limit":           strings.Replace(base, "required_kib=12288", "required_kib=11776", 1),
		"interval":        strings.Replace(base, "elapsed=5.010", "elapsed=1.000", 1),
		"pid":             strings.Replace(base, "elapsed=5.010 pid=23", "elapsed=5.010 pid=24", 1),
		"nan":             strings.Replace(base, "elapsed=5.010", "elapsed=NaN", 1),
		"order":           strings.Replace(base, "sample=2", "sample=3", 1),
		"missing_begin":   strings.Replace(base, "__QDC_R1_gate_BEGIN__\n", "", 1),
		"duplicate_ready": base + "CLIENT_ADMISSION_READY samples=3 interval_seconds=5 required_kib=12288\n",
	} {
		t.Run(name, func(t *testing.T) {
			if admissionEvidence(text, "__QDC_R1_gate_BEGIN__", 12288) == nil {
				t.Fatal("unsafe native evidence accepted")
			}
		})
	}
	if err := admissionEvidence(strings.ReplaceAll(base, "\n", "\r\n"), "__QDC_R1_gate_BEGIN__", 12288); err != nil {
		t.Fatal(err)
	}
}

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
