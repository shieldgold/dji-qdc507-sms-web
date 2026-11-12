package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"syscall"
)

func (a *App) handleRollback(w *Resp, r *Req) error {
	a.actionMu.Lock()
	defer a.actionMu.Unlock()
	if a.maintenance || a.outbox.Busy() || a.calls.Busy() {
		return writeJSON(w, 409, map[string]string{"error": "正在处理短信、通话或维护，请完成后再恢复版本"})
	}
	dir := filepath.Dir(a.store.path)
	if dir != "/data/smsweb" {
		return writeJSON(w, 400, map[string]string{"error": "此环境不支持恢复模块版本"})
	}
	if st, e := os.Stat(filepath.Join(dir, "previous/smsweb")); e != nil || st.Size() == 0 {
		return writeJSON(w, 400, map[string]string{"error": "没有可恢复的旧版本"})
	}
	if e := a.monitor.traffic.Save(); e != nil {
		return writeJSON(w, 500, map[string]string{"error": "流量统计未能保存，已取消维护"})
	}
	cmd := exec.Command("/bin/sh", filepath.Join(dir, "restore-version.sh"))
	cmd.SysProcAttr = &syscall.SysProcAttr{Setsid: true}
	if e := cmd.Start(); e != nil {
		return writeJSON(w, 500, map[string]string{"error": "无法启动恢复程序"})
	}
	a.maintenance = true
	go cmd.Wait()
	return writeJSON(w, 202, map[string]bool{"ok": true})
}
