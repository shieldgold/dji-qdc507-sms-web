package main

import (
	"crypto/ecdsa"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"encoding/pem"
	"errors"
	"io"
	"math/big"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"testing"
	"time"
)

func testPushDelivery(kind string) PushDelivery {
	d := PushDelivery{Kind: kind, Token: strings.Repeat("a", 64), EventID: "12345678-1234-1234-1234-123456789abc", Count: 2, CallID: strings.Repeat("a", 24)}
	d.CallUUID = callPushUUID(d.CallID)
	return d
}
func testAPNSSender(t *testing.T, handler http.Handler, h2 bool) (*apnsSender, *ecdsa.PrivateKey) {
	t.Helper()
	dir, key := nativeTestConfig(t)
	server := httptest.NewUnstartedServer(handler)
	server.EnableHTTP2 = h2
	server.StartTLS()
	t.Cleanup(server.Close)
	if e := os.WriteFile(filepath.Join(dir, "push-ca.pem"), pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: server.Certificate().Raw}), 0600); e != nil {
		t.Fatal(e)
	}
	sender, e := loadAPNSSender(dir)
	if e != nil {
		t.Fatal(e)
	}
	sender.endpoint = server.URL
	sender.clockURL = server.URL + "/clock"
	sender.lastClockRefresh = time.Now()
	t.Cleanup(sender.client.CloseIdleConnections)
	return sender, key
}
func TestNativePushHTTP2PayloadsAndSignature(t *testing.T) {
	var public *ecdsa.PublicKey
	var kind string
	var previousToken string
	sender, key := testAPNSSender(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.ProtoMajor != 2 {
			t.Error("request did not use HTTP/2")
		}
		if r.URL.Path != "/3/device/"+strings.Repeat("a", 64) {
			t.Error("unexpected device path")
		}
		token := strings.TrimPrefix(r.Header.Get("Authorization"), "bearer ")
		parts := strings.Split(token, ".")
		if len(parts) != 3 {
			t.Error("invalid JWT")
			return
		}
		sig, e := base64.RawURLEncoding.DecodeString(parts[2])
		if e != nil || len(sig) != 64 {
			t.Error("invalid ES256 signature")
			return
		}
		hash := sha256.Sum256([]byte(parts[0] + "." + parts[1]))
		if !ecdsa.Verify(public, hash[:], new(big.Int).SetBytes(sig[:32]), new(big.Int).SetBytes(sig[32:])) {
			t.Error("signature was not valid")
		}
		header, _ := base64.RawURLEncoding.DecodeString(parts[0])
		claims, _ := base64.RawURLEncoding.DecodeString(parts[1])
		if !strings.Contains(string(header), `"kid":"ABC1234567"`) || !strings.Contains(string(claims), `"iss":"TEAM123456"`) {
			t.Error("wrong key identity")
		}
		if previousToken != "" && previousToken != token {
			t.Error("provider token was regenerated per event")
		}
		previousToken = token
		raw, _ := io.ReadAll(r.Body)
		if strings.Contains(string(raw), strings.Repeat("a", 64)) {
			t.Error("device token leaked into notification body")
		}
		if r.Header.Get("apns-id") != testPushDelivery(kind).EventID || r.Header.Get("apns-collapse-id") != testPushDelivery(kind).EventID {
			t.Error("unstable event id")
		}
		var payload map[string]any
		if json.Unmarshal(raw, &payload) != nil {
			t.Fatal("bad payload")
		}
		aps := payload["aps"].(map[string]any)
		if kind == "call" {
			if r.Header.Get("apns-topic") != "com.shield.modulephone.voip" || r.Header.Get("apns-push-type") != "voip" || r.Header.Get("apns-expiration") != "0" {
				t.Error("incorrect live-call policy")
			}
			if len(aps) != 0 || payload["call_uuid"] != callPushUUID(strings.Repeat("a", 24)) {
				t.Error("incorrect CallKit identifiers")
			}
		} else {
			if r.Header.Get("apns-topic") != "com.shield.modulephone" || r.Header.Get("apns-push-type") != "alert" || r.Header.Get("apns-expiration") == "0" {
				t.Error("incorrect SMS policy")
			}
			if aps["alert"] == nil || payload["call_id"] != nil {
				t.Error("incorrect SMS payload")
			}
		}
		w.WriteHeader(200)
	}), true)
	public = &key.PublicKey
	for _, kind = range []string{"sms", "test", "call"} {
		if e := sender.send(testPushDelivery(kind)); e != nil {
			t.Fatal(e)
		}
	}
}
func TestNativePushRejectsRedirectHTTP1AndUntrustedTLS(t *testing.T) {
	var calls atomic.Int32
	sender, _ := testAPNSSender(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls.Add(1)
		w.Header().Set("Location", "https://example.invalid/redirect")
		w.WriteHeader(302)
	}), true)
	if sender.send(testPushDelivery("test")) == nil || calls.Load() != 1 {
		t.Fatal("accepted or followed redirect")
	}
	sender, _ = testAPNSSender(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(200) }), false)
	if e := sender.send(testPushDelivery("test")); e == nil || !strings.Contains(e.Error(), "HTTP/2") {
		t.Fatal("accepted HTTP/1 response")
	}
	sender.client.Transport.(*http.Transport).TLSClientConfig.RootCAs = nil
	sender.client.CloseIdleConnections()
	if sender.send(testPushDelivery("test")) == nil {
		t.Fatal("accepted untrusted TLS certificate")
	}
}
func TestNativePushFailureClassification(t *testing.T) {
	for _, tc := range []struct {
		status    int
		reason    string
		permanent bool
	}{
		{400, "BadDeviceToken", true}, {410, "Unregistered", true}, {403, "InvalidProviderToken", true}, {403, "ExpiredProviderToken", false}, {429, "TooManyRequests", false}, {503, "ServiceUnavailable", false},
	} {
		t.Run(tc.reason, func(t *testing.T) {
			sender, _ := testAPNSSender(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				w.WriteHeader(tc.status)
				json.NewEncoder(w).Encode(map[string]string{"reason": tc.reason})
			}), true)
			e := sender.send(testPushDelivery("test"))
			var permanent *pushPermanentError
			if e == nil || errors.As(e, &permanent) != tc.permanent {
				t.Fatalf("wrong failure classification: %v", e)
			}
			if tc.reason == "ExpiredProviderToken" && sender.token != "" {
				t.Fatal("expired token retained")
			}
		})
	}
}
func TestNativePushProviderTokenCacheAndClockRollback(t *testing.T) {
	dir, _ := nativeTestConfig(t)
	s, e := loadAPNSSender(dir)
	if e != nil {
		t.Fatal(e)
	}
	now := time.Now()
	first, _ := s.providerToken(now)
	cached, _ := s.providerToken(now.Add(49 * time.Minute))
	next, _ := s.providerToken(now.Add(51 * time.Minute))
	back, _ := s.providerToken(now.Add(-time.Minute))
	if first != cached || first == next || next == back {
		t.Fatal("incorrect token lifetime or clock rollback handling")
	}
}
func TestNativePushValidatesPrivateConfigAndEvents(t *testing.T) {
	dir, _ := nativeTestConfig(t)
	os.Chmod(filepath.Join(dir, "push-apns.p8"), 0644)
	if _, ok := newPushTransport(dir); ok {
		t.Fatal("accepted world-readable private key")
	}
	os.Chmod(filepath.Join(dir, "push-apns.p8"), 0600)
	for _, raw := range []string{`{"key_id":"ABC1234567","team_id":"TEAM123456","topic":"other.app","environment":"development"}`, `{"key_id":"ABC1234567","team_id":"TEAM123456","topic":"com.shield.modulephone","environment":"production"}`} {
		os.WriteFile(filepath.Join(dir, "push-apns.json"), []byte(raw), 0600)
		if _, ok := newPushTransport(dir); ok {
			t.Fatal("accepted wrong scope")
		}
	}
	for _, kind := range []string{"silent", "sms", "call"} {
		d := testPushDelivery(kind)
		d.Count = 0
		d.CallUUID = "00000000-0000-0000-0000-000000000000"
		if _, e := apnsPayload(d); e == nil {
			t.Fatalf("accepted invalid %s event", kind)
		}
	}
}
func TestNativePushDoesNotLoadOldRelayConfiguration(t *testing.T) {
	dir := t.TempDir()
	os.WriteFile(filepath.Join(dir, "push-relay.json"), []byte(`{"url":"https://relay.invalid/v1/events","secret":"`+strings.Repeat("a", 64)+`"}`), 0600)
	if _, ok := newPushTransport(dir); ok {
		t.Fatal("obsolete relay was enabled")
	}
}
