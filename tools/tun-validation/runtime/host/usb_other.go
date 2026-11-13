//go:build !darwin || !cgo

package main

import "errors"

func openUSB(vid, pid, location uint64) (transport, error) {
	return nil, errors.New("live USB transport requires macOS with cgo; protocol tests use a simulated transport")
}
