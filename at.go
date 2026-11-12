package main

// at.go — 模块内 AT 命令引擎（通过 /dev/smd9 直接与基带通信）

import (
	"bufio"
	"errors"
	"fmt"
	"log"
	"os"
	"strings"
	"sync"
	"syscall"
	"time"
)

type Modem struct {
	portPath string
	f        *os.File
	mu       sync.Mutex
	lines    chan string

	urcMu    sync.Mutex
	onNewSMS func()
}

func OpenModem(path string) (*Modem, error) {
	f, err := os.OpenFile(path, os.O_RDWR|syscall.O_NOCTTY|syscall.O_NONBLOCK, 0)
	if err != nil {
		return nil, err
	}
	// 清掉 O_NONBLOCK，让读操作阻塞（在独立 goroutine 里读）
	if err := syscall.SetNonblock(int(f.Fd()), false); err != nil {
		// 忽略：某些内核不支持
	}
	m := &Modem{portPath: path, f: f, lines: make(chan string, 4096)}
	go m.reader()
	return m, nil
}

func (m *Modem) push(l string) {
	select {
	case m.lines <- l:
	default:
		log.Printf("警告: 行缓冲已满")
	}
}

func (m *Modem) reader() {
	rd := bufio.NewReaderSize(m.f, 8192)
	var pending []byte
	for {
		b, err := rd.ReadByte()
		if err != nil {
			time.Sleep(150 * time.Millisecond)
			continue
		}
		pending = append(pending, b)
		if b == '\n' {
			l := strings.TrimRight(string(pending), "\r\n")
			pending = pending[:0]
			if l != "" {
				m.push(l)
			}
			continue
		}
		// 关键：AT+CMGS 的 "> " 提示符后面没有换行，不单独识别就永远等不到
		if t := strings.TrimLeft(string(pending), "\r\n"); t == ">" || t == "> " {
			m.push(">")
			pending = pending[:0]
		}
	}
}

func isURC(l string) bool {
	return strings.HasPrefix(l, "+CMTI:") ||
		strings.HasPrefix(l, "+CMT:") ||
		strings.HasPrefix(l, "+CDS:") ||
		strings.HasPrefix(l, "RING") ||
		strings.HasPrefix(l, "+CLIP:")
}

func (m *Modem) drain() {
	for {
		select {
		case l := <-m.lines:
			if isURC(l) {
				m.dispatchURC(l)
			}
		default:
			return
		}
	}
}

func (m *Modem) dispatchURC(l string) {
	if strings.HasPrefix(l, "+CMTI:") {
		m.urcMu.Lock()
		cb := m.onNewSMS
		m.urcMu.Unlock()
		if cb != nil {
			go cb()
		}
	}
}

func (m *Modem) SetNewSMSHandler(cb func()) {
	m.urcMu.Lock()
	m.onNewSMS = cb
	m.urcMu.Unlock()
}

// AT 发送一条命令并等待 OK / ERROR
func (m *Modem) AT(cmd string, timeout time.Duration) ([]string, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.atLocked(cmd, timeout)
}

func (m *Modem) atLocked(cmd string, timeout time.Duration) ([]string, error) {
	m.drain()
	if _, err := m.f.WriteString(cmd + "\r"); err != nil {
		return nil, err
	}
	deadline := time.After(timeout)
	var out []string
	for {
		select {
		case l := <-m.lines:
			if isURC(l) {
				m.dispatchURC(l)
				continue
			}
			out = append(out, l)
			if l == "OK" {
				return out, nil
			}
			if l == "ERROR" || strings.HasPrefix(l, "+CME ERROR") || strings.HasPrefix(l, "+CMS ERROR") {
				return out, fmt.Errorf("模块返回错误: %s", l)
			}
		case <-deadline:
			return out, fmt.Errorf("AT 超时 (%s): %s", timeout, cmd)
		}
	}
}

// SendPDU 以 PDU 模式发送一条短信。
// dry=true 时在收到 ">" 提示符后用 ESC 取消，只验证 PDU 是否被模块接受，不真正发出。
func (m *Modem) SendPDU(pduHex string, dry bool) ([]string, error) {
	m.mu.Lock()
	defer m.mu.Unlock()

	total := len(pduHex) / 2
	// 27.005：PDU 模式下 <length> 为 TP 数据单元长度，不含 SMSC 地址字段。
	// 我们的 PDU 里 SMSC 字段是 "00"（1 字节，表示用模块默认短信中心）。
	tpduLen := total - 1

	m.drain()
	if _, err := m.f.WriteString(fmt.Sprintf("AT+CMGS=%d\r", tpduLen)); err != nil {
		return nil, err
	}
	// 等 ">" 提示符
	deadline := time.After(10 * time.Second)
	gotPrompt := false
	for !gotPrompt {
		select {
		case l := <-m.lines:
			if strings.Contains(l, ">") {
				gotPrompt = true
			} else if l == "ERROR" || strings.HasPrefix(l, "+CMS ERROR") || strings.HasPrefix(l, "+CME ERROR") {
				return []string{l}, fmt.Errorf("模块拒绝发送: %s", l)
			}
		case <-deadline:
			return nil, errors.New("等待 CMGS 提示符超时")
		}
	}
	if dry {
		// ESC 取消，短信不会真正发出
		_, _ = m.f.WriteString("\x1b")
		time.Sleep(300 * time.Millisecond)
		m.drain()
		return []string{"DRY-RUN"}, nil
	}
	if _, err := m.f.WriteString(pduHex + "\x1a"); err != nil {
		return nil, &SubmissionError{Uncertain: true, Err: err}
	}
	deadline2 := time.After(60 * time.Second)
	var out []string
	accepted := false
	for {
		select {
		case l := <-m.lines:
			if isURC(l) {
				m.dispatchURC(l)
				continue
			}
			out = append(out, l)
			if strings.HasPrefix(l, "+CMGS:") {
				accepted = true
			}
			if l == "OK" {
				if !accepted {
					return out, &SubmissionError{Uncertain: true, Err: errors.New("未收到短信提交编号，请核对接收端")}
				}
				return out, nil
			}
			if l == "ERROR" || strings.HasPrefix(l, "+CMS ERROR") || strings.HasPrefix(l, "+CME ERROR") {
				return out, fmt.Errorf("发送失败: %s", l)
			}
		case <-deadline2:
			return out, &SubmissionError{Uncertain: true, Err: errors.New("等待模块发送结果超时，请核对接收端")}
		}
	}
}

// Setup 初始化模块到服务需要的状态
func (m *Modem) Setup() error {
	for _, cmd := range []string{"AT", "ATE0"} {
		if _, err := m.AT(cmd, 5*time.Second); err != nil {
			return err
		}
	}
	return nil
}

// SMS setup is deferred until the SIM is READY, so the PIN page stays reachable.
func (m *Modem) SetupSMS() error {
	steps := []struct {
		cmd string
		key string
	}{
		{"AT", "基础连通"},
		{"ATE0", "关闭回显"},
		{"AT+CMGF=0", "PDU 模式"},
		{"AT+CPMS=\"ME\",\"ME\",\"ME\"", "短信存储=模块内存"},
		{"AT+CNMI=2,1,0,0,0", "新短信通知"},
	}
	for _, s := range steps {
		if _, err := m.AT(s.cmd, 5*time.Second); err != nil {
			return fmt.Errorf("%s (%s) 失败: %w", s.key, s.cmd, err)
		}
	}
	return nil
}

// Status 查询信号/运营商/SIM
type ModemStatus struct {
	Signal   string `json:"signal"`
	Carrier  string `json:"carrier"`
	SimReady bool   `json:"simReady"`
	Storage  string `json:"storage"`
	Identity string `json:"identity"`
	OK       bool   `json:"ok"`
	Error    string `json:"error,omitempty"`
}

func (m *Modem) Status() ModemStatus {
	st := ModemStatus{}
	if r, err := m.AT("AT+CPIN?", 5*time.Second); err == nil {
		for _, l := range r {
			if strings.HasPrefix(l, "+CPIN:") {
				st.SimReady = strings.Contains(l, "READY")
			}
		}
	}
	if r, err := m.AT("AT+CSQ", 5*time.Second); err == nil {
		for _, l := range r {
			if strings.HasPrefix(l, "+CSQ:") {
				v := strings.TrimSpace(strings.TrimPrefix(l, "+CSQ:"))
				p := strings.Split(v, ",")
				if len(p) > 0 {
					rssi := strings.TrimSpace(p[0])
					st.Signal = rssi
					if rssi == "99" {
						st.Signal = "无信号"
					} else {
						st.Signal = rssi + " / 31"
					}
				}
			}
		}
	}
	if r, err := m.AT("AT+COPS?", 8*time.Second); err == nil {
		for _, l := range r {
			if strings.HasPrefix(l, "+COPS:") {
				if i := strings.Index(l, "\""); i >= 0 {
					if j := strings.Index(l[i+1:], "\""); j >= 0 {
						st.Carrier = l[i+1 : i+1+j]
					}
				}
			}
		}
	}
	if r, err := m.AT("AT+CGMI", 5*time.Second); err == nil {
		for _, l := range r {
			if l != "OK" && !strings.HasPrefix(l, "AT") && !strings.HasPrefix(l, "+") {
				st.Identity = l
			}
		}
	}
	if r, err := m.AT("AT+CPMS?", 5*time.Second); err == nil {
		for _, l := range r {
			if strings.HasPrefix(l, "+CPMS:") {
				st.Storage = strings.TrimSpace(strings.TrimPrefix(l, "+CPMS:"))
			}
		}
	}
	st.OK = true
	return st
}

// ListMessages 读取模块当前存储的全部短信（PDU 模式）
func (m *Modem) ListMessages() ([]*part, error) {
	r, err := m.AT("AT+CMGL=4", 20*time.Second)
	if err != nil && len(r) == 0 {
		return nil, err
	}
	var parts []*part
	var cur *part
	for _, l := range r {
		l = strings.TrimSpace(l)
		if strings.HasPrefix(l, "+CMGL:") {
			fields := strings.Split(l, ",")
			var idx int
			fmt.Sscanf(strings.TrimSpace(fields[0]), "+CMGL: %d", &idx)
			cur = &part{index: idx}
			if len(fields) > 1 {
				cur.status = strings.TrimSpace(fields[1])
			}
			if len(fields) > 3 {
				cur.status = strings.TrimSpace(fields[1])
			}
			continue
		}
		if l == "OK" || l == "" || strings.HasPrefix(l, "+") && !isHexLine(l) {
			continue
		}
		if cur != nil && isHexLine(l) {
			p, err := parseDeliver(l)
			if err == nil {
				p.index = cur.index
				p.status = cur.status
				parts = append(parts, p)
			} else {
				log.Printf("PDU 解析失败 (index=%d): %v", cur.index, err)
			}
			cur = nil
		}
	}
	return parts, nil
}

func isHexLine(s string) bool {
	if len(s) < 8 || len(s)%2 != 0 {
		return false
	}
	for _, c := range s {
		if !strings.ContainsRune("0123456789abcdefABCDEF", c) {
			return false
		}
	}
	return true
}

// DeleteMessage 删除模块中的某条短信
func (m *Modem) DeleteMessage(index int) error {
	_, err := m.AT(fmt.Sprintf("AT+CMGD=%d", index), 10*time.Second)
	return err
}

var statusNames = map[string]string{
	"0": "未读", "1": "已读", "2": "待发", "3": "已发", "4": "已收",
}

// SimIdentity SIM 卡身份（用于"每个号码一套密码"）
type SimIdentity struct {
	ICCID  string `json:"iccid"`
	IMSI   string `json:"imsi"`
	Number string `json:"number"`
	Key    string `json:"key"` // 账号主键：优先 ICCID，其次 IMSI
}

func digitsOnly(s string) string {
	var sb strings.Builder
	for _, c := range s {
		if c >= '0' && c <= '9' {
			sb.WriteRune(c)
		}
	}
	return sb.String()
}

// Identity 读取 SIM 的 ICCID / IMSI / 本机号码
func (m *Modem) Identity() SimIdentity {
	var si SimIdentity

	for _, cmd := range []string{"AT+QCCID", "AT+CCID", "AT+ICCID"} {
		if r, err := m.AT(cmd, 5*time.Second); err == nil {
			for _, l := range r {
				if strings.HasPrefix(l, "+QCCID:") || strings.HasPrefix(l, "+CCID:") || strings.HasPrefix(l, "+ICCID:") {
					v := digitsOnly(l[strings.IndexByte(l, ':')+1:])
					if len(v) >= 15 {
						si.ICCID = v
					}
				}
			}
			if si.ICCID != "" {
				break
			}
		}
	}
	if r, err := m.AT("AT+CIMI", 5*time.Second); err == nil {
		for _, l := range r {
			v := digitsOnly(l)
			if l == v && len(v) >= 14 {
				si.IMSI = v
			}
		}
	}
	if r, err := m.AT("AT+CNUM", 5*time.Second); err == nil {
		for _, l := range r {
			if strings.HasPrefix(l, "+CNUM:") {
				parts := strings.Split(l, "\"")
				for _, p := range parts {
					if v := digitsOnly(p); strings.HasPrefix(p, "+") && len(v) >= 11 {
						si.Number = "+" + v
						break
					}
				}
			}
		}
	}
	switch {
	case si.ICCID != "":
		si.Key = "iccid:" + si.ICCID
	case si.IMSI != "":
		si.Key = "imsi:" + si.IMSI
	default:
		si.Key = "unknown"
	}
	return si
}
