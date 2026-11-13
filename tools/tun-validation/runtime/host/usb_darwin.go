//go:build darwin && cgo

package main

/*
#cgo CFLAGS: -Wno-deprecated-declarations
#cgo LDFLAGS: -framework IOKit -framework CoreFoundation
#include "usb_darwin.h"
*/
import "C"
import (
	"errors"
	"time"
	"unsafe"
)

type usbTransport struct{ h *C.qdc_usb }

func openUSB(vid, pid, location uint64) (transport, error) {
	h := C.qdc_open(C.uint16_t(vid), C.uint16_t(pid), C.uint32_t(location))
	if h == nil {
		return nil, errors.New(C.GoString(C.qdc_error()))
	}
	return &usbTransport{h}, nil
}
func (u *usbTransport) Close() error {
	if u.h != nil {
		C.qdc_close(u.h)
		u.h = nil
	}
	return nil
}
func (u *usbTransport) Write(p []byte) error {
	if len(p) == 0 {
		return nil
	}
	if C.qdc_write(u.h, unsafe.Pointer(&p[0]), C.uint32_t(len(p))) != 0 {
		return errors.New(C.GoString(C.qdc_error()))
	}
	return nil
}
func (u *usbTransport) Read(p []byte, d time.Duration) (int, error) {
	if len(p) == 0 {
		return 0, nil
	}
	ms := d.Milliseconds()
	if ms < 1 {
		ms = 1
	}
	if ms > 500 {
		ms = 500
	}
	var n C.uint32_t
	if C.qdc_read(u.h, unsafe.Pointer(&p[0]), C.uint32_t(len(p)), C.uint32_t(ms), &n) != 0 {
		return 0, errors.New(C.GoString(C.qdc_error()))
	}
	return int(n), nil
}
