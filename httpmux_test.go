package main

import (
	"fmt"
	"io"
	"net"
	"strings"
	"testing"
)

func TestHTTPRejectsOversizedBodyWithoutDispatch(t *testing.T) {
	server, client := net.Pipe()
	defer client.Close()
	called := false
	go handleConn(server, map[string]Handler{"POST /api/restore": func(w *Resp, r *Req) error { called = true; return w.Write(200, "text/plain", nil) }}, nil)
	go fmt.Fprint(client, "POST /api/restore HTTP/1.1\r\nHost: localhost\r\nContent-Length: 1048577\r\n\r\n")
	b, _ := io.ReadAll(client)
	if !strings.Contains(string(b), "413") || called {
		t.Fatalf("oversize dispatched: %v %s", called, b)
	}
}
