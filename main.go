package main

// smsweb — 跑在大疆 4G 模块（Baiwang QDC507）内部的短信网页服务
//
// 模块插到任何设备（iPad / Mac / 电脑）上，设备通过 USB 网卡拿到 192.168.225.x 地址后，
// 浏览器打开 http://192.168.225.1:8787 即可收发短信、查看验证码。
//
// 环境约束（实测）：内核 3.18.44 / 32 位 ARMv7 / 内存仅 43MB。
// 标准库 net/http 在该内核上一初始化就段错误，因此这里用自写的极简 HTTP 服务器（见 httpmux.go）。

import (
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"runtime/debug"
	"sort"
	"strings"
	"sync"
	"time"
)

var (
	flagAT     = flag.String("at", "/dev/smd9", "AT 命令通道")
	flagPort   = flag.String("port", "8787", "HTTP 端口")
	flagListen = flag.String("listen", "192.168.225.1", "仅监听模块 USB 网络地址")
	flagData   = flag.String("data", "/data/smsweb", "数据目录")
	flagPoll   = flag.Duration("poll", 3*time.Second, "轮询新短信间隔")
	flagDebug  = flag.Bool("debug", false, "打印详细日志")
)

type Store struct {
	mu      sync.RWMutex
	saveMu  sync.Mutex
	loadErr error
	Msgs    map[string]*Message `json:"messages"`
	path    string
}

func LoadStore(dir string) *Store {
	s := &Store{Msgs: map[string]*Message{}, path: filepath.Join(dir, "messages.json")}
	if b, err := os.ReadFile(s.path); err == nil {
		s.loadErr = json.Unmarshal(b, &s.Msgs)
		if s.Msgs == nil {
			s.Msgs = map[string]*Message{}
		}
	}
	return s
}

func (s *Store) Save() error {
	s.saveMu.Lock()
	defer s.saveMu.Unlock()
	if s.loadErr != nil {
		return fmt.Errorf("短信归档无法读取，已保留原文件: %w", s.loadErr)
	}
	s.mu.RLock()
	b, err := json.Marshal(s.Msgs)
	s.mu.RUnlock()
	if err != nil {
		return err
	}
	return atomicWrite(s.path, b)
}

func (s *Store) Merge(msgs []*Message) (added int) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, m := range msgs {
		if old, ok := s.Msgs[m.Key]; ok {
			old.Index = m.Index
			old.Status = m.Status
			continue
		}
		s.Msgs[m.Key] = m
		added++
	}
	// 防止无限增长：最多保留 300 条
	if len(s.Msgs) > 300 {
		all := make([]*Message, 0, len(s.Msgs))
		for _, m := range s.Msgs {
			all = append(all, m)
		}
		sort.Slice(all, func(i, j int) bool { return all[i].Time > all[j].Time })
		for _, m := range all[300:] {
			if m.SendState == "queued" || m.SendState == "sending" {
				continue
			}
			delete(s.Msgs, m.Key)
		}
	}
	return
}

func (s *Store) List() []*Message {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make([]*Message, 0, len(s.Msgs))
	for _, m := range s.Msgs {
		copy := *m
		out = append(out, &copy)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Time > out[j].Time })
	return out
}

func (s *Store) Get(key string) *Message {
	s.mu.RLock()
	defer s.mu.RUnlock()
	if m := s.Msgs[key]; m != nil {
		copy := *m
		return &copy
	}
	return nil
}

func (s *Store) Delete(key string) {
	s.mu.Lock()
	delete(s.Msgs, key)
	s.mu.Unlock()
}

// 验证码识别：优先匹配「验证码/校验码/动态码」等关键词后面的数字，
// 否则退回到独立的 4-8 位数字（必须不被字母、点、斜杠、数字包围，
// 以免把 URL 里的数字、以及手机号中间几位误当成验证码）
var (
	codeReStrong = regexp.MustCompile(`(?:验证码|校验码|动态码|动态密码|确认码|安全码|口令|code)[^0-9]{0,8}([0-9]{4,8})`)
	codeReWeak   = regexp.MustCompile(`(?:^|[^0-9A-Za-z./:_-])([0-9]{4,8})(?:[^0-9A-Za-z./:_-]|$)`)
)

func detectCode(body string) string {
	if m := codeReStrong.FindStringSubmatch(body); len(m) > 1 {
		return m[1]
	}
	if m := codeReWeak.FindStringSubmatch(body); len(m) > 1 {
		return m[1]
	}
	return ""
}

// mergeParts 合并长短信（UDH 分片）
func mergeParts(parts []*part) []*Message {
	type gk struct {
		ref    int
		sender string
		total  int
	}
	groups := map[gk][]*part{}
	var singles []*part
	for _, p := range parts {
		if p.total > 1 && p.ref >= 0 {
			k := gk{p.ref, p.sender, p.total}
			groups[k] = append(groups[k], p)
		} else {
			singles = append(singles, p)
		}
	}
	var out []*Message
	mk := func(sender, body string, t time.Time, index int, status string) *Message {
		ts := t.Format("2006-01-02 15:04:05")
		if t.IsZero() {
			ts = time.Now().Format("2006-01-02 15:04:05")
		}
		key := fmt.Sprintf("%s|%s|%x", sender, ts, body)
		return &Message{
			Key: key, Index: index, Sender: sender, Body: body, Time: ts,
			Status: statusNames[status], Direction: "in", Code: detectCode(body),
		}
	}
	for _, p := range singles {
		out = append(out, mk(p.sender, p.body, p.t, p.index, p.status))
	}
	for _, g := range groups {
		sort.Slice(g, func(i, j int) bool { return g[i].seq < g[j].seq })
		var sb strings.Builder
		for _, p := range g {
			sb.WriteString(p.body)
		}
		earliest := g[0].t
		for _, p := range g {
			if !p.t.IsZero() && (earliest.IsZero() || p.t.Before(earliest)) {
				earliest = p.t
			}
		}
		out = append(out, mk(g[0].sender, sb.String(), earliest, g[0].index, g[0].status))
	}
	return out
}

type App struct {
	modem         *Modem
	store         *Store
	auth          *AuthStore
	pin           *PINController
	opMu          sync.Mutex
	actionMu      sync.Mutex
	maintenance   bool
	smsConfigured bool
	outbox        *Outbox
	monitor       *Monitor

	identMu sync.Mutex
	ident   SimIdentity
	identAt time.Time

	mu    sync.Mutex
	last  time.Time
	lastE error
	seq   int
}

// identity 返回当前 SIM 身份（带缓存；换卡后最多 1 分钟内自动识别）
func (a *App) identity() SimIdentity {
	a.identMu.Lock()
	defer a.identMu.Unlock()
	if a.ident.Key == "" || a.ident.Key == "unknown" || (time.Since(a.identAt) > time.Minute && (a.outbox == nil || !a.outbox.Busy())) {
		if si := a.modem.Identity(); si.Key != "" && si.Key != "unknown" {
			if si.Key != a.ident.Key {
				log.Printf("SIM 身份已更新")
			}
			a.ident = si
			a.identAt = time.Now()
		}
	}
	return a.ident
}

// requireAuth 需要登录才能访问
func (a *App) requireAuth(h Handler) Handler {
	return func(w *Resp, r *Req) error {
		si := a.identity()
		if si.Key == "" || si.Key == "unknown" || !a.auth.Check(cookieValue(r.Header["cookie"], tokenName), si.Key) {
			return writeJSON(w, 401, map[string]string{"error": "未登录", "code": "unauthorized"})
		}
		return h(w, r)
	}
}

func (a *App) handleAuthStatus(w *Resp, r *Req) error {
	si := a.identity()
	tok := cookieValue(r.Header["cookie"], tokenName)
	return writeJSON(w, 200, map[string]any{
		"iccid":         si.ICCID,
		"imsi":          si.IMSI,
		"number":        si.Number,
		"registered":    a.auth.Registered(si.Key),
		"authenticated": a.auth.Check(tok, si.Key),
		"minPassword":   minPassword,
		"sim":           a.pin.Status(),
	})
}

func (a *App) handleAuthSetup(w *Resp, r *Req) error {
	var req struct {
		Password string `json:"password"`
		Confirm  string `json:"confirm"`
		Remember bool   `json:"remember"`
	}
	if err := json.Unmarshal(r.Body, &req); err != nil {
		return writeJSON(w, 400, map[string]string{"error": "请求格式错误"})
	}
	if req.Password != req.Confirm {
		return writeJSON(w, 400, map[string]string{"error": "两次输入的密码不一致"})
	}
	si := a.identity()
	if si.Key == "" || si.Key == "unknown" {
		return writeJSON(w, 400, map[string]string{"error": "尚未识别 SIM 卡，请稍后重试"})
	}
	tok, err := a.auth.Setup(si.Key, si.IMSI, si.Number, req.Password, req.Remember)
	if err != nil {
		return writeJSON(w, 400, map[string]string{"error": err.Error()})
	}
	log.Printf("已为该 SIM 注册访问密码")
	ttl := 0
	if req.Remember {
		ttl = int(rememberTTL.Seconds())
	}
	w.AddCookie(cookieHeader(tok, ttl))
	return writeJSON(w, 200, map[string]any{"ok": true, "remember": req.Remember})
}

func (a *App) handleAuthLogin(w *Resp, r *Req) error {
	var req struct {
		Password string `json:"password"`
		Remember bool   `json:"remember"`
	}
	if err := json.Unmarshal(r.Body, &req); err != nil {
		return writeJSON(w, 400, map[string]string{"error": "请求格式错误"})
	}
	si := a.identity()
	tok, err := a.auth.Login(si.Key, req.Password, req.Remember)
	if err != nil {
		return writeJSON(w, 401, map[string]string{"error": err.Error()})
	}
	ttl := 0
	if req.Remember {
		ttl = int(rememberTTL.Seconds())
	}
	w.AddCookie(cookieHeader(tok, ttl))
	return writeJSON(w, 200, map[string]any{"ok": true, "remember": req.Remember})
}

func (a *App) handleAuthLogout(w *Resp, r *Req) error {
	if err := a.auth.Logout(cookieValue(r.Header["cookie"], tokenName)); err != nil {
		return writeJSON(w, 500, map[string]string{"error": err.Error()})
	}
	w.AddCookie(cookieClear())
	return writeJSON(w, 200, map[string]any{"ok": true})
}

func (a *App) handleAuthChange(w *Resp, r *Req) error {
	var req struct {
		Old      string `json:"old"`
		Password string `json:"password"`
		Confirm  string `json:"confirm"`
		Remember bool   `json:"remember"`
	}
	if err := json.Unmarshal(r.Body, &req); err != nil {
		return writeJSON(w, 400, map[string]string{"error": "请求格式错误"})
	}
	if req.Password != req.Confirm {
		return writeJSON(w, 400, map[string]string{"error": "两次输入的新密码不一致"})
	}
	si := a.identity()
	if err := a.auth.ChangePassword(si.Key, req.Old, req.Password); err != nil {
		return writeJSON(w, 400, map[string]string{"error": err.Error()})
	}
	ttl := 0
	if req.Remember {
		ttl = int(rememberTTL.Seconds())
	}
	tok, err := a.auth.IssueToken(si.Key, req.Remember)
	if err != nil {
		return writeJSON(w, 500, map[string]string{"error": "密码已修改，请重新登录"})
	}
	w.AddCookie(cookieHeader(tok, ttl))
	log.Printf("访问密码已修改")
	return writeJSON(w, 200, map[string]any{"ok": true})
}

func (a *App) pollOnce() {
	a.opMu.Lock()
	defer a.opMu.Unlock()
	if err := a.ensureSMSLocked(); err != nil {
		a.mu.Lock()
		a.last = time.Now()
		a.lastE = err
		a.mu.Unlock()
		return
	}
	parts, err := a.modem.ListMessages()
	a.mu.Lock()
	a.last = time.Now()
	a.lastE = err
	a.mu.Unlock()
	if err != nil && len(parts) == 0 {
		return
	}
	if n := a.store.Merge(mergeParts(parts)); n > 0 {
		log.Printf("新增 %d 条短信", n)
		a.store.Save()
	}
}

func (a *App) pollLoop() {
	for {
		a.pollOnce()
		time.Sleep(*flagPoll)
	}
}

func writeJSON(w *Resp, code int, v any) error {
	b, err := json.Marshal(v)
	if err != nil {
		return w.Write(500, "text/plain; charset=utf-8", []byte(err.Error()))
	}
	return w.Write(code, "application/json; charset=utf-8", b)
}

func (a *App) handleState(w *Resp, r *Req) error {
	a.mu.Lock()
	last, lastE := a.last, a.lastE
	a.mu.Unlock()
	resp := map[string]any{
		"messages": a.store.List(),
		"lastPoll": last.Format("15:04:05"),
	}
	if !a.outbox.Busy() {
		resp["sim"] = a.pin.Status()
	}
	if lastE != nil {
		resp["lastError"] = lastE.Error()
	}
	return writeJSON(w, 200, resp)
}

func (a *App) handleSend(w *Resp, r *Req) error {
	a.actionMu.Lock()
	defer a.actionMu.Unlock()
	if a.maintenance {
		return writeJSON(w, 409, map[string]string{"error": "正在切换版本，请稍后刷新"})
	}
	var req SendRequest
	if err := json.Unmarshal(r.Body, &req); err != nil {
		return writeJSON(w, 400, map[string]string{"error": "请求格式错误"})
	}
	m, err := a.outbox.Enqueue(req)
	if err != nil {
		return writeJSON(w, 400, map[string]string{"error": err.Error()})
	}
	return writeJSON(w, 202, m)
}

func (a *App) handleDelete(w *Resp, r *Req) error {
	a.opMu.Lock()
	defer a.opMu.Unlock()
	var req struct {
		Key string `json:"key"`
	}
	if err := json.Unmarshal(r.Body, &req); err != nil {
		return writeJSON(w, 400, map[string]string{"error": "请求格式错误"})
	}
	m := a.store.Get(req.Key)
	if m == nil {
		return writeJSON(w, 404, map[string]string{"error": "找不到该短信"})
	}
	if m.SendState == "queued" || m.SendState == "sending" {
		return writeJSON(w, 409, map[string]string{"error": "短信正在提交，暂时不能删除"})
	}
	note := ""
	if m.Index > 0 && m.Direction == "in" {
		if err := a.modem.DeleteMessage(m.Index); err != nil {
			note = "（模块侧删除失败: " + err.Error() + "，已从本机列表移除）"
		}
	}
	a.store.Delete(req.Key)
	a.store.Save()
	return writeJSON(w, 200, map[string]any{"ok": true, "note": note})
}

func main() {
	flag.Parse()
	addr, err := listenAddress(*flagListen, *flagPort)
	if err != nil {
		log.Fatal(err)
	}
	// 这块模块只有 43MB 内存，尽量压低运行时开销
	runtime.GOMAXPROCS(1)
	debug.SetGCPercent(20)
	debug.SetMemoryLimit(12 << 20) // Soft runtime target; excludes kernel and other module processes.

	if err := os.MkdirAll(*flagData, 0700); err != nil {
		log.Fatalf("无法创建数据目录: %v", err)
	}
	if logf, err := os.OpenFile(filepath.Join(*flagData, "smsweb.log"),
		os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0600); err == nil {
		log.SetOutput(logf)
	}
	log.Printf("=== smsweb 启动 (AT=%s port=%s) ===", *flagAT, *flagPort)

	app := &App{store: LoadStore(*flagData), auth: LoadAuth(*flagData)}

	var modem *Modem
	for i := 0; i < 60; i++ {
		modem, err = OpenModem(*flagAT)
		if err == nil {
			if err = modem.Setup(); err == nil {
				break
			}
			log.Printf("初始化失败(%d): %v", i, err)
			modem = nil
		} else {
			log.Printf("打开 %s 失败(%d): %v", *flagAT, i, err)
		}
		time.Sleep(2 * time.Second)
	}
	if modem == nil {
		log.Fatalf("无法初始化模块 AT 通道 %s", *flagAT)
	}
	app.modem = modem
	app.pin = newPINController(modem.AT, *flagData)
	app.outbox = NewOutbox(app.store, app.runOutgoing)
	app.monitor = NewMonitor(modem.AT, *flagData)
	go app.monitor.Loop()
	modem.SetNewSMSHandler(func() { app.pollOnce() })
	st := modem.Status()
	log.Printf("模块就绪: 运营商=%s 信号=%s SIM=%v", st.Carrier, st.Signal, st.SimReady)

	go app.pollLoop()
	go func() {
		time.Sleep(2 * time.Second)
		si := app.identity()
		if a := app.auth.Registered(si.Key); a {
			log.Printf("该 SIM 已注册访问密码")
		} else {
			log.Printf("该 SIM 尚未注册访问密码，首次打开网页时需设置")
		}
	}()

	routes := map[string]Handler{
		"GET /api/auth/status":           app.handleAuthStatus,
		"POST /api/time":                 app.requireAuth(handleClientTime),
		"POST /api/maintenance/rollback": app.requireAuth(app.handleRollback),
		"POST /api/auth/setup":           app.handleAuthSetup,
		"POST /api/auth/login":           app.handleAuthLogin,
		"POST /api/auth/logout":          app.handleAuthLogout,
		"POST /api/auth/change":          app.requireAuth(app.handleAuthChange),
		"GET /api/overview":              app.requireAuth(app.handleOverview),
		"POST /api/diagnostics":          app.requireAuth(app.handleDiagnostics),
		"GET /api/export":                app.requireAuth(app.handleExport),
		"POST /api/restore":              app.requireAuth(app.handleRestore),
		"GET /api/state":                 app.requireAuth(app.handleState),
		"GET /api/send/status":           app.requireAuth(app.handleSendStatus),
		"POST /api/send":                 app.requireAuth(app.handleSend),
		"POST /api/delete":               app.requireAuth(app.handleDelete),
		"GET /api/sim":                   app.requireAuth(app.handlePINStatus),
		"POST /api/sim/unlock":           app.requireAuth(app.handlePINUnlock),
		"POST /api/sim/retry":            app.requireAuth(app.handlePINRetry),
	}
	fallback := func(w *Resp, r *Req) error {
		switch r.Path {
		case "/":
			return w.Write(200, "text/html; charset=utf-8", []byte(indexHTML))
		case "/style.css":
			return w.Write(200, "text/css; charset=utf-8", []byte(styleCSS))
		case "/model.js":
			return w.Write(200, "application/javascript; charset=utf-8", []byte(modelJS))
		case "/app.js":
			return w.Write(200, "application/javascript; charset=utf-8", []byte(appJS))
		default:
			return w.Write(404, "text/plain; charset=utf-8", []byte("not found"))
		}
	}

	log.Printf("HTTP 监听 %s", addr)
	if err := ServeHTTP(addr, routes, fallback); err != nil {
		log.Fatalf("HTTP 启动失败: %v", err)
	}
}
