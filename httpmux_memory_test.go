package main

import (
	"bufio"
	"bytes"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"strings"
	"testing"
	"time"
)

// Exercise the real connection handler without timing socket scheduling or
// retaining a second copy of every response in the allocation benchmarks.
type httpMemoryConn struct {
	input    strings.Reader
	output   io.Writer
	maxRead  int
	closed   bool
	deadline time.Time
}

func (c *httpMemoryConn) Read(p []byte) (int, error) {
	if c.maxRead > 0 && len(p) > c.maxRead {
		p = p[:c.maxRead]
	}
	return c.input.Read(p)
}
func (c *httpMemoryConn) Write(p []byte) (int, error) { return c.output.Write(p) }
func (c *httpMemoryConn) Close() error                { c.closed = true; return nil }
func (c *httpMemoryConn) LocalAddr() net.Addr         { return nil }
func (c *httpMemoryConn) RemoteAddr() net.Addr        { return nil }
func (c *httpMemoryConn) SetDeadline(t time.Time) error {
	c.deadline = t
	return nil
}
func (c *httpMemoryConn) SetReadDeadline(time.Time) error  { return nil }
func (c *httpMemoryConn) SetWriteDeadline(time.Time) error { return nil }

func httpMemoryRequest(raw string, routes map[string]Handler, fallback Handler, maxRead int) ([]byte, *httpMemoryConn) {
	var output bytes.Buffer
	c := &httpMemoryConn{output: &output, maxRead: maxRead}
	c.input.Reset(raw)
	handleConn(c, routes, fallback)
	return output.Bytes(), c
}

func parseHTTPMemoryResponse(t *testing.T, raw []byte) (*http.Response, []byte) {
	t.Helper()
	resp, err := http.ReadResponse(bufio.NewReader(bytes.NewReader(raw)), nil)
	if err != nil {
		t.Fatalf("invalid HTTP response: %v; %q", err, raw)
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatalf("incomplete HTTP response: %v", err)
	}
	if resp.ContentLength != int64(len(body)) {
		t.Fatalf("Content-Length=%d body=%d", resp.ContentLength, len(body))
	}
	return resp, body
}

func TestHTTPHandleConnPreservesRoutingHeadersAndBody(t *testing.T) {
	const body = "{\"pin\":\"0000\",\"text\":\"中文\"}"
	request := fmt.Sprintf("POST /api/sim/unlock?return=%%2F HTTP/1.1\r\nHost: localhost\r\nCoOkIe: session=example; token=value\r\nX-Repeated: first\r\nX-Repeated: second\r\nContent-Length: %d\r\n\r\n%s", len(body), body)
	called := false
	routes := map[string]Handler{"POST /api/sim/unlock": func(w *Resp, r *Req) error {
		called = true
		if r.Method != "POST" || r.Path != "/api/sim/unlock" || r.Query != "return=%2F" || string(r.Body) != body {
			t.Errorf("request changed: %+v", r)
		}
		if r.Header["cookie"] != "session=example; token=value" || r.Header["x-repeated"] != "second" {
			t.Errorf("headers changed: %#v", r.Header)
		}
		w.AddCookie("session=first; HttpOnly; SameSite=Strict")
		w.AddCookie("remember=second; Max-Age=31536000")
		w.AddHeader("X-App-Version", "one")
		w.AddHeader("X-App-Version", "two")
		return w.Write(202, "application/json; charset=utf-8", []byte("{\"ok\":true}"))
	}}
	before := time.Now()
	raw, conn := httpMemoryRequest(request, routes, nil, 1)
	resp, got := parseHTTPMemoryResponse(t, raw)
	if !called || !conn.closed || conn.deadline.Before(before.Add(30*time.Second)) || conn.deadline.After(time.Now().Add(30*time.Second)) {
		t.Fatalf("connection dispatch/close/deadline changed: called=%v conn=%+v", called, conn)
	}
	if resp.Status != "202 Accepted" || string(got) != "{\"ok\":true}" || resp.Header.Get("X-App-Version") != "two" || len(resp.Header.Values("Set-Cookie")) != 2 {
		t.Fatalf("response changed: %s %#v %q", resp.Status, resp.Header, got)
	}
	if resp.Header.Get("Cache-Control") != "no-store" || !resp.Close {
		t.Fatalf("cache/connection policy changed: %#v", resp.Header)
	}
}

func TestHTTPHandleConnLineAndHeaderLimits(t *testing.T) {
	requestLine := func(n int) string {
		return "GET /" + strings.Repeat("p", n-len("GET / HTTP/1.1\r\n")) + " HTTP/1.1\r\n"
	}
	headerLine := func(n int) string { return "X: " + strings.Repeat("h", n-len("X: \r\n")) + "\r\n" }
	for _, tt := range []struct {
		name string
		raw  string
		want bool
	}{
		{"request4096", requestLine(4096) + "\r\n", true},
		{"request4097", requestLine(4097) + "\r\n", false},
		{"header4096", "GET / HTTP/1.1\r\n" + headerLine(4096) + "\r\n", true},
		{"header4097", "GET / HTTP/1.1\r\n" + headerLine(4097) + "\r\n", false},
		{"headers8192", "GET / HTTP/1.1\r\n" + headerLine(4095) + headerLine(4095) + "\r\n", true},
		{"headers8193", "GET / HTTP/1.1\r\n" + headerLine(4096) + headerLine(4095) + "\r\n", false},
		{"incompleteRequest", "GET / HTTP/1.1", false},
		{"incompleteHeaders", "GET / HTTP/1.1\r\nX: value", false},
	} {
		t.Run(tt.name, func(t *testing.T) {
			called := false
			raw, _ := httpMemoryRequest(tt.raw, nil, func(w *Resp, r *Req) error {
				called = true
				return w.Write(200, "text/plain", []byte("ok"))
			}, 31)
			if called != tt.want || (len(raw) > 0) != tt.want {
				t.Fatalf("dispatch=%v response=%q want=%v", called, raw, tt.want)
			}
		})
	}
}

func TestHTTPHandleConnBodyLimitsAndTransferEncoding(t *testing.T) {
	for _, tt := range []struct {
		name, path, headers string
		size                int
		wantStatus          int
		wantDispatch        bool
	}{
		{"normal32768", "/api/send", "Content-Length: 32768\r\n", 32768, 200, true},
		{"normal32769", "/api/send", "Content-Length: 32769\r\n", 0, 413, false},
		{"restore1048576", "/api/restore", "Content-Length: 1048576\r\n", 1048576, 200, true},
		{"restore1048577", "/api/restore", "Content-Length: 1048577\r\n", 0, 413, false},
		{"negative", "/api/send", "Content-Length: -1\r\n", 0, 400, false},
		{"invalid", "/api/send", "Content-Length: bad\r\n", 0, 400, false},
		{"overflow", "/api/send", "Content-Length: 999999999999999999999999\r\n", 0, 400, false},
		{"transferEncoding", "/api/send", "Transfer-Encoding: chunked\r\nContent-Length: 1\r\n", 1, 400, false},
		{"incompleteBody", "/api/send", "Content-Length: 10\r\n", 9, 0, false},
	} {
		t.Run(tt.name, func(t *testing.T) {
			body := strings.Repeat("x", tt.size)
			request := "POST " + tt.path + " HTTP/1.1\r\n" + tt.headers + "\r\n" + body
			called := false
			raw, _ := httpMemoryRequest(request, map[string]Handler{"POST " + tt.path: func(w *Resp, r *Req) error {
				called = true
				if string(r.Body) != body {
					t.Errorf("body changed: got %d bytes want %d", len(r.Body), len(body))
				}
				return w.Write(200, "text/plain", nil)
			}}, nil, 173)
			if called != tt.wantDispatch {
				t.Fatalf("dispatch=%v want=%v", called, tt.wantDispatch)
			}
			if tt.wantStatus == 0 {
				if len(raw) != 0 {
					t.Fatalf("incomplete body produced response: %q", raw)
				}
				return
			}
			resp, _ := parseHTTPMemoryResponse(t, raw)
			if resp.StatusCode != tt.wantStatus {
				t.Fatalf("status=%s want=%d", resp.Status, tt.wantStatus)
			}
		})
	}
}

func TestHTTPHandleConnPreservesLargeResponseAndFallback(t *testing.T) {
	body := bytes.Repeat([]byte("\x00中文\xff0123456789"), 65536)
	for _, method := range []string{"GET", "POST", "HEAD"} {
		t.Run(method, func(t *testing.T) {
			called := false
			raw, _ := httpMemoryRequest(method+" /app.js HTTP/1.1\r\n\r\n", nil, func(w *Resp, r *Req) error {
				called = true
				return w.Write(200, "application/octet-stream", body)
			}, 0)
			resp, got := parseHTTPMemoryResponse(t, raw)
			if method == "GET" {
				if !called || resp.StatusCode != 200 || !bytes.Equal(got, body) {
					t.Fatalf("large body changed: called=%v status=%s length=%d", called, resp.Status, len(got))
				}
			} else if called || resp.StatusCode != 404 || string(got) != "not found" {
				t.Fatalf("fallback method policy changed: called=%v status=%s body=%q", called, resp.Status, got)
			}
		})
	}
}

type httpFailWriter struct{ err error }

func (w httpFailWriter) Write([]byte) (int, error) { return 0, w.err }

func TestHTTPResponsePreservesStatusAndWriteErrors(t *testing.T) {
	for _, status := range []int{200, 202, 400, 401, 404, 409, 413, 429, 500, 503} {
		var output bytes.Buffer
		w := &Resp{bw: bufio.NewWriterSize(&output, 512)}
		if err := w.Write(status, "text/plain", []byte("result")); err != nil {
			t.Fatal(err)
		}
		resp, body := parseHTTPMemoryResponse(t, output.Bytes())
		reason := http.StatusText(status)
		if status == 413 {
			reason = "Payload Too Large"
		}
		if status == 503 {
			reason = "OK" // Preserve the existing default for unlisted statuses.
		}
		if resp.Status != fmt.Sprintf("%d %s", status, reason) || string(body) != "result" {
			t.Fatalf("status/body changed: %s %q", resp.Status, body)
		}
	}
	want := errors.New("connection write failed")
	for _, n := range []int{0, 1048576} {
		w := &Resp{bw: bufio.NewWriterSize(httpFailWriter{want}, 512)}
		if err := w.Write(200, "text/plain", bytes.Repeat([]byte("a"), n)); !errors.Is(err, want) {
			t.Fatalf("write error=%v want=%v", err, want)
		}
	}
}

func TestHTTPResponsePreservesLargeHeadersAndCookies(t *testing.T) {
	contentType := "application/" + strings.Repeat("x", 2048)
	headerValue := strings.Repeat("h", 16384)
	cookie := "session=" + strings.Repeat("c", 16384) + "; HttpOnly"
	body := bytes.Repeat([]byte("large-response\x00"), 65536)
	raw, _ := httpMemoryRequest("GET / HTTP/1.1\r\n\r\n", nil, func(w *Resp, r *Req) error {
		w.AddHeader("X-Large", headerValue)
		w.AddCookie(cookie)
		return w.Write(200, contentType, body)
	}, 0)
	resp, got := parseHTTPMemoryResponse(t, raw)
	if resp.Header.Get("Content-Type") != contentType || resp.Header.Get("X-Large") != headerValue || resp.Header.Get("Set-Cookie") != cookie || !bytes.Equal(got, body) {
		t.Fatal("large response/header/cookie was truncated or changed")
	}
}

type httpCountWriter struct{ writes, bytes int }

func (w *httpCountWriter) Write(p []byte) (int, error) {
	w.writes++
	w.bytes += len(p)
	return len(p), nil
}

func TestHTTPHandleConnLargeResponseUsesBoundedWrites(t *testing.T) {
	for _, n := range []int{1024, 12035, 26787, 1048576} {
		body := bytes.Repeat([]byte("x"), n)
		var output httpCountWriter
		conn := &httpMemoryConn{output: &output}
		conn.input.Reset("GET / HTTP/1.1\r\n\r\n")
		handleConn(conn, nil, func(w *Resp, r *Req) error { return w.Write(200, "application/octet-stream", body) })
		if output.writes > 2 || output.bytes <= len(body) {
			t.Fatalf("large response fragmented: body=%d writes=%d bytes=%d", n, output.writes, output.bytes)
		}
	}
}

func TestHTTPStaticAssetsAreReadOnlyAndComplete(t *testing.T) {
	for _, tt := range []struct {
		path string
		body []byte
	}{
		{"web/index.html", []byte(indexHTML)},
		{"web/style.css", []byte(styleCSS)},
		{"web/model.js", []byte(modelJS)},
		{"web/app.js", []byte(appJS)},
	} {
		t.Run(tt.path, func(t *testing.T) {
			want := append([]byte(nil), tt.body...)
			if len(want) == 0 {
				t.Fatal("embedded asset is empty")
			}
			for i := 0; i < 2; i++ {
				raw, _ := httpMemoryRequest("GET / HTTP/1.1\r\n\r\n", nil, func(w *Resp, r *Req) error {
					return w.Write(200, "application/octet-stream", tt.body)
				}, 0)
				_, got := parseHTTPMemoryResponse(t, raw)
				if !bytes.Equal(want, got) || !bytes.Equal(want, tt.body) {
					t.Fatal("embedded asset changed while serving its bytes")
				}
			}
		})
	}
}

func httpMemoryBenchmarks() []struct {
	name, request string
	handler       Handler
	bodyBytes     int
} {
	jsonBody := []byte(`{"ok":true,"state":"ready","messages":[]}`)
	largeBody := bytes.Repeat([]byte("x"), 1048576)
	return []struct {
		name, request string
		handler       Handler
		bodyBytes     int
	}{
		{"Status", "GET / HTTP/1.1\r\nHost: localhost\r\nCookie: token=example\r\n\r\n", func(w *Resp, r *Req) error { return w.Write(200, "application/json", jsonBody) }, len(jsonBody)},
		{"POST", "POST / HTTP/1.1\r\nContent-Length: 2\r\nContent-Type: application/json\r\n\r\n{}", func(w *Resp, r *Req) error { return w.Write(200, "application/json", jsonBody) }, len(jsonBody)},
		{"UIIndex", "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n", func(w *Resp, r *Req) error { return w.Write(200, "text/html; charset=utf-8", []byte(indexHTML)) }, len(indexHTML)},
		{"UIApp", "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n", func(w *Resp, r *Req) error {
			return w.Write(200, "application/javascript; charset=utf-8", []byte(appJS))
		}, len(appJS)},
		{"LargeResponse", "GET / HTTP/1.1\r\nHost: localhost\r\n\r\n", func(w *Resp, r *Req) error { return w.Write(200, "application/octet-stream", largeBody) }, len(largeBody)},
	}
}

func BenchmarkHTTPHandleConn(b *testing.B) {
	for _, tt := range httpMemoryBenchmarks() {
		b.Run(tt.name, func(b *testing.B) {
			c := &httpMemoryConn{output: io.Discard}
			routes := map[string]Handler{"GET /": tt.handler, "POST /": tt.handler}
			b.SetBytes(int64(tt.bodyBytes))
			b.ReportAllocs()
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				c.input.Reset(tt.request)
				handleConn(c, routes, nil)
			}
		})
	}
}

func TestHTTPHandleConnAllocationBudget(t *testing.T) {
	for _, tt := range httpMemoryBenchmarks() {
		if tt.name != "Status" && tt.name != "UIApp" {
			continue
		}
		t.Run(tt.name, func(t *testing.T) {
			result := testing.Benchmark(func(b *testing.B) {
				c := &httpMemoryConn{output: io.Discard}
				routes := map[string]Handler{"GET /": tt.handler}
				b.ResetTimer()
				for i := 0; i < b.N; i++ {
					c.input.Reset(tt.request)
					handleConn(c, routes, nil)
				}
			})
			if n := result.AllocedBytesPerOp(); n > 4096 {
				t.Fatalf("HTTP request allocates %d bytes; budget is 4096 bytes including parsing and response buffers", n)
			}
		})
	}
}
