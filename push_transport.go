package main

import (
	"bytes"
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/sha256"
	"crypto/tls"
	"crypto/x509"
	"encoding/base64"
	"encoding/json"
	"encoding/pem"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"sync"
	"time"
)

type pushPermanentError struct{ reason string }

func (e *pushPermanentError) Error() string { return e.reason }

type apnsConfig struct {
	KeyID       string `json:"key_id"`
	TeamID      string `json:"team_id"`
	Topic       string `json:"topic"`
	Environment string `json:"environment"`
}
type apnsSender struct {
	mu                 sync.Mutex
	config             apnsConfig
	key                *ecdsa.PrivateKey
	client             *http.Client
	endpoint, clockURL string
	token              string
	tokenAt            time.Time
	lastClockRefresh   time.Time
}

var apnsIDPattern = regexp.MustCompile(`^[A-Z0-9]{10}$`)
var apnsUUIDPattern = regexp.MustCompile(`^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$`)
var apnsCallPattern = regexp.MustCompile(`^[a-f0-9]{24}$`)

func privatePushFile(path string) ([]byte, error) {
	info, e := os.Lstat(path)
	if e != nil {
		return nil, e
	}
	if !info.Mode().IsRegular() || info.Mode().Perm()&0077 != 0 {
		return nil, errors.New("推送私有配置权限必须为 0600")
	}
	return os.ReadFile(path)
}
func loadAPNSSender(dir string) (*apnsSender, error) {
	var config apnsConfig
	raw, e := privatePushFile(filepath.Join(dir, "push-apns.json"))
	if e != nil || json.Unmarshal(raw, &config) != nil {
		return nil, errors.New("苹果直连推送尚未配置")
	}
	// The installed app and scoped signing key both use the development topic.
	// Do not permit a configuration file to redirect credentials to another host.
	if !apnsIDPattern.MatchString(config.KeyID) || !apnsIDPattern.MatchString(config.TeamID) || config.Topic != "com.shield.modulephone" || config.Environment != "development" {
		return nil, errors.New("苹果推送配置与当前 App 不匹配")
	}
	raw, e = privatePushFile(filepath.Join(dir, "push-apns.p8"))
	if e != nil {
		return nil, errors.New("苹果推送私钥不可用或权限不正确")
	}
	block, _ := pem.Decode(raw)
	if block == nil {
		return nil, errors.New("苹果推送私钥格式无效")
	}
	parsed, e := x509.ParsePKCS8PrivateKey(block.Bytes)
	if e != nil {
		return nil, errors.New("苹果推送私钥格式无效")
	}
	key, ok := parsed.(*ecdsa.PrivateKey)
	if !ok || key.Curve != elliptic.P256() {
		return nil, errors.New("苹果推送私钥必须使用 P-256")
	}
	var roots *x509.CertPool
	if certs, err := os.ReadFile(filepath.Join(dir, "push-ca.pem")); err == nil {
		roots = x509.NewCertPool()
		if !roots.AppendCertsFromPEM(certs) {
			return nil, errors.New("推送信任证书无效")
		}
	}
	dialer := &net.Dialer{Timeout: 8 * time.Second}
	if runtime.GOOS == "linux" {
		// Firmware DNS listens on bridge0, not loopback; resolv.conf can be empty.
		dialer.Resolver = &net.Resolver{PreferGo: true, Dial: func(ctx context.Context, network, address string) (net.Conn, error) {
			return (&net.Dialer{Timeout: 3 * time.Second}).DialContext(ctx, network, "192.168.225.1:53")
		}}
	}
	client := &http.Client{Timeout: 12 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }, Transport: &http.Transport{
		DialContext: dialer.DialContext, TLSClientConfig: &tls.Config{MinVersion: tls.VersionTLS12, RootCAs: roots, Time: applicationTime.Now},
		ForceAttemptHTTP2: true, DisableCompression: true, MaxIdleConns: 2, MaxIdleConnsPerHost: 1, IdleConnTimeout: 5 * time.Minute, ResponseHeaderTimeout: 10 * time.Second,
	}}
	return &apnsSender{config: config, key: key, client: client, endpoint: "https://api.sandbox.push.apple.com", clockURL: "https://www.apple.com/library/test/success.html"}, nil
}
func newPushTransport(dir string) (func(PushDelivery) error, bool) {
	sender, e := loadAPNSSender(dir)
	if e != nil {
		return func(PushDelivery) error { return e }, false
	}
	return sender.send, true
}
func (s *apnsSender) providerToken(now time.Time) (string, error) {
	if s.token != "" && !now.Before(s.tokenAt) && now.Sub(s.tokenAt) < 50*time.Minute {
		return s.token, nil
	}
	header, _ := json.Marshal(map[string]string{"alg": "ES256", "kid": s.config.KeyID})
	claims, _ := json.Marshal(map[string]any{"iss": s.config.TeamID, "iat": now.Unix()})
	encode := base64.RawURLEncoding.EncodeToString
	prefix := encode(header) + "." + encode(claims)
	hash := sha256.Sum256([]byte(prefix))
	r, v, e := ecdsa.Sign(rand.Reader, s.key, hash[:])
	if e != nil {
		return "", errors.New("苹果推送签名失败")
	}
	signature := make([]byte, 64)
	r.FillBytes(signature[:32])
	v.FillBytes(signature[32:])
	s.token = prefix + "." + encode(signature)
	s.tokenAt = now.Truncate(time.Second)
	return s.token, nil
}
func (s *apnsSender) syncResponseTime(resp *http.Response) {
	// Only read this after a TLS-verified request to a pinned Apple endpoint.
	if date, e := http.ParseTime(resp.Header.Get("Date")); e == nil {
		if applicationTime.Sync(date.UnixMilli()) == nil {
			s.lastClockRefresh = time.Now()
		}
	}
}
func (s *apnsSender) refreshClock(ctx context.Context) {
	if time.Since(s.lastClockRefresh) < 10*time.Minute {
		return
	}
	ctx, cancel := context.WithTimeout(ctx, 3*time.Second)
	defer cancel()
	req, e := http.NewRequestWithContext(ctx, "GET", s.clockURL, nil)
	if e != nil {
		return
	}
	resp, e := s.client.Do(req)
	if e != nil {
		return
	}
	defer resp.Body.Close()
	if resp.StatusCode == 200 {
		s.syncResponseTime(resp)
	}
	_, _ = io.Copy(io.Discard, io.LimitReader(resp.Body, 1024))
}
func apnsPayload(d PushDelivery) ([]byte, error) {
	if !pushTokenPattern.MatchString(d.Token) || len(d.Token)%2 != 0 || !apnsUUIDPattern.MatchString(d.EventID) {
		return nil, &pushPermanentError{"推送事件格式无效"}
	}
	if d.Kind == "call" {
		if !apnsCallPattern.MatchString(d.CallID) || d.CallUUID != callPushUUID(d.CallID) {
			return nil, &pushPermanentError{"来电推送标识无效"}
		}
		return json.Marshal(map[string]any{"aps": map[string]any{}, "call_id": d.CallID, "call_uuid": d.CallUUID, "event_id": d.EventID})
	}
	title := "小电波 · 后台推送测试"
	if d.Kind == "sms" {
		if d.Count < 1 || d.Count > 300 {
			return nil, &pushPermanentError{"短信推送数量无效"}
		}
		title = "模块收到新短信"
		if d.Count > 1 {
			title = fmt.Sprintf("模块收到 %d 条新短信", d.Count)
		}
	} else if d.Kind != "test" {
		return nil, &pushPermanentError{"推送类型无效"}
	}
	return json.Marshal(map[string]any{"aps": map[string]any{"alert": map[string]string{"title": title, "body": "点按查看模块。"}, "sound": "default", "thread-id": "module-sms"}, "destination": "inbox", "event_id": d.EventID})
}
func (s *apnsSender) send(d PushDelivery) error {
	raw, e := apnsPayload(d)
	if e != nil {
		return e
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	if applicationTime.Now().Year() < 2024 {
		return errors.New("请打开 App 校准通知时间")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 12*time.Second)
	defer cancel()
	s.refreshClock(ctx)
	now := applicationTime.Now()
	token, e := s.providerToken(now)
	if e != nil {
		return e
	}
	req, e := http.NewRequestWithContext(ctx, "POST", s.endpoint+"/3/device/"+d.Token, bytes.NewReader(raw))
	if e != nil {
		return errors.New("苹果推送请求创建失败")
	}
	req.Header.Set("Authorization", "bearer "+token)
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("apns-topic", s.config.Topic)
	req.Header.Set("apns-push-type", "alert")
	req.Header.Set("apns-priority", "10")
	req.Header.Set("apns-expiration", strconv.FormatInt(now.Unix()+300, 10))
	req.Header.Set("apns-id", d.EventID)
	req.Header.Set("apns-collapse-id", d.EventID)
	if d.Kind == "call" {
		req.Header.Set("apns-topic", s.config.Topic+".voip")
		req.Header.Set("apns-push-type", "voip")
		req.Header.Set("apns-expiration", "0")
	}
	resp, e := s.client.Do(req)
	if e != nil {
		return errors.New("苹果推送连接失败，请检查模块上网状态")
	}
	defer resp.Body.Close()
	if resp.ProtoMajor != 2 {
		return errors.New("苹果推送未建立 HTTP/2 连接")
	}
	s.syncResponseTime(resp)
	data, e := io.ReadAll(io.LimitReader(resp.Body, 8192))
	if e != nil {
		return errors.New("苹果推送结果未确认")
	}
	if resp.StatusCode == 200 {
		return nil
	}
	var result struct {
		Reason string `json:"reason"`
	}
	_ = json.Unmarshal(data, &result)
	switch result.Reason {
	case "BadDeviceToken", "DeviceTokenNotForTopic", "Unregistered":
		return &pushPermanentError{"苹果推送令牌已失效，请重新打开 App 登记"}
	case "ExpiredProviderToken":
		s.token = ""
		s.lastClockRefresh = time.Time{}
		return errors.New("苹果推送签名已过期，正在重新校准")
	case "InvalidProviderToken", "MissingProviderToken", "TopicDisallowed":
		return &pushPermanentError{"苹果拒绝推送签名，请检查专用密钥与 App 配置"}
	}
	if resp.StatusCode >= 400 && resp.StatusCode < 500 && resp.StatusCode != 429 {
		return &pushPermanentError{"苹果拒绝推送，请检查推送配置"}
	}
	return errors.New("苹果推送暂未成功，将稍后重试")
}
