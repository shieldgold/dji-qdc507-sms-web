package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"path/filepath"
	"time"
)

type SMSBackup struct {
	Version    int        `json:"version"`
	ExportedAt string     `json:"exported_at"`
	Messages   []*Message `json:"messages"`
}

func (s *Store) RestoreBackup(b []byte) (int, error) {
	var backup SMSBackup
	if json.Unmarshal(b, &backup) != nil || backup.Version != 1 {
		return 0, errors.New("不支持的短信备份文件")
	}
	if len(backup.Messages) > 300 {
		return 0, errors.New("一次最多恢复 300 条短信")
	}
	for _, m := range backup.Messages {
		if m == nil || m.Key == "" || len(m.Key) > 24000 || len(m.Body) > 10000 || len(m.Sender) > 80 || (m.Direction != "in" && m.Direction != "out") {
			return 0, errors.New("短信备份内容不完整或格式错误")
		}
	}
	old := s.List()
	before, _ := json.Marshal(old)
	if e := atomicWrite(filepath.Join(filepath.Dir(s.path), "messages.before-restore.json"), before); e != nil {
		return 0, errors.New("无法保存恢复前备份，未修改短信")
	}
	added := []string{}
	s.mu.Lock()
	for _, m := range backup.Messages {
		if _, ok := s.Msgs[m.Key]; ok {
			continue
		}
		if len(s.Msgs) >= 300 {
			break
		}
		copy := *m
		copy.Index = 0 // Imported indexes must never delete current modem messages.
		if copy.SendState == "queued" || copy.SendState == "sending" {
			copy.SendState = "uncertain"
			copy.Status = "导入记录，发送结果待确认"
		}
		s.Msgs[copy.Key] = &copy
		added = append(added, copy.Key)
	}
	s.mu.Unlock()
	if e := s.Save(); e != nil {
		s.mu.Lock()
		for _, key := range added {
			delete(s.Msgs, key)
		}
		s.mu.Unlock()
		return 0, errors.New("恢复未保存，已撤销本次导入")
	}
	return len(added), nil
}
func (a *App) handleExport(w *Resp, r *Req) error {
	w.AddHeader("Content-Disposition", "attachment; filename=\"module-sms-backup.json\"")
	return writeJSON(w, 200, SMSBackup{Version: 1, ExportedAt: time.Now().UTC().Format(time.RFC3339), Messages: a.store.List()})
}
func (a *App) handleRestore(w *Resp, r *Req) error {
	a.actionMu.Lock()
	defer a.actionMu.Unlock()
	if a.outbox.Busy() {
		return writeJSON(w, 409, map[string]string{"error": "短信正在提交，暂时不能恢复备份"})
	}
	a.opMu.Lock()
	defer a.opMu.Unlock()
	n, e := a.store.RestoreBackup(r.Body)
	if e != nil {
		return writeJSON(w, 400, map[string]string{"error": e.Error()})
	}
	return writeJSON(w, 200, map[string]any{"added": n, "note": fmt.Sprintf("已合并 %d 条短信，原有短信保留；归档最多 300 条", n)})
}
