package main

// httpmux.go — 极简 HTTP/1.1 服务器
//
// 为什么不用标准库 net/http：这块模块是内核 3.18 的 32 位 ARM，实测 net/http 一初始化就
// 段错误（net 包本身正常）。手写一个够用的 HTTP 服务器体积更小、内存占用更低，也更可控。

import (
	"bufio"
	"fmt"
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
}

// AddCookie 追加一个 Set-Cookie 头
func (w *Resp) AddCookie(c string) { w.cookies = append(w.cookies, c) }

func (w *Resp) Write(status int, contentType string, body []byte) error {
	reason := "OK"
	switch status {
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
	if _, err := fmt.Fprintf(w.bw, "HTTP/1.1 %d %s\r\n", status, reason); err != nil {
		return err
	}
	if _, err := fmt.Fprintf(w.bw,
		"Content-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\nCache-Control: no-store\r\n",
		contentType, len(body)); err != nil {
		return err
	}
	for _, c := range w.cookies {
		if _, err := fmt.Fprintf(w.bw, "Set-Cookie: %s\r\n", c); err != nil {
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
	for {
		c, err := ln.Accept()
		if err != nil {
			continue
		}
		go handleConn(c, routes, fallback)
	}
}

func handleConn(c net.Conn, routes map[string]Handler, fallback Handler) {
	defer c.Close()
	_ = c.SetDeadline(time.Now().Add(30 * time.Second))
	br := bufio.NewReaderSize(c, 4096)
	bw := bufio.NewWriterSize(c, 16384)

	line, err := br.ReadString('\n')
	if err != nil {
		return
	}
	line = strings.TrimRight(line, "\r\n")
	parts := strings.SplitN(line, " ", 3)
	if len(parts) < 2 {
		return
	}
	req := &Req{Method: parts[0], Path: parts[1], Header: map[string]string{}}
	if i := strings.IndexByte(req.Path, '?'); i >= 0 {
		req.Query = req.Path[i+1:]
		req.Path = req.Path[:i]
	}
	for {
		l, err := br.ReadString('\n')
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
	if cl := req.Header["content-length"]; cl != "" {
		if n, err := strconv.Atoi(cl); err == nil && n > 0 && n <= 65536 {
			req.Body = make([]byte, n)
			if _, err := io.ReadFull(br, req.Body); err != nil {
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
