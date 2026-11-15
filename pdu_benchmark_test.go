package main

import (
	"strings"
	"testing"
)

var pduBenchmarkText string
var pduBenchmarkPDUs []string

func BenchmarkGSM7Decode(b *testing.B) {
	packed, n, _ := gsm7Encode(strings.Repeat("A", 160))
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		pduBenchmarkText = gsm7DecodeFrom(packed, 0, n)
	}
}
func BenchmarkPDUBuildSubmit(b *testing.B) {
	for _, tc := range []struct{ name, text string }{
		{"GSM7-160", strings.Repeat("A", 160)},
		{"GSM7-161", strings.Repeat("A", 161)},
		{"GSM7-1530", strings.Repeat("A", 1530)},
		{"GSM7-ext-760", strings.Repeat("^", 760)},
		{"UCS2-670", strings.Repeat("中", 670)},
	} {
		b.Run(tc.name, func(b *testing.B) {
			b.ReportAllocs()
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				var err error
				pduBenchmarkPDUs, err = buildSubmit("1234567890", tc.text, 1)
				if err != nil {
					b.Fatal(err)
				}
			}
		})
	}
}
