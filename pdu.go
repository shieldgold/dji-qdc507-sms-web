package main

// pdu.go — GSM 03.40 PDU 编解码（短信收发核心）
//
// 为什么用 PDU 模式而不是文本模式：文本模式下模块把中文正文输出成「十六进制 UCS2」，
// 而纯 ASCII 的验证码（如 "123456"）本身也是合法十六进制串，无法可靠区分 —— 那正是最需要
// 准确的场景。PDU 里编码方式由 DCS 字段显式给出，绝无歧义。

import (
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"strconv"
	"strings"
	"time"
)

const gsm7Basic = "@\u00a3$\u00a5\u00e8\u00e9\u00f9\u00ec\u00f2\u00c7\n\u00d8\u00f8\r\u00c5\u00e5\u0394_\u03a6\u0393\u039b\u03a9\u03a0\u03a8\u03a3\u0398\u039e\u001b\u00c6\u00e6\u00df\u00c9 !\"#\u00a4%&'()*+,-./0123456789:;<=>?\u00a1ABCDEFGHIJKLMNOPQRSTUVWXYZ\u00c4\u00d6\u00d1\u00dc\u00a7\u00bfabcdefghijklmnopqrstuvwxyz\u00e4\u00f6\u00f1\u00fc\u00e0"

var gsm7ExtMap = map[int]rune{
	0x0A: '\f', 0x14: '^', 0x28: '{', 0x29: '}', 0x2F: '\\',
	0x3C: '[', 0x3D: '~', 0x3E: ']', 0x40: '|', 0x65: '\u20ac',
}

var gsm7Rev = func() map[rune]int {
	m := make(map[rune]int)
	for i, r := range []rune(gsm7Basic) {
		m[r] = i
	}
	return m
}()

var gsm7ExtRev = func() map[rune]int {
	m := make(map[rune]int)
	for k, v := range gsm7ExtMap {
		m[v] = k
	}
	return m
}()

// ---------------- 解码 ----------------

func gsm7Decode(data []byte, septets int) string {
	return gsm7DecodeFrom(data, 0, septets)
}

// gsm7DecodeFrom 从第 skip 个 septet 开始解码
// （GSM7 正文前面带 UDH 时，正文并不从字节边界开始）
func gsm7DecodeFrom(data []byte, skip, septets int) string {
	basic := []rune(gsm7Basic)
	var out []rune
	pos := skip * 7
	read := func() int {
		v := 0
		for b := 0; b < 7; b++ {
			idx := pos + b
			if idx/8 < len(data) && (data[idx/8]>>uint(idx%8))&1 == 1 {
				v |= 1 << uint(b)
			}
		}
		pos += 7
		return v
	}
	for i := 0; i < septets; i++ {
		v := read()
		if v == 0x1b && i+1 < septets {
			v2 := read()
			i++
			if r, ok := gsm7ExtMap[v2]; ok {
				out = append(out, r)
			}
			continue
		}
		if v < len(basic) {
			out = append(out, basic[v])
		} else {
			out = append(out, '?')
		}
	}
	return string(out)
}

// BCD 半字节交换的电话号码（F 为填充）
func bcdNumber(h string) string {
	var sb strings.Builder
	for i := 0; i+1 < len(h); i += 2 {
		lo, hi := h[i], h[i+1]
		if hi != 'F' && hi != 'f' {
			sb.WriteByte(hi)
		}
		if lo != 'F' && lo != 'f' {
			sb.WriteByte(lo)
		}
	}
	return sb.String()
}

func parseSCTS(h string) time.Time {
	b, err := hex.DecodeString(h)
	if err != nil || len(b) < 7 {
		return time.Time{}
	}
	dec := func(x byte) int { return int(x&0x0f)*10 + int((x>>4)&0x0f) }
	yy, mo, dd := dec(b[0]), dec(b[1]), dec(b[2])
	hh, mi, ss := dec(b[3]), dec(b[4]), dec(b[5])
	tzRaw := b[6]
	sign := 1
	tz := int(tzRaw&0x07)*10 + int((tzRaw>>4)&0x07)
	if tzRaw&0x08 != 0 {
		sign = -1
	}
	year := 2000 + yy
	if yy >= 90 {
		year = 1900 + yy
	}
	offset := sign * tz * 15 * 60
	loc := time.FixedZone(fmt.Sprintf("UTC%+03d:%02d", offset/3600, abs(offset/60)%60), offset)
	return time.Date(year, time.Month(mo), dd, hh, mi, ss, 0, loc)
}

func abs(x int) int {
	if x < 0 {
		return -x
	}
	return x
}

// Message 一条已解码的短信
type Message struct {
	Key       string `json:"key"`
	Index     int    `json:"index"`
	Sender    string `json:"sender"`
	Body      string `json:"body"`
	Time      string `json:"time"`
	Status    string `json:"status"`
	Code      string `json:"code,omitempty"`
	Direction string `json:"direction"`
	Concat    string `json:"concat,omitempty"`
}

type part struct {
	ref, total, seq int
	sender, body    string
	t               time.Time
	index           int
	status          string
}

func padEven(s string) string {
	if len(s)%2 == 1 {
		return s + "0"
	}
	return s
}

func utf16Decode(u []uint16) []rune {
	var out []rune
	for i := 0; i < len(u); i++ {
		c := u[i]
		if c >= 0xD800 && c < 0xDC00 && i+1 < len(u) {
			c2 := u[i+1]
			if c2 >= 0xDC00 && c2 < 0xE000 {
				out = append(out, rune(0x10000+(int32(c)-0xD800)*0x400+(int32(c2)-0xDC00)))
				i++
				continue
			}
		}
		out = append(out, rune(c))
	}
	return out
}

func utf16Encode(s string) []uint16 {
	var out []uint16
	for _, r := range s {
		if r > 0xFFFF {
			r -= 0x10000
			out = append(out, uint16(0xD800+(r>>10)), uint16(0xDC00+(r&0x3FF)))
		} else {
			out = append(out, uint16(r))
		}
	}
	return out
}

// parseDeliver 解析一条 SMS-DELIVER PDU
func parseDeliver(h string) (*part, error) {
	h = strings.ToUpper(strings.TrimSpace(h))
	pos := 0
	take := func(n int) (string, error) {
		if n < 0 || pos+n > len(h) {
			return "", io.ErrUnexpectedEOF
		}
		s := h[pos : pos+n]
		pos += n
		return s, nil
	}
	scaLenHex, err := take(2)
	if err != nil {
		return nil, err
	}
	scaLen := hexInt(scaLenHex)
	// SCA 长度字段不含自身；为 0 时后面既没有 TOA 也没有地址
	if scaLen > 0 {
		if _, err = take(scaLen * 2); err != nil {
			return nil, err
		}
	}
	ptHex, err := take(2)
	if err != nil {
		return nil, err
	}
	ptype := hexInt(ptHex)
	if ptype&0x03 != 0 {
		return nil, errors.New("不是 SMS-DELIVER")
	}
	oaLenHex, err := take(2)
	if err != nil {
		return nil, err
	}
	oaLen := hexInt(oaLenHex)
	oaToaHex, err := take(2)
	if err != nil {
		return nil, err
	}
	oaToa := hexInt(oaToaHex)
	oaHex, err := take(((oaLen + 1) / 2) * 2)
	if err != nil {
		return nil, err
	}
	var sender string
	if (oaToa>>4)&0x07 == 0x05 {
		b, _ := hex.DecodeString(oaHex)
		sender = gsm7Decode(b, oaLen)
	} else {
		sender = bcdNumber(oaHex)
	}
	if _, err = take(2); err != nil { // PID
		return nil, err
	}
	dcsHex, err := take(2)
	if err != nil {
		return nil, err
	}
	dcs := hexInt(dcsHex)
	sctsHex, err := take(14)
	if err != nil {
		return nil, err
	}
	udlHex, err := take(2)
	if err != nil {
		return nil, err
	}
	udl := hexInt(udlHex)
	ud, err := hex.DecodeString(padEven(h[pos:]))
	if err != nil {
		return nil, err
	}

	p := &part{sender: sender, t: parseSCTS(sctsHex), ref: -1, total: 1, seq: 1}

	var udh []byte
	textOct := ud
	if ptype&0x40 != 0 && len(ud) > 0 {
		l := int(ud[0])
		if len(ud) >= 1+l {
			udh = ud[:1+l]
			textOct = ud[1+l:]
		}
	}
	if len(udh) >= 6 && udh[1] == 0x00 && udh[2] == 0x03 {
		p.ref = int(udh[3])
		p.total = int(udh[4])
		p.seq = int(udh[5])
	} else if len(udh) >= 7 && udh[1] == 0x08 && udh[2] == 0x04 {
		p.ref = int(udh[3])<<8 | int(udh[4])
		p.total = int(udh[5])
		p.seq = int(udh[6])
	}

	switch (dcs >> 2) & 0x03 {
	case 2: // UCS2
		n := udl - len(udh)
		if n < 0 || n > len(textOct) {
			n = len(textOct)
		}
		b := textOct[:n]
		if len(b)%2 == 1 {
			b = b[:len(b)-1]
		}
		u := make([]uint16, 0, len(b)/2)
		for i := 0; i+1 < len(b); i += 2 {
			u = append(u, uint16(b[i])<<8|uint16(b[i+1]))
		}
		p.body = string(utf16Decode(u))
	case 1: // 8bit
		n := udl - len(udh)
		if n < 0 || n > len(textOct) {
			n = len(textOct)
		}
		p.body = string(textOct[:n])
	default: // GSM7
		// UDH 与正文共享位流：UDH 占位到 7 位边界后，正文从下一个 septet 开始
		udhSeptets := (len(udh)*8 + 6) / 7
		n := udl - udhSeptets
		if n < 0 {
			n = 0
		}
		p.body = gsm7DecodeFrom(ud, udhSeptets, n)
	}
	return p, nil
}

// ---------------- 发送 ----------------

// gsm7Encode 返回打包字节、septet 数、是否可编码
func gsm7Encode(s string) ([]byte, int, bool) {
	var septets []int
	for _, r := range s {
		if idx, ok := gsm7Rev[r]; ok {
			septets = append(septets, idx)
		} else if code, ok := gsm7ExtRev[r]; ok {
			septets = append(septets, 0x1b, code)
		} else {
			return nil, 0, false
		}
	}
	out := make([]byte, (len(septets)*7+7)/8)
	bit := 0
	for _, v := range septets {
		for b := 0; b < 7; b++ {
			if (v>>uint(b))&1 == 1 {
				out[bit/8] |= 1 << uint(bit%8)
			}
			bit++
		}
	}
	return out, len(septets), true
}

// packBits 把 UDH 字节与 GSM7 打包文本按位拼接（UDH 填充到 7 位边界后接正文）
func packBits(udh []byte, packed []byte, septets int) []byte {
	var bits []int
	for _, b := range udh {
		for i := 0; i < 8; i++ {
			bits = append(bits, int((b>>uint(i))&1))
		}
	}
	for len(bits)%7 != 0 {
		bits = append(bits, 0)
	}
	udhSeptets := len(bits) / 7
	_ = udhSeptets
	pos := 0
	for i := 0; i < septets; i++ {
		v := 0
		for b := 0; b < 7; b++ {
			idx := pos + b
			if idx/8 < len(packed) && (packed[idx/8]>>uint(idx%8))&1 == 1 {
				v |= 1 << uint(b)
			}
		}
		pos += 7
		for b := 0; b < 7; b++ {
			bits = append(bits, (v>>uint(b))&1)
		}
	}
	out := make([]byte, (len(bits)+7)/8)
	for i, bit := range bits {
		if bit == 1 {
			out[i/8] |= 1 << uint(i%8)
		}
	}
	return out
}

func encodeAddress(num string) string {
	num = strings.TrimPrefix(strings.TrimSpace(num), "+")
	if len(num)%2 == 1 {
		num += "F"
	}
	var sb strings.Builder
	for i := 0; i+1 < len(num); i += 2 {
		sb.WriteByte(num[i+1])
		sb.WriteByte(num[i])
	}
	return sb.String()
}

func splitGSM7(text string, max int) []string {
	var segs []string
	cur := ""
	for _, r := range text {
		cand := cur + string(r)
		if _, n, ok := gsm7Encode(cand); ok && n <= max {
			cur = cand
			continue
		}
		if cur != "" {
			segs = append(segs, cur)
		}
		cur = string(r)
	}
	if cur != "" {
		segs = append(segs, cur)
	}
	return segs
}

func splitUCS2(text string, maxUnits int) []string {
	var segs []string
	cur := ""
	curUnits := 0
	for _, r := range text {
		u := 1
		if r > 0xFFFF {
			u = 2
		}
		if curUnits+u > maxUnits && cur != "" {
			segs = append(segs, cur)
			cur, curUnits = "", 0
		}
		cur += string(r)
		curUnits += u
	}
	if cur != "" {
		segs = append(segs, cur)
	}
	return segs
}

// buildSubmit 生成一条或多条 SMS-SUBMIT PDU（长短信自动 UDH 分片）
// ref 为本次分片的参考号（1..255）
func buildSubmit(number, text string, ref int) ([]string, error) {
	if strings.TrimSpace(text) == "" {
		return nil, errors.New("短信内容为空")
	}
	raw := strings.TrimSpace(number)
	digits := strings.TrimPrefix(raw, "+")
	for _, c := range digits {
		if c < '0' || c > '9' {
			return nil, fmt.Errorf("号码含非法字符: %q", number)
		}
	}
	if digits == "" {
		return nil, errors.New("号码为空")
	}
	toa := "81"
	if strings.HasPrefix(raw, "+") {
		toa = "91"
	}
	addr := encodeAddress(digits)

	// SMS-SUBMIT TPDU：
	//   SCA长度(00) + PDU类型 + MR(00) + 目的号码长度 + 号码类型 + 号码 + PID(00) + DCS + UDL + 正文
	//   PDU 类型 0x01 = 单条；0x41 = 带 UDH（长短信分片，UDHI 位必须置 1）
	mkPDU := func(ptype, dcs string, udl int, payload []byte) string {
		return fmt.Sprintf("00%s00%02X%s%s00%s%02X%s",
			ptype, len(digits), toa, addr, dcs, udl, strings.ToUpper(hex.EncodeToString(payload)))
	}

	_, _, isGSM7 := gsm7Encode(text)

	if isGSM7 {
		pk, n, _ := gsm7Encode(text)
		if n <= 160 {
			return []string{mkPDU("01", "00", n, pk)}, nil
		}
		segs := splitGSM7(text, 153)
		total := len(segs)
		var out []string
		for i, s := range segs {
			b, n, _ := gsm7Encode(s)
			udh := []byte{0x05, 0x00, 0x03, byte(ref), byte(total), byte(i + 1)}
			udhSeptets := (len(udh)*8 + 6) / 7
			combined := packBits(udh, b, n)
			out = append(out, mkPDU("41", "00", n+udhSeptets, combined))
		}
		return out, nil
	}

	// UCS2
	u := utf16Encode(text)
	payload := make([]byte, 0, len(u)*2)
	for _, c := range u {
		payload = append(payload, byte(c>>8), byte(c))
	}
	if len(payload) <= 140 {
		return []string{mkPDU("01", "08", len(payload), payload)}, nil
	}
	segs := splitUCS2(text, 134/2)
	total := len(segs)
	var out []string
	for i, s := range segs {
		uu := utf16Encode(s)
		seg := make([]byte, 0, len(uu)*2)
		for _, c := range uu {
			seg = append(seg, byte(c>>8), byte(c))
		}
		udh := []byte{0x05, 0x00, 0x03, byte(ref), byte(total), byte(i + 1)}
		combined := append(append([]byte{}, udh...), seg...)
		out = append(out, mkPDU("41", "08", len(combined), combined))
	}
	return out, nil
}

// hexInt 把十六进制字符串转成 int（PDU 所有长度/类型字段都是十六进制）
func hexInt(s string) int {
	v, _ := strconv.ParseInt(strings.TrimSpace(s), 16, 32)
	return int(v)
}
