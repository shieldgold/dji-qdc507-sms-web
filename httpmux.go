package main

// httpmux.go — 极简 HTTP/1.1 服务器
//
// 为什么不用标准库 net/http：这块模块是内核 3.18 的 32 位 ARM，实测 net/http 一初始化就
// 段错误（net 包本身正常）。手写一个够用的 HTTP 服务器体积更小、内存占用更低，也更可控。

import (
	"bufio"
	"io"
	"net"
	"strconv"
	"strings"
	"time"
)

type Req struct {
	Method string
	Path   string
	Query  string
	Header map[string]string
	Body   []byte
}

type Resp struct {
	bw      *bufio.Writer
	cookies []string
	headers map[string]string
}

func (w *Resp) AddHeader(k, v string) {
	if w.headers == nil {
		w.headers = map[string]string{}
	}
	w.headers[k] = v
}

// AddCookie 追加一个 Set-Cookie 头
func (w *Resp) AddCookie(c string) { w.cookies = append(w.cookies, c) }

func (w *Resp) Write(status int, contentType string, body []byte) error {
	reason := "OK"
	switch status {
	case 202:
		reason = "Accepted"
	case 409:
		reason = "Conflict"
	case 413:
		reason = "Payload Too Large"
	case 400:
		reason = "Bad Request"
	case 401:
		reason = "Unauthorized"
	case 404:
		reason = "Not Found"
	case 429:
		reason = "Too Many Requests"
	case 500:
		reason = "Internal Server Error"
	}
	// Build the common header in the writer's own buffer. Large bodies still go
	// through bufio.Writer.Write, which writes them directly after flushing.
	header := w.bw.AvailableBuffer()
	header = append(header, "HTTP/1.1 "...)
	header = strconv.AppendInt(header, int64(status), 10)
	header = append(header, ' ')
	header = append(header, reason...)
	header = append(header, "\r\nContent-Type: "...)
	header = append(header, contentType...)
	header = append(header, "\r\nContent-Length: "...)
	header = strconv.AppendInt(header, int64(len(body)), 10)
	header = append(header, "\r\nConnection: close\r\nCache-Control: no-store\r\n"...)
	if _, err := w.bw.Write(header); err != nil {
		return err
	}
	for k, v := range w.headers {
		if _, err := w.bw.WriteString(k); err != nil {
			return err
		}
		if _, err := w.bw.WriteString(": "); err != nil {
			return err
		}
		if _, err := w.bw.WriteString(v); err != nil {
			return err
		}
		if _, err := w.bw.WriteString("\r\n"); err != nil {
			return err
		}
	}
	for _, c := range w.cookies {
		if _, err := w.bw.WriteString("Set-Cookie: "); err != nil {
			return err
		}
		if _, err := w.bw.WriteString(c); err != nil {
			return err
		}
		if _, err := w.bw.WriteString("\r\n"); err != nil {
			return err
		}
	}
	if _, err := w.bw.WriteString("\r\n"); err != nil {
		return err
	}
	if _, err := w.bw.Write(body); err != nil {
		return err
	}
	return w.bw.Flush()
}

type Handler func(*Resp, *Req) error

// ServeHTTP 极简服务器：每连接一个 goroutine，只支持 GET/POST 与 Content-Length 请求体
func ServeHTTP(addr string, routes map[string]Handler, fallback Handler) error {
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return err
	}
	slots := make(chan struct{}, 8)
	for {
		c, err := ln.Accept()
		if err != nil {
			continue
		}
		select {
		case slots <- struct{}{}:
			go func() { defer func() { <-slots }(); handleConn(c, routes, fallback) }()
		default:
			c.Close()
		}
	}
}

func handleConn(c net.Conn, routes map[string]Handler, fallback Handler) {
	defer c.Close()
	_ = c.SetDeadline(time.Now().Add(30 * time.Second))
	br := bufio.NewReaderSize(c, 512)
	bw := bufio.NewWriterSize(c, 512)

	line, err := readHTTPLine(br)
	if err != nil {
		return
	}
	line = strings.TrimRight(line, "\r\n")
	i := strings.IndexByte(line, ' ')
	if i < 0 {
		return
	}
	method, path := line[:i], line[i+1:]
	if i := strings.IndexByte(path, ' '); i >= 0 {
		path = path[:i]
	}
	req := &Req{Method: method, Path: path, Header: map[string]string{}}
	if i := strings.IndexByte(req.Path, '?'); i >= 0 {
		req.Query = req.Path[i+1:]
		req.Path = req.Path[:i]
	}
	headerBytes := 0
	for {
		l, err := readHTTPLine(br)
		headerBytes += len(l)
		if headerBytes > 8192 {
			return
		}
		if err != nil {
			return
		}
		l = strings.TrimRight(l, "\r\n")
		if l == "" {
			break
		}
		if i := strings.IndexByte(l, ':'); i > 0 {
			req.Header[strings.ToLower(strings.TrimSpace(l[:i]))] = strings.TrimSpace(l[i+1:])
		}
	}
	if req.Header["transfer-encoding"] != "" {
		_ = (&Resp{bw: bw}).Write(400, "text/plain", []byte("unsupported transfer encoding"))
		return
	}
	if cl := req.Header["content-length"]; cl != "" {
		n, e := strconv.Atoi(cl)
		if e != nil || n < 0 {
			_ = (&Resp{bw: bw}).Write(400, "text/plain", []byte("invalid content length"))
			return
		}
		limit := 32768
		if req.Path == "/api/restore" {
			limit = 1048576
		}
		if n > limit {
			_ = (&Resp{bw: bw}).Write(413, "text/plain", []byte("request too large"))
			return
		}
		if n > 0 {
			req.Body = make([]byte, n)
			if _, e := io.ReadFull(br, req.Body); e != nil {
				return
			}
		}
	}

	resp := &Resp{bw: bw}
	h := routes[req.Method+" "+req.Path]
	if h == nil && req.Method == "GET" {
		h = fallback
	}
	if h == nil {
		_ = resp.Write(404, "text/plain; charset=utf-8", []byte("not found"))
		return
	}
	if err := h(resp, req); err != nil {
		_ = resp.Write(500, "text/plain; charset=utf-8", []byte(err.Error()))
	}
}

// Keep the original 4 KiB per-line limit while paying for that capacity only
// when a request/header line does not fit the small connection read buffer.
func readHTTPLine(br *bufio.Reader) (string, error) {
	raw, err := br.ReadSlice('\n')
	if err != bufio.ErrBufferFull {
		return string(raw), err
	}
	var line strings.Builder
	line.Grow(4096)
	for {
		if line.Len()+len(raw) > 4096 {
			return "", bufio.ErrBufferFull
		}
		_, _ = line.Write(raw)
		if err != bufio.ErrBufferFull {
			return line.String(), err
		}
		if line.Len() == 4096 {
			return "", bufio.ErrBufferFull
		}
		raw, err = br.ReadSlice('\n')
	}
}
