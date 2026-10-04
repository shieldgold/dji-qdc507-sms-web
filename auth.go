package main

// auth.go — 访问密码（按 SIM 卡 ICCID 分别注册，每个号码一套密码）
//
// 设计：
//   · 身份 = SIM 卡的 ICCID（换卡就是新账号，与"每个号码不一样"一致）
//   · 密码 = PBKDF2-HMAC-SHA256（随机盐 + 20000 轮），只存哈希，永不存明文
//   · 登录后发一个随机 token 放到 HttpOnly Cookie
//       - 勾选"在本机记住登录" → token 有效期 1 年，并写入闪存，模块重启仍有效
//       - 不勾选             → 仅在浏览器会话内有效，服务端 12 小时后失效
//   · 连续输错 5 次锁定 60 秒，防暴力破解

import (
	"crypto/hmac"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	pbkdf2Iter  = 20000
	pbkdf2KeyLen = 32
	sessionTTL  = 12 * time.Hour
	rememberTTL = 365 * 24 * time.Hour
	maxFails    = 5
	lockoutTime = 60 * time.Second
	tokenName   = "smsweb_token"
	minPassword = 4
	maxPassword = 64
)

type Account struct {
	Identity  string `json:"identity"` // ICCID
	IMSI      string `json:"imsi,omitempty"`
	Number    string `json:"number,omitempty"`
	Salt      string `json:"salt"`
	Hash      string `json:"hash"`
	Iter      int    `json:"iter"`
	CreatedAt string `json:"createdAt"`
}

type Token struct {
	Identity string `json:"identity"`
	Expires  int64  `json:"expires"`
	Remember bool   `json:"remember"`
}

type failInfo struct {
	count int
	until time.Time
}

type AuthStore struct {
	mu       sync.Mutex
	path     string
	Accounts map[string]*Account `json:"accounts"`
	Tokens   map[string]*Token   `json:"tokens"`
	fails    map[string]*failInfo
}

func LoadAuth(dir string) *AuthStore {
	a := &AuthStore{
		path:     dir + "/auth.json",
		Accounts: map[string]*Account{},
		Tokens:   map[string]*Token{},
		fails:    map[string]*failInfo{},
	}
	if b, err := os.ReadFile(a.path); err == nil {
		_ = json.Unmarshal(b, a)
		if a.Accounts == nil {
			a.Accounts = map[string]*Account{}
		}
		if a.Tokens == nil {
			a.Tokens = map[string]*Token{}
		}
	}
	a.pruneLocked()
	return a
}

func (a *AuthStore) saveLocked() {
	b, err := json.MarshalIndent(struct {
		Accounts map[string]*Account `json:"accounts"`
		Tokens   map[string]*Token   `json:"tokens"`
	}{a.Accounts, a.Tokens}, "", " ")
	if err != nil {
		return
	}
	tmp := a.path + ".tmp"
	if err := os.WriteFile(tmp, b, 0600); err == nil {
		_ = os.Rename(tmp, a.path)
	}
}

func (a *AuthStore) pruneLocked() {
	now := time.Now().Unix()
	for k, t := range a.Tokens {
		if t.Expires < now {
			delete(a.Tokens, k)
		}
	}
}

// randomHex 从 /dev/urandom 取随机字节（不依赖 crypto/rand）
func randomHex(n int) string {
	b := make([]byte, n)
	ok := false
	if f, err := os.Open("/dev/urandom"); err == nil {
		if _, err := io.ReadFull(f, b); err == nil {
			ok = true
		}
		_ = f.Close()
	}
	if !ok {
		// 极端兜底：时间 + 计数器（模块一定有 /dev/urandom，这里只是不让它崩）
		seed := time.Now().UnixNano()
		for i := range b {
			seed = seed*6364136223846793005 + 1442695040888963407
			b[i] = byte(seed >> 33)
		}
	}
	return hex.EncodeToString(b)
}

// pbkdf2SHA256 是 PBKDF2-HMAC-SHA256 的最小实现（RFC 2898）。
// 之所以手写：Go 1.24+ 的 crypto/pbkdf2 会连带初始化 FIPS 140-3 模块，
// 而该初始化在这块模块的内核 3.18 上会直接段错误，只能用 Go 1.21 构建。
func pbkdf2SHA256(password, salt []byte, iter, keyLen int) []byte {
	hLen := sha256.Size
	blocks := (keyLen + hLen - 1) / hLen
	var dk []byte
	idx := make([]byte, 4)
	for block := 1; block <= blocks; block++ {
		mac := hmac.New(sha256.New, password)
		mac.Write(salt)
		idx[0] = byte(block >> 24)
		idx[1] = byte(block >> 16)
		idx[2] = byte(block >> 8)
		idx[3] = byte(block)
		mac.Write(idx)
		u := mac.Sum(nil)
		t := make([]byte, hLen)
		copy(t, u)
		for i := 1; i < iter; i++ {
			mac.Reset()
			mac.Write(u)
			u = mac.Sum(u[:0])
			for j := 0; j < hLen; j++ {
				t[j] ^= u[j]
			}
		}
		dk = append(dk, t...)
	}
	return dk[:keyLen]
}

func hashPassword(password, salt string, iter int) (string, error) {
	return hex.EncodeToString(pbkdf2SHA256([]byte(password), []byte(salt), iter, pbkdf2KeyLen)), nil
}

func (a *AuthStore) Registered(identity string) bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	_, ok := a.Accounts[identity]
	return ok
}

func (a *AuthStore) lockedOut(identity string) time.Duration {
	f := a.fails[identity]
	if f == nil || f.until.IsZero() {
		return 0
	}
	if d := time.Until(f.until); d > 0 {
		return d
	}
	f.count = 0
	f.until = time.Time{}
	return 0
}

// Setup 首次为该 SIM 注册密码
func (a *AuthStore) Setup(identity, imsi, number, password string, remember bool) (string, error) {
	if len([]rune(password)) < minPassword {
		return "", errors.New("密码至少 " + strconv.Itoa(minPassword) + " 位")
	}
	if len(password) > maxPassword {
		return "", errors.New("密码太长")
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	if _, ok := a.Accounts[identity]; ok {
		return "", errors.New("该号码已经注册过密码")
	}
	salt := randomHex(16)
	h, err := hashPassword(password, salt, pbkdf2Iter)
	if err != nil {
		return "", err
	}
	a.Accounts[identity] = &Account{
		Identity: identity, IMSI: imsi, Number: number,
		Salt: salt, Hash: h, Iter: pbkdf2Iter,
		CreatedAt: time.Now().Format("2006-01-02 15:04:05"),
	}
	tok, err := a.newTokenLocked(identity, remember)
	if err != nil {
		return "", err
	}
	a.saveLocked()
	return tok, nil
}

// Login 校验密码并发放 token
func (a *AuthStore) Login(identity, password string, remember bool) (string, error) {
	a.mu.Lock()
	defer a.mu.Unlock()
	if d := a.lockedOut(identity); d > 0 {
		return "", errors.New("密码错误次数过多，请 " + strconv.Itoa(int(d.Seconds())+1) + " 秒后再试")
	}
	acc := a.Accounts[identity]
	if acc == nil {
		return "", errors.New("该号码尚未注册密码")
	}
	h, err := hashPassword(password, acc.Salt, acc.Iter)
	if err != nil {
		return "", err
	}
	if subtle.ConstantTimeCompare([]byte(h), []byte(acc.Hash)) != 1 {
		f := a.fails[identity]
		if f == nil {
			f = &failInfo{}
			a.fails[identity] = f
		}
		f.count++
		if f.count >= maxFails {
			f.until = time.Now().Add(lockoutTime)
			f.count = 0
			return "", errors.New("密码错误次数过多，已锁定 " + strconv.Itoa(int(lockoutTime.Seconds())) + " 秒")
		}
		return "", errors.New("密码错误（还可尝试 " + strconv.Itoa(maxFails-f.count) + " 次）")
	}
	delete(a.fails, identity)
	tok, err := a.newTokenLocked(identity, remember)
	if err != nil {
		return "", err
	}
	a.saveLocked()
	return tok, nil
}

func (a *AuthStore) newTokenLocked(identity string, remember bool) (string, error) {
	tok := randomHex(32)
	ttl := sessionTTL
	if remember {
		ttl = rememberTTL
	}
	a.Tokens[tok] = &Token{Identity: identity, Expires: time.Now().Add(ttl).Unix(), Remember: remember}
	return tok, nil
}

// Check 校验 token 是否有效，并返回其是否属于该 SIM
func (a *AuthStore) Check(token, identity string) bool {
	if token == "" {
		return false
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	t := a.Tokens[token]
	if t == nil {
		return false
	}
	if t.Expires < time.Now().Unix() {
		delete(a.Tokens, token)
		return false
	}
	return t.Identity == identity
}

func (a *AuthStore) Logout(token string) {
	if token == "" {
		return
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	delete(a.Tokens, token)
	a.saveLocked()
}

// ChangePassword 修改密码（需已登录）
func (a *AuthStore) ChangePassword(identity, oldPw, newPw string) error {
	if len([]rune(newPw)) < minPassword {
		return errors.New("新密码至少 " + strconv.Itoa(minPassword) + " 位")
	}
	a.mu.Lock()
	acc := a.Accounts[identity]
	if acc == nil {
		a.mu.Unlock()
		return errors.New("该号码尚未注册密码")
	}
	salt, iter, oldHash := acc.Salt, acc.Iter, acc.Hash
	a.mu.Unlock()

	h, err := hashPassword(oldPw, salt, iter)
	if err != nil {
		return err
	}
	if subtle.ConstantTimeCompare([]byte(h), []byte(oldHash)) != 1 {
		return errors.New("原密码不正确")
	}

	newSalt := randomHex(16)
	nh, err := hashPassword(newPw, newSalt, pbkdf2Iter)
	if err != nil {
		return err
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	acc = a.Accounts[identity]
	if acc == nil {
		return errors.New("账号已不存在")
	}
	acc.Salt, acc.Hash, acc.Iter = newSalt, nh, pbkdf2Iter
	// 改密码后踢掉其他所有会话，只保留当前这个由调用方重新发放
	a.Tokens = map[string]*Token{}
	a.saveLocked()
	return nil
}

// IssueToken 修改密码后为当前会话重新发一个 token
func (a *AuthStore) IssueToken(identity string, remember bool) string {
	a.mu.Lock()
	defer a.mu.Unlock()
	tok, _ := a.newTokenLocked(identity, remember)
	a.saveLocked()
	return tok
}

func cookieValue(hdr, name string) string {
	for _, part := range strings.Split(hdr, ";") {
		part = strings.TrimSpace(part)
		if i := strings.IndexByte(part, '='); i > 0 && part[:i] == name {
			return part[i+1:]
		}
	}
	return ""
}

func cookieHeader(token string, maxAge int) string {
	c := tokenName + "=" + token + "; Path=/; HttpOnly; SameSite=Lax"
	if maxAge > 0 {
		// 勾选"在本机记住登录"才写 Max-Age；否则是会话 Cookie，关掉浏览器即失效
		c += "; Max-Age=" + strconv.Itoa(maxAge)
	}
	return c
}

func cookieClear() string {
	return tokenName + "=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0"
}
