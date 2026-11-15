package main

import (
	"encoding/hex"
	"errors"
	"fmt"
	"reflect"
	"strings"
	"testing"
	"time"
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

func TestGSM7DecodeAllocationBudget(t *testing.T) {
	packed, septets, _ := referenceGsm7Encode(strings.Repeat("A", 160))
	allocs := testing.AllocsPerRun(100, func() { pduBenchmarkText = gsm7DecodeFrom(packed, 0, septets) })
	if allocs > 2 {
		t.Fatalf("GSM7 decode allocates %.0f objects for 160 characters; want <= 2", allocs)
	}
}

func TestLongGSM7SubmitAllocationBudget(t *testing.T) {
	text := strings.Repeat("A", 1530)
	allocs := testing.AllocsPerRun(10, func() {
		var err error
		pduBenchmarkPDUs, err = buildSubmit("1234567890", text, 1)
		if err != nil {
			t.Fatal(err)
		}
	})
	if allocs > 250 {
		t.Fatalf("10-part GSM7 submit allocates %.0f objects; want <= 250", allocs)
	}
	if len(pduBenchmarkPDUs) != 10 {
		t.Fatal("allocation budget changed the 10-part output")
	}
}

func TestGSM7DecoderMatchesOriginal(t *testing.T) {
	exts := "\f^{}\\[~]|€"
	inputs := [][]byte{nil, {0}, {0xff}, {0x1b}, {0x1b, 0}, {0x1b, 0x7f}}
	for _, text := range []string{gsm7Basic, exts, strings.Repeat("A", 160), "A^€@\r\nB"} {
		packed, _, _ := referenceGsm7Encode(text)
		inputs = append(inputs, packed)
	}
	for _, data := range inputs {
		for skip := 0; skip < 10; skip++ {
			for _, n := range []int{-1, 0, 1, 2, 7, 8, 127, 160, 255} {
				got, want := gsm7DecodeFrom(data, skip, n), referenceGsm7DecodeFrom(data, skip, n)
				if got != want {
					t.Fatalf("decoder changed for bytes=%x skip=%d septets=%d", data, skip, n)
				}
			}
		}
	}
	packed, n, _ := referenceGsm7Encode(gsm7Basic + exts)
	for _, udh := range [][]byte{{5, 0, 3, 7, 2, 1}, {6, 8, 4, 0x12, 0x34, 2, 1}} {
		combined := referencePackBits(udh, packed, n)
		skip := (len(udh)*8 + 6) / 7
		if got, want := gsm7DecodeFrom(combined, skip, n), referenceGsm7DecodeFrom(combined, skip, n); got != want {
			t.Fatal("GSM7 UDH offset changed decoded text")
		}
	}
}

func TestGSM7EncoderAndPackingMatchOriginal(t *testing.T) {
	exts := "\f^{}\\[~]|€"
	texts := []string{"", gsm7Basic, exts, gsm7Basic + exts, "中文😀", string([]byte{0xff}), strings.Repeat("A", 1530)}
	for _, r := range gsm7Basic + exts {
		texts = append(texts, string(r))
	}
	for _, text := range texts {
		got, n, ok := gsm7Encode(text)
		want, wn, wok := referenceGsm7Encode(text)
		if !reflect.DeepEqual(got, want) || n != wn || ok != wok {
			t.Fatalf("encoder differs for synthetic input %q", text)
		}
		for _, max := range []int{0, 1, 2, 7, 152, 153, 160} {
			if got, want := splitGSM7(text, max), referenceSplitGSM7(text, max); !reflect.DeepEqual(got, want) {
				t.Fatalf("GSM7 split differs for max=%d input %q", max, text)
			}
		}
	}
	headers := [][]byte{nil, {}, {0}, {5, 0, 3, 1, 2, 1}, {6, 8, 4, 0x12, 0x34, 2, 1}}
	for size := 1; size <= 14; size++ {
		header := make([]byte, size)
		for i := range header {
			header[i] = byte(0xa5 + i)
		}
		headers = append(headers, header)
	}
	for _, udh := range headers {
		for _, data := range [][]byte{nil, {0}, {0xff}, {0x1b, 0x7f, 0x5a, 0x91}} {
			for _, n := range []int{-1, 0, 1, 2, 7, 8, 10, 160} {
				if got, want := packBits(udh, data, n), referencePackBits(udh, data, n); !reflect.DeepEqual(got, want) {
					t.Fatalf("UDH packing changed: udh=%x data=%x septets=%d", udh, data, n)
				}
			}
		}
	}
}

func TestSubmitPDUBytesMatchOriginalAtBoundaries(t *testing.T) {
	exts := "\f^{}\\[~]|€"
	texts := []string{"", " \t\n", gsm7Basic + exts, strings.Repeat(gsm7Basic+exts, 7), "中文和emoji😀", string([]byte{0xff, 0xfe, 'A'})}
	for _, n := range []int{1, 152, 153, 159, 160, 161, 305, 306, 307, 1529, 1530, 1531, 1600, 1601} {
		texts = append(texts, strings.Repeat("A", n))
	}
	for _, n := range []int{76, 77, 79, 80, 81, 759, 760, 761, 764, 765, 766} {
		texts = append(texts, strings.Repeat("^", n))
	}
	for _, n := range []int{66, 67, 70, 71, 669, 670, 671} {
		texts = append(texts, strings.Repeat("中", n))
	}
	for _, n := range []int{33, 34, 35, 36, 329, 330, 331} {
		texts = append(texts, strings.Repeat("😀", n))
	}
	texts = append(texts, strings.Repeat("A", 152)+"^B", strings.Repeat("A", 151)+"€AB", strings.Repeat("中", 66)+"😀中")
	for i, text := range texts {
		t.Run(fmt.Sprint(i), func(t *testing.T) {
			for _, number := range []string{"10010", " +8613800138000 ", "12345678901234567890", "", "+", "123x"} {
				for _, ref := range []int{1, 255} {
					got, err := buildSubmit(number, text, ref)
					want, werr := referenceBuildSubmit(number, text, ref)
					if !reflect.DeepEqual(got, want) || fmt.Sprint(err) != fmt.Sprint(werr) {
						t.Fatalf("PDU bytes/errors changed for number=%q ref=%d", number, ref)
					}
				}
			}
		})
	}
}

func TestGSM7RoundTripCompletePrintableAlphabet(t *testing.T) {
	text := strings.ReplaceAll(gsm7Basic, "\x1b", "") + "\f^{}\\[~]|€"
	text = strings.Repeat(text, 5)
	pdus, err := buildSubmit("12345", text, 255)
	if err != nil {
		t.Fatal(err)
	}
	var got strings.Builder
	for _, pdu := range pdus {
		_, body, err := parseSubmitForTest(pdu)
		if err != nil {
			t.Fatal(err)
		}
		got.WriteString(body)
	}
	if got.String() != text {
		t.Fatal("complete printable GSM7 alphabet failed multipart round trip")
	}
}

func TestGSM7DeliverKeepsTimestampAndMessageIdentity(t *testing.T) {
	packed, n, _ := referenceGsm7Encode(gsm7Basic + "\f^{}\\[~]|€")
	for _, tz := range []struct {
		hex, name string
		offset    int
	}{
		{"00", "UTC+00:00", 0},
		{"23", "UTC+08:00", 8 * 3600},
		// Preserve the original name even for a negative offset below one hour.
		{"28", "UTC+00:30", -30 * 60},
		{"69", "UTC-04:00", -4 * 3600},
		{"1A", "UTC-05:15", -5*3600 - 15*60},
	} {
		for _, udh := range [][]byte{nil, {5, 0, 3, 7, 2, 1}, {6, 8, 4, 0x12, 0x34, 2, 1}} {
			ptype, udl, payload := "00", n, packed
			skip := 0
			if len(udh) > 0 {
				ptype = "40"
				skip = (len(udh)*8 + 6) / 7
				udl += skip
				payload = referencePackBits(udh, packed, n)
			}
			raw := "00" + ptype + "0A8121436587090000" + "621151800000" + tz.hex + fmt.Sprintf("%02X", udl) + hex.EncodeToString(payload)
			got, err := parseDeliver(raw)
			if err != nil {
				t.Fatal(err)
			}
			name, offset := got.t.Zone()
			if name != tz.name || offset != tz.offset || got.t.Format("2006-01-02 15:04:05") != "2026-11-15 08:00:00" {
				t.Fatalf("deliver timestamp/timezone changed for %s", tz.hex)
			}
			if !got.t.Equal(time.Date(2026, 11, 15, 8, 0, 0, 0, time.FixedZone(tz.name, tz.offset))) {
				t.Fatal("deliver timestamp instant changed")
			}
			got.index, got.status = 7, "0"
			want := *got
			want.body = referenceGsm7DecodeFrom(payload, skip, n)
			if !reflect.DeepEqual(mergeParts([]*part{got}), mergeParts([]*part{&want})) {
				t.Fatal("decode changed merged body, timestamp, key or verification code")
			}
		}
	}
}
