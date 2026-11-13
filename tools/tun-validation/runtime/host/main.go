package main

import (
	"bufio"
	"encoding/base64"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"time"
)

func main() {
	vid := flag.String("vid", "", "exact VID")
	pid := flag.String("pid", "", "exact PID")
	location := flag.String("location", "", "exact USB location")
	dir := flag.String("logdir", "", "new host evidence directory")
	run := flag.String("run", "", "unique run id")
	trace := flag.Bool("usb-trace", false, "persist USB transfer metadata without payloads")
	flag.Parse()
	if *dir == "" || !safeID.MatchString(*run) {
		fmt.Fprintln(os.Stderr, "exact USB identity, unique run id and new logdir required")
		os.Exit(2)
	}
	v, e := strconv.ParseUint(*vid, 0, 16)
	if e != nil {
		panic(e)
	}
	p, e := strconv.ParseUint(*pid, 0, 16)
	if e != nil {
		panic(e)
	}
	l, e := strconv.ParseUint(*location, 0, 32)
	if e != nil {
		panic(e)
	}
	if e = os.Mkdir(*dir, 0700); e != nil {
		panic(e)
	}
	events, e := os.OpenFile(filepath.Join(*dir, "events.jsonl"), os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
	if e != nil {
		panic(e)
	}
	defer events.Close()
	zone, e := time.LoadLocation("Asia/Shanghai")
	if e != nil {
		panic(e)
	}
	emit := func(kind string, data any) {
		event := map[string]any{"time": time.Now().In(zone).Format(time.RFC3339Nano), "run": *run, "kind": kind, "data": data}
		b, e := json.Marshal(event)
		if e != nil {
			panic(e)
		}
		if _, e = events.Write(append(b, '\n')); e != nil {
			panic(e)
		}
		if e = events.Sync(); e != nil {
			panic(e)
		}
		fmt.Println(string(b))
	}
	tr, e := openUSB(v, p, l)
	if e != nil {
		emit("open_failed", e.Error())
		os.Exit(1)
	}
	defer tr.Close()
	if *trace {
		f, err := os.OpenFile(filepath.Join(*dir, "usb-transfers.jsonl"), os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0600)
		if err != nil {
			panic(err)
		}
		defer f.Close()
		tr = &tracedTransport{transport: tr, save: func(e transferEvent) error {
			b, err := json.Marshal(e)
			if err != nil {
				return err
			}
			if _, err = f.Write(append(b, '\n')); err != nil {
				return err
			}
			return f.Sync()
		}}
	}
	emit("ready", map[string]any{"vid": v, "pid": p, "location": l, "automatic_recovery": false, "at_commands": false})
	c := newClient(tr)
	c.orphan = func(p packet) error {
		emit("orphan_packet", map[string]any{"command": fmt.Sprintf("%x", p.cmd), "arg0": p.a, "arg1": p.b, "payload_base64": base64.StdEncoding.EncodeToString(p.data)})
		return nil
	}
	input := bufio.NewScanner(os.Stdin)
	input.Buffer(make([]byte, 4096), 16384)
	for input.Scan() {
		var q request
		if e = json.Unmarshal(input.Bytes(), &q); e != nil {
			emit("request_rejected", e.Error())
			continue
		}
		if q.Op == "quit" {
			emit("closed", nil)
			return
		}
		emit("begin", map[string]any{"id": q.ID, "op": q.Op})
		r := runRequest(c, q, *run, *dir)
		emit("result", r)
	}
	if e = input.Err(); e != nil {
		emit("stdin_error", e.Error())
	}
}
