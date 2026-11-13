package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"
)

type request struct {
	Op      string  `json:"op"`
	ID      string  `json:"id"`
	Command string  `json:"command"`
	Seconds float64 `json:"seconds"`
	Ready   string  `json:"ready"`
	Local   string  `json:"local"`
	Remote  string  `json:"remote"`
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
	if q.Op != "shell" || strings.IndexByte(q.Command, 0) >= 0 || len(q.Command) > 3300 {
		return fail(fmt.Errorf("unsupported operation or excessive command"))
	}
	end := "__QDC_" + run + "_" + q.ID + "_END__"
	begin := "__QDC_" + run + "_" + q.ID + "_BEGIN__"
	command := "printf '%s\\n' '" + begin + "'; (\n" + q.Command + "\n); qdc_remote_status=$?; printf '\\n" + end + " RC=%d\\n' \"$qdc_remote_status\""
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
	if rc != 0 {
		r.Status = "failed"
		r.Error = fmt.Sprintf("remote exit %d", rc)
	} else {
		r.Status = "ok"
	}
	return r
}
