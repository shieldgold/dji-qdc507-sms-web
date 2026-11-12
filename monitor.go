package main

import (
	"encoding/csv"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

type NetworkSnapshot struct {
	UpdatedAt    string `json:"updated_at"`
	Technology   string `json:"technology"`
	Operator     string `json:"operator"`
	Band         string `json:"band"`
	Channel      string `json:"channel"`
	Registered   bool   `json:"registered"`
	Registration string `json:"registration"`
	RSRP         string `json:"rsrp"`
	RSRQ         string `json:"rsrq"`
	SINR         string `json:"sinr"`
	APN          string `json:"apn"`
	IMS          string `json:"ims"`
	Error        string `json:"error,omitempty"`
}
type DeviceSnapshot struct {
	Version           string `json:"version"`
	Firmware          string `json:"firmware"`
	Uptime            uint64 `json:"uptime"`
	MemoryTotal       uint64 `json:"memory_total"`
	MemoryAvailable   uint64 `json:"memory_available"`
	DiskTotal         uint64 `json:"disk_total"`
	DiskAvailable     uint64 `json:"disk_available"`
	AudioReady        bool   `json:"audio_ready"`
	RollbackAvailable bool   `json:"rollback_available"`
}
type Monitor struct {
	mu        sync.RWMutex
	refreshMu sync.Mutex
	command   func(string, time.Duration) ([]string, error)
	dataDir   string
	network   NetworkSnapshot
	firmware  string
	traffic   *Traffic
	paused    func() bool
}

func NewMonitor(command func(string, time.Duration) ([]string, error), dir string) *Monitor {
	return &Monitor{command: command, dataDir: dir, traffic: LoadTraffic(dir)}
}
func fields(s string) []string {
	r := csv.NewReader(strings.NewReader(s))
	r.TrimLeadingSpace = true
	x, _ := r.Read()
	for i := range x {
		x[i] = strings.TrimSpace(x[i])
	}
	return x
}
func applyNetworkLine(n *NetworkSnapshot, line string) {
	p := strings.SplitN(line, ":", 2)
	if len(p) != 2 {
		return
	}
	v := fields(strings.TrimSpace(p[1]))
	switch p[0] {
	case "+QNWINFO":
		if len(v) >= 4 {
			n.Technology, n.Operator, n.Band, n.Channel = v[0], v[1], v[2], v[3]
		}
	case "+CEREG":
		if len(v) < 2 {
			return
		}
		n.Registered = v[1] == "1" || v[1] == "5"
		names := map[string]string{"0": "未注册", "1": "已注册", "2": "正在搜索网络", "3": "注册被拒绝", "4": "状态未知", "5": "已注册（漫游）"}
		n.Registration = names[v[1]]
	case "+QCSQ":
		n.RSRP = ""
		n.RSRQ = ""
		n.SINR = ""
		if len(v) >= 5 && v[0] == "LTE" {
			n.RSRP = v[2] + " dBm"
			n.RSRQ = v[4] + " dB"
			if x, e := strconv.Atoi(v[3]); e == nil {
				n.SINR = fmt.Sprintf("%.1f dB", float64(x)/5-20)
			}
		}
	case "+CGDCONT":
		if len(v) >= 3 && v[0] == "1" {
			n.APN = v[2]
		}
	case "+QCFG":
		if len(v) >= 2 && v[0] == "ims" {
			modes := map[string]string{"0": "自动（按运营商配置）", "1": "强制启用", "2": "强制关闭"}
			n.IMS = modes[v[1]]
			if n.IMS == "" {
				n.IMS = "未知配置"
			}
			if len(v) > 2 {
				if v[2] == "1" {
					n.IMS += "；VoLTE 功能已启用，注册状态待验证"
				} else {
					n.IMS += "；VoLTE 功能未启用"
				}
			}
		}
	}
}
func (m *Monitor) Refresh() {
	if m.paused != nil && m.paused() {
		return
	}
	m.refreshMu.Lock()
	defer m.refreshMu.Unlock()
	n := NetworkSnapshot{Registration: "未知", IMS: "未知"}
	failed := false
	for _, cmd := range []string{"AT+QNWINFO", "AT+CEREG?", "AT+QCSQ", "AT+CGDCONT?", "AT+QCFG=\"ims\""} {
		lines, e := m.command(cmd, 3*time.Second)
		if e != nil {
			failed = true
			continue
		}
		for _, line := range lines {
			applyNetworkLine(&n, line)
		}
	}
	n.UpdatedAt = applicationTime.Now().UTC().Format(time.RFC3339)
	if failed {
		n.Error = "部分网络信息暂不可用"
	}
	m.mu.Lock()
	m.network = n
	m.mu.Unlock()
}
func (m *Monitor) Network() NetworkSnapshot { m.mu.RLock(); defer m.mu.RUnlock(); return m.network }
func readUint(path string) uint64 {
	b, e := os.ReadFile(path)
	if e != nil {
		return 0
	}
	v, _ := strconv.ParseUint(strings.TrimSpace(string(b)), 10, 64)
	return v
}
func (m *Monitor) sampleTraffic() {
	b, e := os.ReadFile("/proc/sys/kernel/random/boot_id")
	if e != nil {
		return
	}
	base := "/sys/class/net/ecm0/statistics/"
	if _, e = os.Stat(base + "rx_bytes"); e != nil {
		return
	}
	// ECM receives bytes uploaded by the USB host; tx is download to that host.
	m.traffic.Sample(strings.TrimSpace(string(b)), readUint(base+"rx_bytes"), readUint(base+"tx_bytes"), applicationTime.Now())
}
func (m *Monitor) Loop() {
	if lines, e := m.command("ATI", 3*time.Second); e == nil {
		for _, s := range lines {
			if strings.HasPrefix(s, "Revision:") {
				m.mu.Lock()
				m.firmware = strings.TrimSpace(strings.TrimPrefix(s, "Revision:"))
				m.mu.Unlock()
			}
		}
	}
	for i := 0; ; i++ {
		m.sampleTraffic()
		if i%3 == 0 {
			m.Refresh()
		}
		if i%12 == 0 {
			_ = m.traffic.Save()
		}
		time.Sleep(5 * time.Second)
	}
}
func (m *Monitor) Device() DeviceSnapshot {
	m.mu.RLock()
	firmware := m.firmware
	m.mu.RUnlock()
	d := DeviceSnapshot{Version: "2.0.0", Firmware: firmware}
	if b, e := os.ReadFile("/proc/uptime"); e == nil {
		f := strings.Fields(string(b))
		if len(f) > 0 {
			x, _ := strconv.ParseFloat(f[0], 64)
			d.Uptime = uint64(x)
		}
	}
	if b, e := os.ReadFile("/proc/meminfo"); e == nil {
		for _, line := range strings.Split(string(b), "\n") {
			f := strings.Fields(line)
			if len(f) < 2 {
				continue
			}
			v, _ := strconv.ParseUint(f[1], 10, 64)
			switch f[0] {
			case "MemTotal:":
				d.MemoryTotal = v * 1024
			case "MemAvailable:":
				d.MemoryAvailable = v * 1024
			}
		}
	}
	var stat syscall.Statfs_t
	if syscall.Statfs(m.dataDir, &stat) == nil {
		d.DiskTotal = stat.Blocks * uint64(stat.Bsize)
		d.DiskAvailable = stat.Bavail * uint64(stat.Bsize)
	}
	if b, e := os.ReadFile("/proc/asound/cards"); e == nil {
		d.AudioReady = strings.TrimSpace(string(b)) != "" && !strings.Contains(string(b), "no soundcards")
	}
	if _, e := os.Stat(filepath.Join(m.dataDir, "previous/smsweb")); e == nil {
		d.RollbackAvailable = true
	}
	return d
}
func (a *App) handleOverview(w *Resp, r *Req) error {
	return writeJSON(w, 200, map[string]any{"network": a.monitor.Network(), "traffic": a.monitor.traffic.View(applicationTime.Now()), "device": a.monitor.Device()})
}

type Diagnostic struct {
	Label  string `json:"label"`
	State  string `json:"state"`
	Detail string `json:"detail"`
}

func (a *App) handleDiagnostics(w *Resp, r *Req) error {
	if a.outbox.Busy() {
		return writeJSON(w, 409, map[string]string{"error": "正在发送短信，请完成后再诊断"})
	}
	a.monitor.Refresh()
	n := a.monitor.Network()
	sim := a.pin.Status()
	checks := []Diagnostic{{"管理页面连接", "ok", "已通过 USB 网络连接到模块"}}
	s := "warn"
	if sim.State == "READY" {
		s = "ok"
	}
	checks = append(checks, Diagnostic{"SIM 卡", s, sim.State})
	s = "warn"
	if n.Registered {
		s = "ok"
	}
	checks = append(checks, Diagnostic{"运营商注册", s, n.Registration})
	s = "warn"
	if n.RSRP != "" {
		s = "ok"
	}
	checks = append(checks, Diagnostic{"无线信号", s, strings.TrimSpace(n.RSRP + " / " + n.RSRQ)})
	a.mu.Lock()
	lastError := a.lastE
	a.mu.Unlock()
	if lastError == nil {
		checks = append(checks, Diagnostic{"短信读取", "ok", "最近一次轮询正常"})
	} else {
		checks = append(checks, Diagnostic{"短信读取", "warn", lastError.Error()})
	}
	checks = append(checks, Diagnostic{"4G 外网访问", "manual", "请在 iPhone 关闭 Wi-Fi 和自身蜂窝数据后打开测试网页；本页连接成功不代表 4G 已联网"})
	return writeJSON(w, 200, map[string]any{"checks": checks, "checked_at": applicationTime.Now().UTC().Format(time.RFC3339)})
}
