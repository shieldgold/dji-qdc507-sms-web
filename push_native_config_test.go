package main

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/x509"
	"encoding/json"
	"encoding/pem"
	"os"
	"path/filepath"
	"testing"
)

func nativeTestConfig(t *testing.T) (string, *ecdsa.PrivateKey) {
	t.Helper()
	dir := t.TempDir()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	der, err := x509.MarshalPKCS8PrivateKey(key)
	if err != nil {
		t.Fatal(err)
	}
	if err = os.WriteFile(filepath.Join(dir, "push-apns.p8"), pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: der}), 0600); err != nil {
		t.Fatal(err)
	}
	raw, _ := json.Marshal(map[string]string{"key_id": "ABC1234567", "team_id": "TEAM123456", "topic": "com.shield.modulephone", "environment": "development"})
	if err = os.WriteFile(filepath.Join(dir, "push-apns.json"), raw, 0600); err != nil {
		t.Fatal(err)
	}
	return dir, key
}
func TestNativePushLoadsScopedPrivateConfiguration(t *testing.T) {
	dir, _ := nativeTestConfig(t)
	_, configured := newPushTransport(dir)
	if !configured {
		t.Fatal("native APNs configuration was not loaded")
	}
}
