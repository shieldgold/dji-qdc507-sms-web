package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"sync"
	"time"
)

type ByteCount struct {
	Upload   uint64 `json:"upload"`
	Download uint64 `json:"download"`
}

func (b *ByteCount) add(up, down uint64) { b.Upload += up; b.Download += down }

type trafficState struct {
	BootID      string               `json:"boot_id"`
	Initialized bool                 `json:"initialized"`
	LastRX      uint64               `json:"last_rx"`
	LastTX      uint64               `json:"last_tx"`
	Total       ByteCount            `json:"total"`
	Session     ByteCount            `json:"session"`
	Days        map[string]ByteCount `json:"days"`
	Months      map[string]ByteCount `json:"months"`
	Since       string               `json:"since"`
}
type Traffic struct {
	mu      sync.Mutex
	file    string
	state   trafficState
	last    time.Time
	rate    ByteCount
	loadErr error
}
type TrafficView struct {
	Source     string    `json:"source"`
	Since      string    `json:"since"`
	Total      ByteCount `json:"total"`
	Session    ByteCount `json:"session"`
	Today      ByteCount `json:"today"`
	Month      ByteCount `json:"month"`
	Rate       ByteCount `json:"rate"`
	ClockValid bool      `json:"clock_valid"`
}

func LoadTraffic(dir string) *Traffic {
	t := &Traffic{file: filepath.Join(dir, "traffic.json")}
	if b, e := os.ReadFile(t.file); e == nil {
		t.loadErr = json.Unmarshal(b, &t.state)
	}
	if t.state.Days == nil {
		t.state.Days = map[string]ByteCount{}
	}
	if t.state.Months == nil {
		t.state.Months = map[string]ByteCount{}
	}
	return t
}

var chinaTime = time.FixedZone("CST", 8*3600)

func counterDelta(current, last uint64) uint64 {
	if current < last {
		return current
	}
	return current - last
}
func (t *Traffic) Sample(boot string, rx, tx uint64, now time.Time) {
	t.mu.Lock()
	defer t.mu.Unlock()
	up, down := uint64(0), uint64(0)
	if t.state.Initialized {
		if boot != t.state.BootID {
			up, down = rx, tx
			t.state.Session = ByteCount{}
		} else {
			up = counterDelta(rx, t.state.LastRX)
			down = counterDelta(tx, t.state.LastTX)
		}
	} else {
		t.state.Since = now.UTC().Format(time.RFC3339)
	}
	t.state.Initialized = true
	t.state.BootID = boot
	t.state.LastRX = rx
	t.state.LastTX = tx
	t.state.Total.add(up, down)
	t.state.Session.add(up, down)
	if now.Year() >= 2024 {
		local := now.In(chinaTime)
		day, month := local.Format("2006-01-02"), local.Format("2006-01")
		d := t.state.Days[day]
		d.add(up, down)
		t.state.Days[day] = d
		m := t.state.Months[month]
		m.add(up, down)
		t.state.Months[month] = m
		cutoff := local.AddDate(0, 0, -62).Format("2006-01-02")
		for k := range t.state.Days {
			if k < cutoff {
				delete(t.state.Days, k)
			}
		}
		mc := local.AddDate(-2, 0, 0).Format("2006-01")
		for k := range t.state.Months {
			if k < mc {
				delete(t.state.Months, k)
			}
		}
	}
	t.rate = ByteCount{}
	if seconds := now.Sub(t.last).Seconds(); !t.last.IsZero() && seconds > 0 && seconds < 60 {
		t.rate = ByteCount{uint64(float64(up) / seconds), uint64(float64(down) / seconds)}
	}
	t.last = now
}
func (t *Traffic) View(now time.Time) TrafficView {
	t.mu.Lock()
	defer t.mu.Unlock()
	local := now.In(chinaTime)
	return TrafficView{Source: "USB 链路（含管理页面流量，不等同运营商账单）", Since: t.state.Since, Total: t.state.Total, Session: t.state.Session, Today: t.state.Days[local.Format("2006-01-02")], Month: t.state.Months[local.Format("2006-01")], Rate: t.rate, ClockValid: now.Year() >= 2024}
}
func (t *Traffic) Save() error {
	t.mu.Lock()
	defer t.mu.Unlock()
	if t.loadErr != nil {
		return t.loadErr
	}
	b, e := json.Marshal(t.state)
	if e != nil {
		return e
	}
	return atomicWrite(t.file, b)
}
