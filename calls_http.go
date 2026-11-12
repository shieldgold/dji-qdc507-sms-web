package main

import "encoding/json"

func (a *App) handleCalls(w *Resp, r *Req) error {
	return writeJSON(w, 200, map[string]any{"telephone": a.calls.Snapshot(), "audio": a.callAudio.Status(), "background_incoming": false})
}

func (a *App) handleCallDial(w *Resp, r *Req) error {
	a.actionMu.Lock()
	defer a.actionMu.Unlock()
	if a.maintenance || a.outbox.Busy() {
		return writeJSON(w, 409, map[string]string{"error": "正在处理短信或维护，请稍后拨号"})
	}
	if a.callAudio == nil || !a.callAudio.Status().Available {
		return writeJSON(w, 409, map[string]string{"error": "模块音频尚未准备好，未拨号"})
	}
	var body struct {
		Number    string `json:"number"`
		RequestID string `json:"request_id"`
	}
	if json.Unmarshal(r.Body, &body) != nil {
		return writeJSON(w, 400, map[string]string{"error": "请求格式错误"})
	}
	s, err := a.calls.Dial(body.Number, body.RequestID)
	if err != nil {
		return writeJSON(w, 409, map[string]any{"error": err.Error(), "telephone": s})
	}
	return writeJSON(w, 202, s)
}

func (a *App) handleCallAction(w *Resp, r *Req, action string) error {
	a.actionMu.Lock()
	defer a.actionMu.Unlock()
	var body struct {
		ID    string `json:"id"`
		Digit string `json:"digit"`
	}
	if json.Unmarshal(r.Body, &body) != nil || body.ID == "" {
		return writeJSON(w, 400, map[string]string{"error": "缺少当前通话标识"})
	}
	var err error
	switch action {
	case "answer":
		if !a.callAudio.Status().Available {
			return writeJSON(w, 409, map[string]string{"error": "模块音频尚未准备好，未接听"})
		}
		err = a.calls.Answer(body.ID)
	case "hangup":
		err = a.calls.Hangup(body.ID)
	case "dtmf":
		err = a.calls.DTMF(body.ID, body.Digit)
	}
	if err != nil {
		return writeJSON(w, 409, map[string]string{"error": err.Error()})
	}
	return writeJSON(w, 202, map[string]bool{"accepted": true})
}

func (a *App) handleCallAnswer(w *Resp, r *Req) error { return a.handleCallAction(w, r, "answer") }
func (a *App) handleCallHangup(w *Resp, r *Req) error { return a.handleCallAction(w, r, "hangup") }
func (a *App) handleCallDTMF(w *Resp, r *Req) error   { return a.handleCallAction(w, r, "dtmf") }

func (a *App) handleCallAudio(w *Resp, r *Req) error {
	if !a.callAudio.Status().Available {
		return writeJSON(w, 409, map[string]string{"error": "模块语音运行时尚未准备好"})
	}
	ticket := a.callAudio.issueTicket()
	if ticket == "" {
		return writeJSON(w, 409, map[string]string{"error": "请等待当前电话状态确认后连接音频"})
	}
	return writeJSON(w, 200, map[string]any{"ticket": ticket, "port": 8788, "sample_rate": 8000, "channels": 1, "format": "s16le", "frame_bytes": 320, "expires_in": 30})
}
