package main

import (
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"
)

type request struct {
	Op             string  `json:"op"`
	ID             string  `json:"id"`
	Command        string  `json:"command"`
	Seconds        float64 `json:"seconds"`
	Ready          string  `json:"ready"`
	Local          string  `json:"local"`
	Remote         string  `json:"remote"`
	MinimumKiB     int     `json:"min_available_kib,omitempty"`
	ExpectedBytes  int     `json:"expected_bytes,omitempty"`
	ExpectedSHA256 string  `json:"expected_sha256,omitempty"`
}
type result struct {
	Status   string `json:"status"`
	RemoteRC *int   `json:"remote_rc,omitempty"`
	Error    string `json:"error,omitempty"`
	ID       string `json:"id"`
	Bytes    int    `json:"bytes"`
	Tail     string `json:"tail,omitempty"`
}

var safeID = regexp.MustCompile(`^[A-Za-z0-9_-]{1,96}$`)
var digest256 = regexp.MustCompile(`^[0-9a-f]{64}$`)

func admissionEvidence(tail, begin string, required int) error {
	count, begins, ready, pid := 0, 0, 0, 0
	last := 0.0
	for _, raw := range strings.Split(tail, "\n") {
		line := strings.TrimSpace(raw)
		if line == begin {
			begins++
		}
		if line == fmt.Sprintf("CLIENT_ADMISSION_READY samples=3 interval_seconds=5 required_kib=%d", required) {
			ready++
		}
		if !strings.HasPrefix(line, "MEM_GATE ") {
			continue
		}
		var available, limit, sample, process int
		var elapsed float64
		n, err := fmt.Sscanf(line, "MEM_GATE available_kib=%d required_kib=%d sample=%d elapsed=%f pid=%d", &available, &limit, &sample, &elapsed, &process)
		if err != nil || n != 5 || len(strings.Fields(line)) != 6 || available < required || limit != required ||
			sample != count+1 || process < 1 || (pid != 0 && pid != process) ||
			math.IsNaN(elapsed) || math.IsInf(elapsed, 0) || elapsed < 0 || (count > 0 && elapsed-last < 4.998) {
			return fmt.Errorf("native admission sample, interval or identity mismatch")
		}
		pid = process
		last = elapsed
		count++
	}
	if count != 3 || begins != 1 || ready != 1 {
		return fmt.Errorf("native admission evidence incomplete")
	}
	return nil
}

func runRequest(c *client, q request, run, dir string) (r result) {
	r.ID = q.ID
	r.Status = "unknown"
	fail := func(e error) result { r.Error = e.Error(); return r }
	if !safeID.MatchString(q.ID) || !safeID.MatchString(run) || q.Seconds <= 0 || q.Seconds > 90 {
		return fail(fmt.Errorf("invalid stage identity or deadline"))
	}
	f, e := os.OpenFile(filepath.Join(dir, q.ID+".raw"), os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if e != nil {
		return fail(e)
	}
	defer f.Close()
	if q.Op == "push" {
		b, e := os.ReadFile(q.Local)
		if e != nil {
			return fail(e)
		}
		if e = c.push(q.Remote, b, time.Duration(q.Seconds*float64(time.Second))); e != nil {
			return fail(e)
		}
		hash := sha256.Sum256(b)
		record := map[string]any{"remote": q.Remote, "bytes": len(b), "sha256": hex.EncodeToString(hash[:])}
		raw, _ := json.Marshal(record)
		if _, e = f.Write(raw); e != nil {
			return fail(e)
		}
		if e = f.Sync(); e != nil {
			return fail(e)
		}
		r.Status = "ok"
		r.Bytes = len(b)
		return r
	}
	if q.Op == "backup" || q.Op == "backup-program" {
		allowed := map[string]bool{"auth.json": true, "messages.json": true, "push-state.json": true,
			"call-attempts.json": true, "clock.json": true, "traffic.json": true}
		name := strings.TrimPrefix(q.Remote, "/data/smsweb/")
		maximum, encodedMaximum, seconds := 256<<10, 1<<20, 15.0
		if q.Op == "backup-program" {
			allowed = map[string]bool{"smsweb": true, "start.sh": true}
			maximum, encodedMaximum, seconds = 8<<20, 12<<20, 60
			if name == "start.sh" {
				maximum = 64 << 10
			}
		}
		if !allowed[name] || q.Remote != "/data/smsweb/"+name || q.Seconds != seconds ||
			q.ExpectedBytes < 1 || q.ExpectedBytes > maximum || !digest256.MatchString(q.ExpectedSHA256) ||
			q.Command != "" || q.Ready != "" || q.Local != "" || q.MinimumKiB != 0 {
			return fail(fmt.Errorf("restricted private state backup path, digest, size or fields"))
		}
		backupName := q.ID + ".backup.b64"
		backupPath := filepath.Join(dir, backupName)
		backup, err := os.OpenFile(backupPath, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
		if err != nil {
			return fail(err)
		}
		defer backup.Close()
		err = c.shell("exec /bin/busybox base64 "+q.Remote, time.Duration(q.Seconds*float64(time.Second)), func(p []byte) error {
			r.Bytes += len(p)
			if r.Bytes > encodedMaximum {
				return fmt.Errorf("private backup transport bound exceeded")
			}
			if _, err := backup.Write(p); err != nil {
				return err
			}
			return backup.Sync()
		}, "", "")
		if err != nil {
			return fail(err)
		}
		if err = backup.Close(); err != nil {
			return fail(err)
		}
		encoded, err := os.ReadFile(backupPath)
		if err != nil {
			return fail(err)
		}
		data, err := base64.StdEncoding.DecodeString(string(encoded))
		if err != nil {
			return fail(fmt.Errorf("private backup encoding invalid"))
		}
		digest := sha256.Sum256(data)
		if len(data) != q.ExpectedBytes || hex.EncodeToString(digest[:]) != q.ExpectedSHA256 {
			return fail(fmt.Errorf("private backup size or SHA-256 mismatch"))
		}
		directory, err := os.Open(dir)
		if err != nil {
			return fail(err)
		}
		err = directory.Sync()
		directory.Close()
		if err != nil {
			return fail(err)
		}
		meta, err := json.Marshal(map[string]any{"remote": q.Remote, "backup_file": backupName,
			"encoding": "base64", "bytes": len(data), "sha256": q.ExpectedSHA256, "encoded_bytes": len(encoded)})
		if err != nil {
			return fail(err)
		}
		if _, err = f.Write(meta); err != nil {
			return fail(err)
		}
		if err = f.Sync(); err != nil {
			return fail(err)
		}
		r.Bytes = len(data)
		r.Status = "ok"
		return r
	}
	end := "__QDC_" + run + "_" + q.ID + "_END__"
	begin := "__QDC_" + run + "_" + q.ID + "_BEGIN__"
	var command string
	if q.Op == "admission" {
		if (q.MinimumKiB != 12288 && q.MinimumKiB != 11776) || q.Seconds != 25 || len(run) > 48 || len(q.ID) > 48 ||
			q.Command != "" || q.Ready != "" || q.Local != "" || q.Remote != "" || q.ExpectedBytes != 0 || q.ExpectedSHA256 != "" {
			return fail(fmt.Errorf("restricted native admission identity, limit or fields"))
		}
		command = fmt.Sprintf("exec /run/qdc-tun-%s/qdc-ike-guard admit-client %d %s %s", run, q.MinimumKiB, run, q.ID)
	} else if q.Op == "shell" && strings.IndexByte(q.Command, 0) < 0 && len(q.Command) <= 3300 {
		command = "printf '%s\\n' '" + begin + "'; (\n" + q.Command + "\n); qdc_remote_status=$?; printf '\\n" + end + " RC=%d\\n' \"$qdc_remote_status\""
	} else {
		return fail(fmt.Errorf("unsupported operation or excessive command"))
	}
	var tail string
	e = c.shell(command, time.Duration(q.Seconds*float64(time.Second)), func(p []byte) error {
		n, e := f.Write(p)
		r.Bytes += n
		if e != nil {
			return e
		}
		if e = f.Sync(); e != nil {
			return e
		}
		tail += string(p)
		if len(tail) > 8192 {
			tail = tail[len(tail)-8192:]
		}
		if r.Bytes > 8<<20 {
			return fmt.Errorf("stage log budget exceeded after retaining received data")
		}
		return nil
	}, q.Ready, "GO\n")
	r.Tail = tail
	if len(r.Tail) > 1600 {
		r.Tail = r.Tail[len(r.Tail)-1600:]
	}
	if e != nil {
		return fail(e)
	}
	rc, e := remoteStatus(tail, end)
	if e != nil {
		return fail(e)
	}
	r.RemoteRC = &rc
	if rc == 0 && q.Op == "admission" {
		if e = admissionEvidence(tail, begin, q.MinimumKiB); e != nil {
			return fail(e)
		}
	}
	if rc != 0 {
		r.Status = "failed"
		r.Error = fmt.Sprintf("remote exit %d", rc)
	} else {
		r.Status = "ok"
	}
	return r
}
