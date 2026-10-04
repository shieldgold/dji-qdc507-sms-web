package main

import (
	"errors"
	"encoding/hex"
	"strings"
	"testing"
)

// 合成的 SMS-DELIVER PDU 测试向量：一条两片的长短信、UCS2 编码、含验证码。
// 完全按 GSM 03.40 构造，覆盖 UDH 分片拼接、UCS2 正文、数字发件人、验证码提取。
const pduPartA = "0891683108000000F0400881019600000008620140210591233E0500032A02013010793A4F8B79D16280301160A876849A8C8BC17801662F0020003600350034003300320031FF0C003500205206949F5185670965483002"
const pduPartB = "0891683108000000F040088101960000000862014021059123220500032A02025982975E672C4EBA64CD4F5CFF0C8BF75FFD7565672C77ED4FE13002"

func TestParseDeliver(t *testing.T) {
	p0, err := parseDeliver(pduPartA)
	if err != nil {
		t.Fatalf("分片1 解析失败: %v", err)
	}
	if p0.sender != "10690000" {
		t.Errorf("分片1 发件人 = %q, 期望 \"10690000\"", p0.sender)
	}
	if !strings.Contains(p0.body, "验证码") {
		t.Errorf("分片1 正文 = %q, 期望包含 验证码", p0.body)
	}
	if p0.total != 2 || p0.seq != 1 {
		t.Errorf("分片1 分片信息 = %d/%d, 期望 1/2", p0.seq, p0.total)
	}
	if got := detectCode(p0.body); got != "654321" {
		t.Errorf("验证码提取 = %q, 期望 654321", got)
	}

	p1, err := parseDeliver(pduPartB)
	if err != nil {
		t.Fatalf("分片2 解析失败: %v", err)
	}
	if p1.sender != "10690000" {
		t.Errorf("分片2 发件人 = %q, 期望 \"10690000\"", p1.sender)
	}
	if !strings.Contains(p1.body, "忽略本短信") {
		t.Errorf("分片2 正文 = %q, 期望包含 忽略本短信", p1.body)
	}
	if p1.total != 2 || p1.seq != 2 {
		t.Errorf("分片2 分片信息 = %d/%d, 期望 2/2", p1.seq, p1.total)
	}
	t.Logf("分片1 正文: %s", p0.body)
	t.Logf("分片2 正文: %s", p1.body)
}

func TestMergeConcat(t *testing.T) {
	p0, _ := parseDeliver(pduPartA)
	p1, _ := parseDeliver(pduPartB)
	msgs := mergeParts([]*part{p0, p1})
	if len(msgs) != 1 {
		t.Fatalf("期望合并为 1 条，得到 %d 条", len(msgs))
	}
	b := msgs[0].Body
	if !strings.HasPrefix(b, "【示例科技】") {
		t.Errorf("合并后正文顺序不对（应按 seq 升序）: %q", b[:min(20, len(b))])
	}
	if !strings.Contains(b, "忽略本短信") {
		t.Errorf("合并后缺少后半段: %q", b)
	}
	if msgs[0].Code != "654321" {
		t.Errorf("合并后验证码 = %q, 期望 654321", msgs[0].Code)
	}
	t.Logf("合并后正文: %s", b)
}

// 反解 SMS-SUBMIT，用于验证编码正确性
func parseSubmitForTest(h string) (num, body string, err error) {
	pos := 0
	take := func(n int) string {
		if pos+n > len(h) {
			n = len(h) - pos
		}
		s := h[pos : pos+n]
		pos += n
		return s
	}
	scaLen := hexInt(take(2))
	if scaLen > 0 {
		take(scaLen * 2)
	}
	ptype := hexInt(take(2))
	if ptype&0x03 != 1 {
		return "", "", errors.New("不是 SMS-SUBMIT 类型")
	}
	take(2) // MR
	daLen := hexInt(take(2))
	take(2) // da toa
	daHex := take(((daLen + 1) / 2) * 2)
	num = bcdNumber(daHex)
	take(2) // PID
	dcs := hexInt(take(2))
	udl := hexInt(take(2))
	ud, _ := hex.DecodeString(padEven(h[pos:]))
	udhLen := 0
	if ptype&0x40 != 0 && len(ud) > 0 {
		udhLen = 1 + int(ud[0])
	}
	text := ud
	if udhLen > 0 && len(ud) > udhLen {
		text = ud[udhLen:]
	}
	switch (dcs >> 2) & 0x03 {
	case 2:
		n := udl - udhLen
		if n < 0 {
			n = 0
		}
		if n > len(text) {
			n = len(text)
		}
		b := text[:n]
		if len(b)%2 == 1 {
			b = b[:len(b)-1]
		}
		u := make([]uint16, 0, len(b)/2)
		for i := 0; i+1 < len(b); i += 2 {
			u = append(u, uint16(b[i])<<8|uint16(b[i+1]))
		}
		body = string(utf16Decode(u))
	default:
		udhSeptets := (udhLen*8 + 6) / 7
		sept := udl - udhSeptets
		if sept < 0 {
			sept = 0
		}
		body = gsm7DecodeFrom(ud, udhSeptets, sept)
	}
	return
}

func TestSubmitChineseSingle(t *testing.T) {
	text := "验证码 123456，5 分钟内有效"
	pdus, err := buildSubmit("+8613800138000", text, 1)
	if err != nil {
		t.Fatal(err)
	}
	if len(pdus) != 1 {
		t.Fatalf("期望 1 条 PDU，得到 %d", len(pdus))
	}
	num, body, err := parseSubmitForTest(pdus[0])
	if err != nil {
		t.Fatal(err)
	}
	if num != "8613800138000" {
		t.Errorf("号码往返 = %q", num)
	}
	if body != text {
		t.Errorf("正文往返 = %q, 期望 %q", body, text)
	}
}

func TestSubmitASCII(t *testing.T) {
	text := "123456"
	pdus, err := buildSubmit("10010", text, 2)
	if err != nil {
		t.Fatal(err)
	}
	num, body, err := parseSubmitForTest(pdus[0])
	if err != nil {
		t.Fatal(err)
	}
	if num != "10010" {
		t.Errorf("号码往返 = %q", num)
	}
	if body != text {
		t.Errorf("纯数字正文往返 = %q, 期望 %q（这正是文本模式会出错的场景）", body, text)
	}
	if detectCode(body) != "123456" {
		t.Errorf("验证码识别失败: %q", detectCode(body))
	}
}

func TestSubmitLongChinese(t *testing.T) {
	text := strings.Repeat("这是一条很长的中文测试短信，用来验证长短信分片是否正确。", 6)
	pdus, err := buildSubmit("10010", text, 7)
	if err != nil {
		t.Fatal(err)
	}
	if len(pdus) < 2 {
		t.Fatalf("长短信应被分片，得到 %d 条", len(pdus))
	}
	var sb strings.Builder
	for _, p := range pdus {
		_, b, err := parseSubmitForTest(p)
		if err != nil {
			t.Fatal(err)
		}
		sb.WriteString(b)
	}
	if sb.String() != text {
		t.Errorf("长短信往返不一致\n得到: %q\n期望: %q", sb.String(), text)
	}
}

func TestSubmitLongGSM7(t *testing.T) {
	text := strings.Repeat("Hello world, this is a test. ", 12)
	pdus, err := buildSubmit("10010", text, 9)
	if err != nil {
		t.Fatal(err)
	}
	if len(pdus) < 2 {
		t.Fatalf("长短信应被分片，得到 %d 条", len(pdus))
	}
	var sb strings.Builder
	for _, p := range pdus {
		_, b, err := parseSubmitForTest(p)
		if err != nil {
			t.Fatal(err)
		}
		sb.WriteString(b)
	}
	if sb.String() != text {
		got := sb.String()
		n := 0
		for n < len(got) && n < len(text) && got[n] == text[n] {
			n++
		}
		t.Errorf("GSM7 长短信往返不一致（首个差异在第 %d 字符）\n得到: %q\n期望: %q", n, got, text)
	}
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}
