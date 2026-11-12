package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestNetworkParsingDistinguishesSignalFromRegistration(t *testing.T) {
	n := NetworkSnapshot{}
	applyNetworkLine(&n, "+QNWINFO: \"FDD LTE\",\"46011\",\"LTE BAND 3\",1850")
	applyNetworkLine(&n, "+QCSQ: \"LTE\",59,-86,212,-5")
	applyNetworkLine(&n, "+CEREG: 0,2")
	if n.Registered || n.Registration != "正在搜索网络" || n.Band != "LTE BAND 3" || n.RSRP != "-86 dBm" {
		t.Fatalf("%+v", n)
	}
	applyNetworkLine(&n, "+CEREG: 0,5")
	if !n.Registered {
		t.Fatal("roaming registered")
	}
	applyNetworkLine(&n, "+QCSQ: \"NOSERVICE\"")
	if n.RSRP != "" {
		t.Fatal("stale signal")
	}
}
func TestTrafficCountersPersistAndHandleBootAndMidnight(t *testing.T) {
	dir := t.TempDir()
	tr := LoadTraffic(dir)
	now := time.Date(2026, 11, 12, 23, 59, 50, 0, time.FixedZone("CST", 8*3600))
	tr.Sample("boot-a", 1000, 2000, now)
	tr.Sample("boot-a", 1200, 2300, now.Add(5*time.Second))
	if tr.View(now).Today.Upload != 200 || tr.View(now).Today.Download != 300 {
		t.Fatal("baseline counted as new traffic")
	}
	if e := tr.Save(); e != nil {
		t.Fatal(e)
	}
	tr = LoadTraffic(dir)
	tr.Sample("boot-a", 1300, 2400, now.Add(15*time.Second))
	v := tr.View(now.Add(15 * time.Second))
	if v.Today.Upload != 100 || v.Month.Upload != 300 {
		t.Fatalf("day %d month %d", v.Today.Upload, v.Month.Upload)
	}
	tr.Sample("boot-b", 40, 50, now.Add(20*time.Second))
	v = tr.View(now.Add(20 * time.Second))
	if v.Total.Upload != 340 || v.Session.Upload != 40 {
		t.Fatalf("new boot %+v", v)
	}
	tr.Sample("boot-b", 5, 6, now.Add(25*time.Second))
	if tr.View(now).Total.Upload != 345 {
		t.Fatal("counter reset underflow")
	}
}
func TestBackupMergeNeverDeletesOrResends(t *testing.T) {
	s := LoadStore(t.TempDir())
	s.Msgs["keep"] = &Message{Key: "keep", Body: "keep", Sender: "10010", Direction: "in"}
	_ = s.Save()
	data, _ := json.Marshal(SMSBackup{Version: 1, Messages: []*Message{{Key: "import", Sender: "10010", Body: "test", Direction: "out", SendState: "sending", RequestID: "request-backup"}}})
	n, e := s.RestoreBackup(data)
	if e != nil || n != 1 {
		t.Fatal(n, e)
	}
	if len(s.List()) != 2 || s.Get("import").SendState != "uncertain" {
		t.Fatal("restore deleted or queued messages")
	}
	if _, e := os.Stat(filepath.Join(filepath.Dir(s.path), "messages.before-restore.json")); e != nil {
		t.Fatal("backup before restore missing")
	}
	n, e = s.RestoreBackup(data)
	if e != nil || n != 0 {
		t.Fatal("duplicate import")
	}
	if _, e = s.RestoreBackup([]byte(`{"version":999,"messages":[]}`)); e == nil {
		t.Fatal("accepted unknown backup version")
	}
	bad := strings.Replace(string(data), `"direction":"out"`, `"direction":"invalid"`, 1)
	if _, e = s.RestoreBackup([]byte(bad)); e == nil {
		t.Fatal("accepted malformed message")
	}
}

func TestIMSConfigurationNeverClaimsRegistration(t *testing.T) {
	n := NetworkSnapshot{}
	applyNetworkLine(&n, `+QCFG: "ims",0,1`)
	if !strings.Contains(n.IMS, "自动") || !strings.Contains(n.IMS, "注册状态待验证") || strings.Contains(n.IMS, "已注册") {
		t.Fatalf("capability confused with registration: %s", n.IMS)
	}
}
