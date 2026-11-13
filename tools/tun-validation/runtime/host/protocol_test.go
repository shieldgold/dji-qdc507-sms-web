package main

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"sync"
	"testing"
	"time"
)

type fakeWire struct {
	mu        sync.Mutex
	rx        []byte
	pending   []byte
	service   func(string, *fakeWire)
	opens     int
	writes    [][]byte
	broken    bool
	dataHook  func([]byte, *fakeWire)
	syncClose bool
}

func frame(cmd, a, b uint32, p []byte) []byte {
	out := make([]byte, 24+len(p))
	for i, v := range []uint32{cmd, a, b, uint32(len(p)), sum(p), cmd ^ 0xffffffff} {
		binary.LittleEndian.PutUint32(out[i*4:], v)
	}
	copy(out[24:], p)
	return out
}
func (f *fakeWire) enqueue(cmd, a, b uint32, p []byte) { f.rx = append(f.rx, frame(cmd, a, b, p)...) }
func (f *fakeWire) Write(p []byte) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.writes = append(f.writes, append([]byte{}, p...))
	f.pending = append(f.pending, p...)
	for len(f.pending) >= 24 {
		h := f.pending[:24]
		n := int(binary.LittleEndian.Uint32(h[12:]))
		if len(f.pending) < 24+n {
			break
		}
		cmd := binary.LittleEndian.Uint32(h)
		a := binary.LittleEndian.Uint32(h[4:])
		b := binary.LittleEndian.Uint32(h[8:])
		data := append([]byte{}, f.pending[24:24+n]...)
		f.pending = f.pending[24+n:]
		switch cmd {
		case cnxn:
			f.enqueue(cnxn, 0, 4096, []byte("device::"))
		case open:
			f.opens++
			f.service(string(bytes.TrimRight(data, "\x00")), f)
		case wrte:
			f.enqueue(okay, b, a, nil)
			if f.dataHook != nil {
				f.dataHook(data, f)
			}
		case clse:
			if f.syncClose {
				f.enqueue(clse, b, a, nil)
			}
		}
	}
	return nil
}
func (f *fakeWire) Read(p []byte, d time.Duration) (int, error) {
	end := time.Now().Add(d)
	for {
		f.mu.Lock()
		if len(f.rx) > 0 {
			n := copy(p, f.rx)
			f.rx = f.rx[n:]
			f.mu.Unlock()
			return n, nil
		}
		broken := f.broken
		f.mu.Unlock()
		if broken {
			return 0, io.EOF
		}
		if time.Now().After(end) {
			return 0, nil
		}
		time.Sleep(time.Millisecond)
	}
}
func (f *fakeWire) Close() error { return nil }

func TestOutputDeliveredBeforeRemoteCloses(t *testing.T) {
	f := &fakeWire{}
	f.service = func(_ string, f *fakeWire) { f.enqueue(okay, 7, 1, nil); f.enqueue(wrte, 7, 1, []byte("early")) }
	c := newClient(f)
	var got bytes.Buffer
	err := c.shell("true", 100*time.Millisecond, func(p []byte) error { got.Write(p); f.mu.Lock(); f.enqueue(clse, 7, 1, nil); f.mu.Unlock(); return nil }, "", "")
	if err != nil || got.String() != "early" {
		t.Fatalf("stream before close: %q %v", got.String(), err)
	}
}
func TestTimeoutRetainsPartialOutputWithoutRetry(t *testing.T) {
	f := &fakeWire{}
	f.service = func(_ string, f *fakeWire) {
		f.enqueue(okay, 7, 1, nil)
		f.enqueue(wrte, 7, 1, []byte("before-timeout"))
	}
	c := newClient(f)
	var got bytes.Buffer
	err := c.shell("hang", 30*time.Millisecond, func(p []byte) error { _, e := got.Write(p); return e }, "", "")
	if !errors.Is(err, errTimeout) || got.String() != "before-timeout" || f.opens != 1 {
		t.Fatalf("partial=%q opens=%d err=%v", got.String(), f.opens, err)
	}
	if err = c.shell("retry", time.Second, func([]byte) error { return nil }, "", ""); err == nil || f.opens != 1 {
		t.Fatalf("failed channel reused: %v opens=%d", err, f.opens)
	}
}
func TestDisconnectPreservesPartial(t *testing.T) {
	f := &fakeWire{}
	f.service = func(_ string, f *fakeWire) {
		f.enqueue(okay, 7, 1, nil)
		f.enqueue(wrte, 7, 1, []byte("last"))
		f.broken = true
	}
	var b bytes.Buffer
	err := newClient(f).shell("x", time.Second, func(p []byte) error { b.Write(p); return nil }, "", "")
	if err == nil || b.String() != "last" {
		t.Fatalf("%q %v", b.String(), err)
	}
}
func TestGateIsSentOnlyAfterSuccessfulWriter(t *testing.T) {
	f := &fakeWire{}
	f.service = func(_ string, f *fakeWire) {
		f.enqueue(okay, 7, 1, nil)
		f.enqueue(wrte, 7, 1, []byte("READY-token"))
		f.enqueue(clse, 7, 1, nil)
	}
	err := newClient(f).shell("x", time.Second, func([]byte) error { return errors.New("disk full") }, "READY-token", "GO\n")
	if err == nil {
		t.Fatal("writer failure ignored")
	}
	for _, p := range f.writes {
		if bytes.Equal(p, []byte("GO\n")) {
			t.Fatal("armed despite disk failure")
		}
	}
}
func TestGateAcrossPacketBoundary(t *testing.T) {
	f := &fakeWire{}
	f.service = func(_ string, f *fakeWire) {
		f.enqueue(okay, 7, 1, nil)
		f.enqueue(wrte, 7, 1, []byte("READ"))
		f.enqueue(wrte, 7, 1, []byte("Y-token"))
		f.enqueue(clse, 7, 1, nil)
	}
	err := newClient(f).shell("x", time.Second, func([]byte) error { return nil }, "READY-token", "GO\n")
	if err != nil {
		t.Fatal(err)
	}
	count := 0
	for _, p := range f.writes {
		if bytes.Equal(p, []byte("GO\n")) {
			count++
		}
	}
	if count != 1 {
		t.Fatalf("GO count=%d", count)
	}
}
func TestBadPacketRejected(t *testing.T) {
	f := &fakeWire{}
	f.service = func(_ string, f *fakeWire) {
		f.enqueue(okay, 7, 1, nil)
		bad := frame(wrte, 7, 1, []byte("bad"))
		bad[16]++
		f.rx = append(f.rx, bad...)
	}
	var got bytes.Buffer
	err := newClient(f).shell("x", time.Second, func(p []byte) error { got.Write(p); return nil }, "", "")
	if err == nil || got.Len() != 0 {
		t.Fatal("corrupt packet accepted")
	}
}
func TestRemoteStatusRequiresUniqueEndMarker(t *testing.T) {
	for _, x := range []struct {
		body string
		want int
		ok   bool
	}{{"noise\nEND-token RC=0\n", 0, true}, {"END-token RC=23\n", 23, true}, {"successful output", 0, false}, {"END-other RC=0\n", 0, false}, {"END-token RC=0\nEND-token RC=0\n", 0, false}} {
		rc, e := remoteStatus(x.body, "END-token")
		if (e == nil) != x.ok || (x.ok && rc != x.want) {
			t.Fatalf("%q rc=%d err=%v", x.body, rc, e)
		}
	}
}

func TestPushUsesSyncAndWaitsForVerifiedStatus(t *testing.T) {
	f := &fakeWire{syncClose: true}
	var incoming []byte
	var contents []byte
	var destination string
	f.service = func(service string, f *fakeWire) {
		if service != "sync:" {
			t.Errorf("service=%q", service)
		}
		f.enqueue(okay, 7, 1, nil)
	}
	f.dataHook = func(p []byte, f *fakeWire) {
		incoming = append(incoming, p...)
		for len(incoming) >= 8 {
			tag := string(incoming[:4])
			n := int(binary.LittleEndian.Uint32(incoming[4:]))
			if tag == "DONE" {
				f.enqueue(wrte, 7, 1, []byte("OKAY\x00\x00\x00\x00"))
				incoming = incoming[8:]
				continue
			}
			if len(incoming) < 8+n {
				return
			}
			body := incoming[8 : 8+n]
			if tag == "SEND" {
				destination = string(body)
			} else if tag == "DATA" {
				contents = append(contents, body...)
			} else {
				t.Errorf("unexpected tag %q", tag)
			}
			incoming = incoming[8+n:]
		}
	}
	data := bytes.Repeat([]byte("payload"), 1000)
	err := newClient(f).push("/run/qdc-tun-test/probe.part", data, time.Second)
	if err != nil || !bytes.Equal(contents, data) || destination != "/run/qdc-tun-test/probe.part,33216" {
		t.Fatalf("push destination=%q bytes=%d error=%v", destination, len(contents), err)
	}
}
func TestPushRejectsNonStagingPathBeforeOpening(t *testing.T) {
	f := &fakeWire{}
	err := newClient(f).push("/etc/test", []byte("x"), time.Second)
	if err == nil || f.opens != 0 {
		t.Fatalf("%v opens=%d", err, f.opens)
	}
}

func TestConnectRetainsAndSkipsStalePackets(t *testing.T) {
	f := &fakeWire{}
	f.rx = frame(wrte, 88, 99, []byte("old logger output"))
	f.service = func(_ string, f *fakeWire) {
		f.enqueue(okay, 7, 1, nil)
		f.enqueue(wrte, 7, 1, []byte("fresh"))
		f.enqueue(clse, 7, 1, nil)
	}
	c := newClient(f)
	var orphan, body bytes.Buffer
	c.orphan = func(p packet) error { _, e := orphan.Write(p.data); return e }
	err := c.shell("id", time.Second, func(p []byte) error { _, e := body.Write(p); return e }, "", "")
	if err != nil || orphan.String() != "old logger output" || body.String() != "fresh" || f.opens != 1 {
		t.Fatalf("err=%v orphan=%q body=%q opens=%d", err, orphan.String(), body.String(), f.opens)
	}
}
