package main

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"strconv"
	"strings"
	"time"
)

const (
	cnxn uint32 = 0x4e584e43
	open uint32 = 0x4e45504f
	okay uint32 = 0x59414b4f
	wrte uint32 = 0x45545257
	clse uint32 = 0x45534c43
	auth uint32 = 0x48545541
)

var errTimeout = errors.New("deadline exceeded; remote operation may still be running")

type transport interface {
	Write([]byte) error
	Read([]byte, time.Duration) (int, error)
	Close() error
}
type client struct {
	orphan    func(packet) error
	tr        transport
	connected bool
	failed    bool
	next      uint32
}
type packet struct {
	cmd, a, b uint32
	data      []byte
}

func sum(p []byte) uint32 {
	var n uint32
	for _, b := range p {
		n += uint32(b)
	}
	return n
}
func newClient(t transport) *client { return &client{tr: t, next: 1} }
func (c *client) send(cmd, a, b uint32, p []byte) error {
	if len(p) > 4096 {
		return errors.New("outgoing payload exceeds 4096")
	}
	h := make([]byte, 24)
	for i, v := range []uint32{cmd, a, b, uint32(len(p)), sum(p), cmd ^ 0xffffffff} {
		binary.LittleEndian.PutUint32(h[i*4:], v)
	}
	if e := c.tr.Write(h); e != nil {
		return e
	}
	if len(p) > 0 {
		return c.tr.Write(p)
	}
	return nil
}
func (c *client) exact(p []byte, until time.Time) error {
	for n := 0; n < len(p); {
		left := time.Until(until)
		if left <= 0 {
			return errTimeout
		}
		got, e := c.tr.Read(p[n:], left)
		if e != nil {
			return e
		}
		if got < 0 || got > len(p)-n {
			return errors.New("invalid transport byte count")
		}
		n += got
		if got == 0 {
			time.Sleep(time.Millisecond)
		}
	}
	return nil
}
func (c *client) recv(until time.Time) (packet, error) {
	var p packet
	h := make([]byte, 24)
	if e := c.exact(h, until); e != nil {
		return p, e
	}
	p.cmd = binary.LittleEndian.Uint32(h)
	p.a = binary.LittleEndian.Uint32(h[4:])
	p.b = binary.LittleEndian.Uint32(h[8:])
	n := binary.LittleEndian.Uint32(h[12:])
	if binary.LittleEndian.Uint32(h[20:]) != p.cmd^0xffffffff || n > 4096 {
		return p, errors.New("invalid ADB header or excessive payload")
	}
	p.data = make([]byte, n)
	if e := c.exact(p.data, until); e != nil {
		return p, e
	}
	if sum(p.data) != binary.LittleEndian.Uint32(h[16:]) {
		return p, errors.New("ADB payload checksum mismatch")
	}
	return p, nil
}
func (c *client) connect(until time.Time) error {
	if c.failed {
		return errors.New("transport is poisoned; no automatic retry")
	}
	if c.connected {
		return nil
	}
	if e := c.send(cnxn, 0x01000000, 4096, []byte("host::\x00")); e != nil {
		return e
	}
	for count := 0; count < 32; count++ {
		p, e := c.recv(until)
		if e != nil {
			return e
		}
		if p.cmd == cnxn {
			c.connected = true
			return nil
		}
		if p.cmd == auth {
			return errors.New("authentication is not attempted")
		}
		if c.orphan != nil {
			if e = c.orphan(p); e != nil {
				return e
			}
		}
	}
	return errors.New("too many residual packets before CNXN")
}
func (c *client) openStream(service string, until time.Time) (uint32, uint32, error) {
	if strings.IndexByte(service, 0) >= 0 || len(service) > 4000 {
		return 0, 0, errors.New("invalid service")
	}
	if e := c.connect(until); e != nil {
		return 0, 0, e
	}
	id := c.next
	c.next++
	if e := c.send(open, id, 0, append([]byte(service), 0)); e != nil {
		return 0, 0, e
	}
	for count := 0; count < 32; count++ {
		p, e := c.recv(until)
		if e != nil {
			return 0, 0, e
		}
		if p.cmd == okay && p.b == id {
			return id, p.a, nil
		}
		if p.cmd == auth || (p.cmd == clse && p.b == id) {
			return 0, 0, fmt.Errorf("service refused: %x", p.cmd)
		}
		if c.orphan != nil {
			if e = c.orphan(p); e != nil {
				return 0, 0, e
			}
		}
	}
	return 0, 0, errors.New("too many residual packets before OPEN acknowledgement")
}
func (c *client) shell(s string, d time.Duration, w func([]byte) error, gate, input string) (err error) {
	defer func() {
		if err != nil {
			c.failed = true
		}
	}()
	until := time.Now().Add(d)
	id, remote, e := c.openStream("shell:"+s, until)
	if e != nil {
		return e
	}
	var tail []byte
	armed := false
	for {
		p, e := c.recv(until)
		if e != nil {
			return e
		}
		if p.b != id || p.a != remote {
			return fmt.Errorf("unexpected stream id %d/%d", p.a, p.b)
		}
		switch p.cmd {
		case wrte:
			if e = w(p.data); e != nil {
				return fmt.Errorf("persist output: %w", e)
			}
			// Caller must sync output before this acknowledgement and before GO.
			if e = c.send(okay, id, remote, nil); e != nil {
				return e
			}
			if gate != "" && !armed {
				tail = append(tail, p.data...)
				if bytes.Contains(tail, []byte(gate)) {
					if e = c.send(wrte, id, remote, []byte(input)); e != nil {
						return e
					}
					armed = true
				}
				if len(tail) > 8192 {
					tail = tail[len(tail)-8192:]
				}
			}
		case okay: // acknowledgement of optional stdin data
		case clse:
			if e = c.send(clse, id, remote, nil); e != nil {
				return e
			}
			if gate != "" && !armed {
				return errors.New("service ended before logger READY")
			}
			return nil
		default:
			return fmt.Errorf("unexpected stream command %x", p.cmd)
		}
	}
}
func remoteStatus(s, end string) (int, error) {
	prefix := end + " RC="
	count := 0
	value := -1
	for _, line := range strings.Split(strings.ReplaceAll(s, "\r\n", "\n"), "\n") {
		if strings.HasPrefix(line, prefix) {
			count++
			v, e := strconv.Atoi(strings.TrimPrefix(line, prefix))
			if e != nil || v < 0 || v > 255 {
				return 0, errors.New("invalid remote exit status")
			}
			value = v
		}
	}
	if count != 1 {
		return 0, fmt.Errorf("expected exactly one remote END marker; got %d", count)
	}
	return value, nil
}

// push uses adbd sync without shell forks. The selected parent must already
// have been verified as RAM by a separate, read-only preflight.
func (c *client) push(path string, data []byte, d time.Duration) (err error) {
	if !strings.HasPrefix(path, "/run/qdc-tun-") || strings.Contains(path, "..") || strings.ContainsAny(path, "\x00,\n\r") {
		return errors.New("push restricted to reviewed /run/qdc-tun-* staging paths")
	}
	if len(data) > 1<<20 {
		return errors.New("staging artifact exceeds 1 MiB")
	}
	defer func() {
		if err != nil {
			c.failed = true
		}
	}()
	until := time.Now().Add(d)
	id, remote, e := c.openStream("sync:", until)
	if e != nil {
		return e
	}
	var response []byte
	receive := func() (packet, error) {
		p, e := c.recv(until)
		if e == nil && (p.a != remote || p.b != id) {
			e = errors.New("sync stream id mismatch")
		}
		return p, e
	}
	keep := func(p packet) error {
		response = append(response, p.data...)
		if len(response) > 8192 {
			return errors.New("excessive sync response")
		}
		return c.send(okay, id, remote, nil)
	}
	send := func(body []byte) error {
		for len(body) > 0 {
			n := len(body)
			if n > 4096 {
				n = 4096
			}
			if e := c.send(wrte, id, remote, body[:n]); e != nil {
				return e
			}
			for {
				p, e := receive()
				if e != nil {
					return e
				}
				if p.cmd == wrte {
					if e = keep(p); e != nil {
						return e
					}
					continue
				}
				if p.cmd != okay {
					return errors.New("sync closed before write acknowledgement")
				}
				break
			}
			body = body[n:]
		}
		return nil
	}
	message := func(tag string, body []byte) error {
		header := make([]byte, 8)
		copy(header, tag)
		binary.LittleEndian.PutUint32(header[4:], uint32(len(body)))
		return send(append(header, body...))
	}
	if e = message("SEND", []byte(path+",33216")); e != nil {
		return e
	}
	for len(data) > 0 {
		n := len(data)
		if n > 16384 {
			n = 16384
		}
		if e = message("DATA", data[:n]); e != nil {
			return e
		}
		data = data[n:]
	}
	done := make([]byte, 8)
	copy(done, "DONE")
	binary.LittleEndian.PutUint32(done[4:], uint32(time.Now().Unix()))
	if e = send(done); e != nil {
		return e
	}
	for len(response) < 8 {
		p, e := receive()
		if e != nil {
			return e
		}
		if p.cmd != wrte {
			return errors.New("missing sync status")
		}
		if e = keep(p); e != nil {
			return e
		}
	}
	if string(response[:4]) != "OKAY" || binary.LittleEndian.Uint32(response[4:]) != 0 {
		return fmt.Errorf("sync failure: %q", response)
	}
	if e = c.send(clse, id, remote, nil); e != nil {
		return e
	}
	p, e := receive()
	if e != nil {
		return e
	}
	if p.cmd != clse {
		return errors.New("sync service failed to close")
	}
	return nil
}
