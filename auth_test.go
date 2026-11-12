package main

import (
	"encoding/hex"
	"os"
	"testing"
)

func newTestAuth(t *testing.T) *AuthStore {
	t.Helper()
	return LoadAuth(t.TempDir())
}

func TestSetupAndLogin(t *testing.T) {
	a := newTestAuth(t)
	id := "iccid:89012345678901234567"

	if a.Registered(id) {
		t.Fatal("新账号不应已注册")
	}
	tok, err := a.Setup(id, "460001234567890", "+8613800138000", "hunter42", true)
	if err != nil {
		t.Fatalf("注册失败: %v", err)
	}
	if tok == "" {
		t.Fatal("注册后应返回 token")
	}
	if !a.Registered(id) {
		t.Fatal("注册后应标记为已注册")
	}
	if !a.Check(tok, id) {
		t.Fatal("注册返回的 token 应有效")
	}
	if a.Check(tok, "iccid:另一张卡") {
		t.Fatal("token 不应在别的卡上有效")
	}
}

func TestPasswordNotStoredInPlaintext(t *testing.T) {
	a := newTestAuth(t)
	id := "iccid:1"
	if _, err := a.Setup(id, "", "", "MySecret123", false); err != nil {
		t.Fatal(err)
	}
	acc := a.Accounts[id]
	if acc.Hash == "MySecret123" || acc.Salt == "" {
		t.Fatal("密码不能明文存储，且必须有盐")
	}
	if _, err := a.Login(id, "MySecret123", false); err != nil {
		t.Fatalf("正确密码应登录成功: %v", err)
	}
}

func TestWrongPassword(t *testing.T) {
	a := newTestAuth(t)
	id := "iccid:2"
	if _, err := a.Setup(id, "", "", "correct-horse", false); err != nil {
		t.Fatal(err)
	}
	_, err := a.Login(id, "wrong-password", false)
	if err == nil {
		t.Fatal("错误密码不应登录成功")
	}
	t.Logf("错误提示: %v", err)
}

func TestLockoutAfterRepeatedFailures(t *testing.T) {
	a := newTestAuth(t)
	id := "iccid:3"
	if _, err := a.Setup(id, "", "", "abcdef", false); err != nil {
		t.Fatal(err)
	}
	var lastErr error
	for i := 0; i < maxFails; i++ {
		_, lastErr = a.Login(id, "nope", false)
	}
	if lastErr == nil {
		t.Fatal("连续错误应有报错")
	}
	// 锁定后即使密码正确也应被拒绝
	if _, err := a.Login(id, "abcdef", false); err == nil {
		t.Fatal("锁定期间即使密码正确也应拒绝")
	} else {
		t.Logf("锁定提示: %v", err)
	}
}

func TestPerSimIsolation(t *testing.T) {
	a := newTestAuth(t)
	idA, idB := "iccid:AAA", "iccid:BBB"
	if _, err := a.Setup(idA, "", "", "aaaa11", false); err != nil {
		t.Fatal(err)
	}
	if _, err := a.Setup(idB, "", "", "bbbb22", false); err != nil {
		t.Fatal(err)
	}
	if _, err := a.Login(idA, "bbbb22", false); err == nil {
		t.Fatal("A 卡不应该能用 B 卡的密码登录")
	}
	if _, err := a.Login(idA, "aaaa11", false); err != nil {
		t.Fatalf("A 卡用自己密码应能登录: %v", err)
	}
	if _, err := a.Login(idB, "bbbb22", false); err != nil {
		t.Fatalf("B 卡用自己密码应能登录: %v", err)
	}
}

func TestPasswordTooShort(t *testing.T) {
	a := newTestAuth(t)
	if _, err := a.Setup("iccid:4", "", "", "123", false); err == nil {
		t.Fatal("少于 4 位应被拒绝")
	}
}

func TestDuplicateSetupRejected(t *testing.T) {
	a := newTestAuth(t)
	id := "iccid:5"
	if _, err := a.Setup(id, "", "", "abcd", false); err != nil {
		t.Fatal(err)
	}
	if _, err := a.Setup(id, "", "", "efgh", false); err == nil {
		t.Fatal("重复注册应被拒绝（否则可被用来改掉别人的密码）")
	}
}

func TestSessionVsRememberedTTL(t *testing.T) {
	a := newTestAuth(t)
	id := "iccid:6"
	tokSession, _ := a.Setup(id, "", "", "abcd", false)
	tokRemember, _ := a.Login(id, "abcd", true)

	s := a.Tokens[tokSession]
	r := a.Tokens[tokRemember]
	if s.Remember || r.Remember != true {
		t.Fatal("已记住/未记住的标记不对")
	}
	// 记住的过期时间应远大于会话
	if r.Expires-s.Expires < 300*24*3600 {
		t.Fatalf("记住登录的有效期应远长于会话: session=%d remember=%d", s.Expires, r.Expires)
	}
}

func TestLogoutInvalidatesToken(t *testing.T) {
	a := newTestAuth(t)
	id := "iccid:7"
	tok, _ := a.Setup(id, "", "", "abcd", false)
	a.Logout(tok)
	if a.Check(tok, id) {
		t.Fatal("退出后 token 应失效")
	}
}

func TestPersistence(t *testing.T) {
	dir := t.TempDir()
	a := LoadAuth(dir)
	id := "iccid:8"
	tok, _ := a.Setup(id, "46001", "+8613800000000", "abcd", true)

	b := LoadAuth(dir) // 模拟模块重启
	if !b.Registered(id) {
		t.Fatal("重启后账号应仍在")
	}
	if _, err := b.Login(id, "abcd", false); err != nil {
		t.Fatalf("重启后应能用原密码登录: %v", err)
	}
	if !b.Check(tok, id) {
		t.Fatal("勾选记住登录时，token 应在重启后仍有效")
	}
}

func TestChangePassword(t *testing.T) {
	a := newTestAuth(t)
	id := "iccid:9"
	if _, err := a.Setup(id, "", "", "oldpass", false); err != nil {
		t.Fatal(err)
	}
	if err := a.ChangePassword(id, "wrongold", "newpass"); err == nil {
		t.Fatal("原密码错误时应拒绝")
	}
	if err := a.ChangePassword(id, "oldpass", "newpass"); err != nil {
		t.Fatalf("修改密码应成功: %v", err)
	}
	if _, err := a.Login(id, "oldpass", false); err == nil {
		t.Fatal("旧密码应失效")
	}
	if _, err := a.Login(id, "newpass", false); err != nil {
		t.Fatalf("新密码应可登录: %v", err)
	}
}

// 手写 PBKDF2 必须对上标准测试向量（PBKDF2-HMAC-SHA256）
func TestPBKDF2KnownVectors(t *testing.T) {
	cases := []struct {
		pw, salt string
		iter     int
		want     string
	}{
		{"password", "salt", 1, "120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b"},
		{"password", "salt", 2, "ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43"},
		{"password", "salt", 4096, "c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a"},
	}
	for _, c := range cases {
		got := hex.EncodeToString(pbkdf2SHA256([]byte(c.pw), []byte(c.salt), c.iter, 32))
		if got != c.want {
			t.Errorf("PBKDF2(%q,%q,%d)\n 得到 %s\n 期望 %s", c.pw, c.salt, c.iter, got, c.want)
		}
	}
}

func TestCookieParsing(t *testing.T) {
	hdr := "foo=bar; smsweb_token=deadbeef; other=1"
	if got := cookieValue(hdr, tokenName); got != "deadbeef" {
		t.Fatalf("解析 Cookie 失败: %q", got)
	}
	if got := cookieValue("", tokenName); got != "" {
		t.Fatal("空 Cookie 应返回空串")
	}
	c := cookieHeader("abc", 100)
	if !contains(c, "Max-Age=100") || !contains(c, "HttpOnly") {
		t.Fatalf("记住登录的 Cookie 头不对: %s", c)
	}
	c2 := cookieHeader("abc", 0)
	if contains(c2, "Max-Age") {
		t.Fatalf("不记住时不应写 Max-Age（应为会话 Cookie）: %s", c2)
	}
}

func contains(s, sub string) bool {
	return len(s) >= len(sub) && (s == sub || indexOf(s, sub) >= 0)
}

func indexOf(s, sub string) int {
	for i := 0; i+len(sub) <= len(s); i++ {
		if s[i:i+len(sub)] == sub {
			return i
		}
	}
	return -1
}

func TestAuthWriteFailureDoesNotClaimRegistration(t *testing.T) {
	a := newTestAuth(t)
	a.path = t.TempDir() // a directory cannot replace an auth file
	if _, e := a.Setup("test", "", "", "abcd", true); e == nil {
		t.Fatal("setup succeeded without durable data")
	}
	if a.Registered("test") || len(a.Tokens) != 0 {
		t.Fatal("failed setup left usable credentials")
	}
}
func TestCorruptAuthIsNotOverwritten(t *testing.T) {
	dir := t.TempDir()
	file := dir + "/auth.json"
	if e := os.WriteFile(file, []byte(""), 0600); e != nil {
		t.Fatal(e)
	}
	a := LoadAuth(dir)
	if _, e := a.Setup("test", "", "", "abcd", true); e == nil {
		t.Fatal("overwrote unreadable account archive")
	}
	b, _ := os.ReadFile(file)
	if len(b) != 0 {
		t.Fatal("original file changed")
	}
}
func TestPasswordChangeWriteFailureRetainsOldPassword(t *testing.T) {
	a := newTestAuth(t)
	_, e := a.Setup("test", "", "", "old-password", true)
	if e != nil {
		t.Fatal(e)
	}
	original := a.path
	a.path = t.TempDir()
	if e := a.ChangePassword("test", "old-password", "new-password"); e == nil {
		t.Fatal("password update succeeded without durable data")
	}
	a.path = original
	if _, e := a.Login("test", "old-password", false); e != nil {
		t.Fatal("old password lost")
	}
}
