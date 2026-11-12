package main

import (
	"bufio"
	"crypto/subtle"
	"errors"
	"io"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

type CallAudioStatus struct {
	Available bool   `json:"available"`
	State     string `json:"state"`
	Error     string `json:"error,omitempty"`
	UpBytes   uint64 `json:"up_bytes"`
	DownBytes uint64 `json:"down_bytes"`
}

type CallAudio struct {
	mu           sync.Mutex
	calls        *CallController
	dir          string
	ticket       string
	ticketCallID string
	listening    bool
	expires      time.Time
	active       net.Conn
	status       CallAudioStatus
}

func newCallAudio(calls *CallController, dir string) *CallAudio {
	return &CallAudio{calls: calls, dir: filepath.Join(dir, "voice-runtime"), status: CallAudioStatus{State: "idle"}}
}

func (a *CallAudio) Status() CallAudioStatus {
	a.mu.Lock()
	defer a.mu.Unlock()
	s := a.status
	_, e1 := os.Stat(filepath.Join(a.dir, "network-pcm-bridge"))
	_, e2 := os.Stat("/dev/snd/pcmC0D6c")
	s.Available = a.listening && e1 == nil && e2 == nil && calibrationAlive("/tmp/smsweb-voice-ready", "/proc")
	return s
}

// Readiness belongs to the exact live calibrator, not a stale success file.
func calibrationAlive(marker, procRoot string) bool {
	b, err := os.ReadFile(marker)
	if err != nil {
		return false
	}
	f := strings.Fields(string(b))
	if len(f) != 2 {
		return false
	}
	pid, err := strconv.Atoi(f[0])
	if err != nil || pid < 1 {
		return false
	}
	stat, err := os.ReadFile(filepath.Join(procRoot, f[0], "stat"))
	if err != nil {
		return false
	}
	end := strings.LastIndexByte(string(stat), ')')
	if end < 0 {
		return false
	}
	fields := strings.Fields(string(stat[end+1:]))
	if len(fields) <= 19 || fields[0] == "Z" || fields[19] != f[1] {
		return false
	}
	cmd, err := os.ReadFile(filepath.Join(procRoot, f[0], "cmdline"))
	return err == nil && strings.SplitN(string(cmd), "\x00", 2)[0] == "/usr/bin/alsaucm_test"
}

func (a *CallAudio) issueTicket() string {
	s := a.calls.Snapshot()
	if len(s.Calls) != 1 || s.Error != "" {
		return ""
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	a.ticket = randomHex(24)
	a.ticketCallID = s.Calls[0].ID
	a.expires = time.Now().Add(30 * time.Second)
	return a.ticket
}

func (a *CallAudio) consumeTicket(ticket string) (string, bool) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if a.ticket == "" || time.Now().After(a.expires) || subtle.ConstantTimeCompare([]byte(ticket), []byte(a.ticket)) != 1 {
		return "", false
	}
	a.ticket = ""
	for _, call := range a.calls.Snapshot().Calls {
		if call.ID == a.ticketCallID {
			return call.ID, true
		}
	}
	return "", false
}

func (a *CallAudio) setState(state, message string) {
	a.mu.Lock()
	a.status.State, a.status.Error = state, message
	a.mu.Unlock()
}

func (a *CallAudio) Serve(addr string) error {
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return err
	}
	defer ln.Close()
	a.mu.Lock()
	a.listening = true
	a.mu.Unlock()
	defer func() { a.mu.Lock(); a.listening = false; a.mu.Unlock() }()
	slots := make(chan struct{}, 3)
	for {
		c, err := ln.Accept()
		if err != nil {
			return err
		}
		select {
		case slots <- struct{}{}:
			go func() { defer func() { <-slots }(); a.serveConn(c) }()
		default:
			c.Close()
		}
	}
}

func (a *CallAudio) serveConn(c net.Conn) {
	defer c.Close()
	_ = c.SetDeadline(time.Now().Add(5 * time.Second))
	br := bufio.NewReaderSize(c, 128)
	line, err := br.ReadSlice('\n')
	if err != nil || len(line) != 54 || string(line[:5]) != "PCM1 " {
		return
	}
	callID, accepted := a.consumeTicket(string(line[5:53]))
	if !accepted {
		return
	}
	a.mu.Lock()
	if a.active != nil {
		a.mu.Unlock()
		return
	}
	a.active = c
	a.status = CallAudioStatus{State: "waiting"}
	a.mu.Unlock()
	defer func() { a.mu.Lock(); a.active = nil; a.mu.Unlock() }()
	endCallOnExit := true
	defer func() {
		if endCallOnExit {
			_ = a.calls.Hangup(callID)
		}
	}()
	if _, err := c.Write([]byte("OK\n")); err != nil {
		return
	}
	_ = c.SetDeadline(time.Time{})
	frames := make(chan []byte, 4)
	done := make(chan struct{})
	defer close(done)
	defer c.Close()
	go func() {
		defer close(frames)
		for {
			_ = c.SetReadDeadline(time.Now().Add(6 * time.Second))
			b := make([]byte, 320)
			if _, e := io.ReadFull(br, b); e != nil {
				return
			}
			a.mu.Lock()
			a.status.UpBytes += 320
			a.mu.Unlock()
			select {
			case frames <- b:
			case <-done:
				return
			default: /* Bound latency: discard when consumer lags. */
			}
		}
	}()
	started := time.Now()
	for {
		s := a.calls.Snapshot()
		found, active := false, false
		for _, call := range s.Calls {
			if call.ID == callID {
				found, active = true, call.State == "active"
			}
		}
		if !found {
			endCallOnExit = false
			a.setState("idle", "")
			return
		}
		if active && s.Error == "" {
			break
		}
		select {
		case _, ok := <-frames:
			if !ok {
				a.setState("idle", "")
				return
			}
		case <-time.After(100 * time.Millisecond):
		}
		if time.Since(started) > 90*time.Second {
			a.setState("failed", "等待接通超时")
			return
		}
	}

	err = a.stream(c, frames, callID)
	if err != nil {
		a.setState("failed", "音频连接已中断，正在结束本次电话")
	} else {
		endCallOnExit = false
		a.setState("idle", "")
	}
}

type audioProcess struct {
	cmd  *exec.Cmd
	done chan error
}

// Vendor diagnostics live in RAM on this module. Bound them for long calls.
type audioLog struct {
	mu        sync.Mutex
	writer    io.Writer
	remaining int
}

func (l *audioLog) Write(p []byte) (int, error) {
	l.mu.Lock()
	defer l.mu.Unlock()
	n := len(p)
	if len(p) > l.remaining {
		p = p[:l.remaining]
	}
	written, err := l.writer.Write(p)
	l.remaining -= written
	if err != nil {
		return written, err
	}
	return n, nil
}

func startAudioProcess(cmd *exec.Cmd) (*audioProcess, error) {
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	p := &audioProcess{cmd: cmd, done: make(chan error, 1)}
	go func() { p.done <- cmd.Wait(); close(p.done) }()
	return p, nil
}

func (p *audioProcess) stop() {
	if p == nil {
		return
	}
	_ = p.cmd.Process.Signal(syscall.SIGTERM)
	select {
	case <-p.done:
	case <-time.After(5 * time.Second):
		_ = p.cmd.Process.Kill()
		<-p.done
	}
}

func (a *CallAudio) stream(c net.Conn, frames <-chan []byte, callID string) error {
	helper := filepath.Join(a.dir, "network-pcm-bridge")
	logFile, err := os.OpenFile("/tmp/smsweb-voice.log", os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0600)
	if err != nil {
		return err
	}
	defer logFile.Close()
	boundedLog := &audioLog{writer: logFile, remaining: 64 << 10}
	routeCmd := exec.Command(helper, "--network-route-session", "--verbose")
	routeCmd.Stdout, routeCmd.Stderr = boundedLog, boundedLog
	route, err := startAudioProcess(routeCmd)
	if err != nil {
		return err
	}
	defer route.stop()
	ready := false
	for i := 0; i < 60; i++ {
		select {
		case <-route.done:
			return errors.New("voice route exited")
		default:
		}
		b, _ := os.ReadFile("/tmp/smsweb-voice.log")
		if strings.Contains(string(b), "VoLTE route session active") {
			ready = true
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	if !ready {
		return errors.New("voice route timed out")
	}
	pcmCmd := exec.Command(helper, "--stdio", "--no-mixers", "--playback-device", "hw:0,5", "--capture-device", "hw:0,6", "--verbose")
	pcmCmd.Stderr = boundedLog
	in, err := pcmCmd.StdinPipe()
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := pcmCmd.StdoutPipe()
	if err != nil {
		return err
	}
	defer out.Close()
	pcm, err := startAudioProcess(pcmCmd)
	if err != nil {
		return err
	}
	defer pcm.stop()
	stop := make(chan struct{})
	defer close(stop)
	fail := make(chan error, 2)
	go func() {
		for {
			select {
			case b, ok := <-frames:
				if !ok {
					fail <- io.EOF
					return
				}
				if _, e := in.Write(b); e != nil {
					fail <- e
					return
				}
			case <-stop:
				return
			}
		}
	}()
	go func() {
		b := make([]byte, 320)
		for {
			if _, e := io.ReadFull(out, b); e != nil {
				fail <- e
				return
			}
			_ = c.SetWriteDeadline(time.Now().Add(2 * time.Second))
			if _, e := c.Write(b); e != nil {
				fail <- e
				return
			}
			a.mu.Lock()
			a.status.DownBytes += 320
			a.mu.Unlock()
		}
	}()
	a.setState("streaming", "")
	ticker := time.NewTicker(250 * time.Millisecond)
	defer ticker.Stop()
	for {
		select {
		case err := <-fail:
			return err
		case <-route.done:
			return errors.New("voice route stopped")
		case <-pcm.done:
			return errors.New("voice PCM stopped")
		case <-ticker.C:
			s := a.calls.Snapshot()
			found := false
			for _, call := range s.Calls {
				if call.ID == callID {
					found = true
				}
			}
			if !found {
				return nil
			}
		}
	}
}
