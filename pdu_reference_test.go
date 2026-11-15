package main

// Frozen reference from 138c1c43b20eeaa3b74c4212e89b3d4816876616.
// Keep the old allocation-heavy implementation for synthetic differential tests.
import (
	"encoding/hex"
	"errors"
	"fmt"
	"strings"
)

func referenceGsm7DecodeFrom(data []byte, skip, septets int) string {
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

func referenceGsm7Encode(s string) ([]byte, int, bool) {
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

func referencePackBits(udh []byte, packed []byte, septets int) []byte {
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

func referenceSplitGSM7(text string, max int) []string {
	var segs []string
	cur := ""
	for _, r := range text {
		cand := cur + string(r)
		if _, n, ok := referenceGsm7Encode(cand); ok && n <= max {
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

func referenceBuildSubmit(number, text string, ref int) ([]string, error) {
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
	if len(digits) > 20 {
		return nil, errors.New("号码最多 20 位数字")
	}
	if len([]rune(text)) > 1600 {
		return nil, errors.New("短信内容过长，请分成多条发送")
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

	_, _, isGSM7 := referenceGsm7Encode(text)

	if isGSM7 {
		pk, n, _ := referenceGsm7Encode(text)
		if n <= 160 {
			return []string{mkPDU("01", "00", n, pk)}, nil
		}
		segs := referenceSplitGSM7(text, 153)
		total := len(segs)
		if total > 10 {
			return nil, errors.New("一条消息最多拆分为 10 条短信，请缩短内容")
		}
		var out []string
		for i, s := range segs {
			b, n, _ := referenceGsm7Encode(s)
			udh := []byte{0x05, 0x00, 0x03, byte(ref), byte(total), byte(i + 1)}
			udhSeptets := (len(udh)*8 + 6) / 7
			combined := referencePackBits(udh, b, n)
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
	if total > 10 {
		return nil, errors.New("一条消息最多拆分为 10 条短信，请缩短内容")
	}
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
