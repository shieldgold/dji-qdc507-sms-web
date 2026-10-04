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
	flagAT    = flag.String("at", "/dev/smd9", "AT 命令通道")
	flagPort  = flag.String("port", "8787", "HTTP 端口")
	flagData  = flag.String("data", "/data/smsweb", "数据目录")
	flagPoll  = flag.Duration("poll", 3*time.Second, "轮询新短信间隔")
	flagDebug = flag.Bool("debug", false, "打印详细日志")
)

type Store struct {
	mu   sync.RWMutex
	Msgs map[string]*Message `json:"messages"`
	path string
}

func LoadStore(dir string) *Store {
	s := &Store{Msgs: map[string]*Message{}, path: filepath.Join(dir, "messages.json")}
	if b, err := os.ReadFile(s.path); err == nil {
		_ = json.Unmarshal(b, &s.Msgs)
	}
	return s
}

func (s *Store) Save() {
	s.mu.RLock()
	b, _ := json.Marshal(s.Msgs)
	s.mu.RUnlock()
	tmp := s.path + ".tmp"
	if err := os.WriteFile(tmp, b, 0644); err == nil {
		_ = os.Rename(tmp, s.path)
	}
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
		out = append(out, m)
	}
	sort.Slice(out, func(i, j int) bool { return out[i].Time > out[j].Time })
	return out
}

func (s *Store) Get(key string) *Message {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.Msgs[key]
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
	modem *Modem
	store *Store
	auth  *AuthStore

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
	if a.ident.Key == "" || a.ident.Key == "unknown" || time.Since(a.identAt) > time.Minute {
		if si := a.modem.Identity(); si.Key != "" && si.Key != "unknown" {
			if si.Key != a.ident.Key {
				log.Printf("SIM 身份: ICCID=%s IMSI=%s 号码=%s", si.ICCID, si.IMSI, si.Number)
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
		if !a.auth.Check(cookieValue(r.Header["cookie"], tokenName), si.Key) {
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
	tok, err := a.auth.Setup(si.Key, si.IMSI, si.Number, req.Password, req.Remember)
	if err != nil {
		return writeJSON(w, 400, map[string]string{"error": err.Error()})
	}
	log.Printf("已为该 SIM 注册访问密码 (ICCID=%s)", si.ICCID)
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
	a.auth.Logout(cookieValue(r.Header["cookie"], tokenName))
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
	w.AddCookie(cookieHeader(a.auth.IssueToken(si.Key, req.Remember), ttl))
	log.Printf("访问密码已修改 (ICCID=%s)", si.ICCID)
	return writeJSON(w, 200, map[string]any{"ok": true})
}

func (a *App) pollOnce() {
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
	st := a.modem.Status()
	a.mu.Lock()
	last, lastE := a.last, a.lastE
	a.mu.Unlock()
	resp := map[string]any{
		"status":   st,
		"messages": a.store.List(),
		"lastPoll": last.Format("15:04:05"),
	}
	if lastE != nil {
		resp["lastError"] = lastE.Error()
	}
	return writeJSON(w, 200, resp)
}

func (a *App) handleSend(w *Resp, r *Req) error {
	var req struct {
		Number string `json:"number"`
		Text   string `json:"text"`
		Dry    bool   `json:"dry"` // 只验证 PDU 是否被模块接受，在 Ctrl-Z 前取消，不真正发出
	}
	if err := json.Unmarshal(r.Body, &req); err != nil {
		return writeJSON(w, 400, map[string]string{"error": "请求格式错误"})
	}
	a.seq++
	pdus, err := buildSubmit(req.Number, req.Text, (a.seq%255)+1)
	if err != nil {
		return writeJSON(w, 400, map[string]string{"error": err.Error()})
	}
	for i, p := range pdus {
		if _, err := a.modem.SendPDU(p, req.Dry); err != nil {
			return writeJSON(w, 500, map[string]string{
				"error": fmt.Sprintf("第 %d/%d 条发送失败: %v", i+1, len(pdus), err)})
		}
	}
	if req.Dry {
		return writeJSON(w, 200, map[string]any{"ok": true, "dry": true, "parts": len(pdus),
			"note": "干跑通过：模块接受了 PDU，已在发送前取消"})
	}
	log.Printf("已发送到 %s（%d 条分片）", req.Number, len(pdus))
	ts := time.Now().Format("2006-01-02 15:04:05")
	key := fmt.Sprintf("%s|%s|%x|out", req.Number, ts, req.Text)
	a.store.mu.Lock()
	a.store.Msgs[key] = &Message{
		Key: key, Sender: req.Number, Body: req.Text, Time: ts,
		Status: "已发送", Direction: "out", Code: detectCode(req.Text),
	}
	a.store.mu.Unlock()
	a.store.Save()
	return writeJSON(w, 200, map[string]any{"ok": true, "parts": len(pdus)})
}

func (a *App) handleDelete(w *Resp, r *Req) error {
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
	// 这块模块只有 43MB 内存，尽量压低运行时开销
	runtime.GOMAXPROCS(1)
	debug.SetGCPercent(20)

	if err := os.MkdirAll(*flagData, 0755); err != nil {
		log.Fatalf("无法创建数据目录: %v", err)
	}
	if logf, err := os.OpenFile(filepath.Join(*flagData, "smsweb.log"),
		os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0644); err == nil {
		log.SetOutput(logf)
	}
	log.Printf("=== smsweb 启动 (AT=%s port=%s) ===", *flagAT, *flagPort)

	app := &App{store: LoadStore(*flagData), auth: LoadAuth(*flagData)}

	var modem *Modem
	var err error
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
	modem.SetNewSMSHandler(func() { app.pollOnce() })
	st := modem.Status()
	log.Printf("模块就绪: 运营商=%s 信号=%s SIM=%v", st.Carrier, st.Signal, st.SimReady)

	go app.pollLoop()
	go func() {
		time.Sleep(2 * time.Second)
		si := app.identity()
		if a := app.auth.Registered(si.Key); a {
			log.Printf("该 SIM 已注册访问密码 (ICCID=%s)", si.ICCID)
		} else {
			log.Printf("该 SIM 尚未注册访问密码，首次打开网页时需设置 (ICCID=%s)", si.ICCID)
		}
	}()

	routes := map[string]Handler{
		"GET /api/auth/status":  app.handleAuthStatus,
		"POST /api/auth/setup":  app.handleAuthSetup,
		"POST /api/auth/login":  app.handleAuthLogin,
		"POST /api/auth/logout": app.handleAuthLogout,
		"POST /api/auth/change": app.requireAuth(app.handleAuthChange),
		"GET /api/state":        app.requireAuth(app.handleState),
		"POST /api/send":        app.requireAuth(app.handleSend),
		"POST /api/delete":      app.requireAuth(app.handleDelete),
	}
	fallback := func(w *Resp, r *Req) error {
		if r.Path != "/" {
			return w.Write(404, "text/plain; charset=utf-8", []byte("not found"))
		}
		return w.Write(200, "text/html; charset=utf-8", []byte(indexHTML))
	}

	addr := ":" + *flagPort
	log.Printf("HTTP 监听 %s", addr)
	if err := ServeHTTP(addr, routes, fallback); err != nil {
		log.Fatalf("HTTP 启动失败: %v", err)
	}
}
