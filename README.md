# dji-qdc507-sms-web

> This branch adds standalone SMS management and an experimental native iOS calling app. See [deployment and rollback](IPHONE_DIRECT.md) and [iOS calling status and setup](IOS_CALLING.md). The user has confirmed stable outgoing iPhone calls with two-way audio, and significantly improved latency that no longer accumulates during continuous speech after the module fix. Version 0.3.0 adds SMS and incoming-call push notifications sent directly from the module to Apple; see [configuration and acceptance status](BACKGROUND_NOTIFICATIONS.md).

**Read and send SMS — including verification codes — on an iPad, iPhone, or any browser-only device, using a cheap DJI Cellular 4G module.**

[English](README.md) · [简体中文](README.zh-CN.md)

---

## The problem: an iPad **cannot** receive SMS from a USB 4G dongle

You bought a ~$5 DJI Cellular 4G module (Baiwang **QDC507**) so your iPad could get online anywhere. You plug it into the iPad's USB-C port and — it works. The module speaks **CDC-ECM**, so iPadOS picks it up as a wired network adapter with no driver at all. Internet: solved.

But **you cannot receive SMS verification codes on that iPad.** And that is not a configuration mistake — it is a hard wall in iPadOS/iOS:

| What you would need | Why it is impossible on iPadOS / iOS |
|---|---|
| An app that talks to the module's USB AT command port | iPadOS has **no DriverKit** (that is macOS-only). A third-party app cannot open a generic USB serial device. Apple's own developer forums confirm it: *"Can an iOS app access a generic FTDI USB-serial device? (Works on Android, not on iPhone)"* — no. |
| The MFi accessory framework | `ExternalAccessory` only talks to **MFi-certified** hardware. This module is not MFi. |
| A web page that talks to the USB port | Every browser on iOS/iPadOS is WebKit underneath, and **WebKit supports neither WebUSB nor WebSerial**. |
| The module's built-in web UI | The module ships bare modem firmware. A full port scan of its gateway shows **exactly one open port: DNS (53)**. There is no web interface to visit. |

So on an iPad the module is a network card and nothing more. Reading SMS requires AT commands (`AT+CMGL`, `AT+CMGR`) over a serial channel — and iPadOS gives you no way to reach one. On macOS you can (DriverKit / libusb); on iPadOS you cannot. **Verification codes keep going to a phone you left at home.**

## The solution: put the web server *inside* the module

The QDC507 is not a dumb dongle. It is a **complete Linux computer** — Qualcomm MDM9607, ARMv7, kernel 3.18.44, 43 MB RAM — running Quectel firmware, and you can get a **root shell over ADB without flashing anything**.

So instead of fighting the iPad, we move the problem:

```
              BEFORE                                        AFTER
  ┌────────────────────┐                        ┌──────────────────────────┐
  │       iPad         │                        │          iPad            │
  │  no way to reach   │                        │  Safari →                │
  │  the SMS at all    │                        │  http://192.168.225.1:8787
  └─────────┬──────────┘                        └────────────┬─────────────┘
            │ USB-C                                          │ USB-C (ECM network)
  ┌─────────┴──────────┐                        ┌────────────┴─────────────┐
  │      QDC507        │                        │        QDC507            │
  │  modem only        │                        │  ├─ modem                │
  │  no web UI, no API │                        │  ├─ AT channel (/dev/smd9)
  └────────────────────┘                        │  └─ smsweb  ← HTTP :8787 │
                                                └──────────────────────────┘
```

**`smsweb` is a 2.6 MB static ARMv7 binary that runs inside the module itself.** It
talks to the modem's AT channel on `/dev/smd9`, serves a small web UI on the module's
own USB network interface, stores messages on the module's writable `/data` partition,
and auto-starts on every boot.

Now **any device with a browser** — iPad, iPhone, Android, Windows, Linux — can read
and send SMS over the module's SIM card. No app, no cloud, no computer, no internet.

---

## Features

| | |
|---|---|
| **SMS inbox** | Live list, newest first. New messages appear within ~1s (driven by the modem's `+CMTI` URC), 3s polling as a safety net. |
| **Verification codes** | Auto-detected (keyword-aware, 4–8 digits), shown large, **one-tap copy**. The reason this project exists. |
| **Send SMS** | Chinese and English, long messages automatically split into concatenated parts (UDH). |
| **Delete** | Removes the message from the module's own storage *and* from the local archive. |
| **Per-SIM password** | First visit asks you to set a password. The account is keyed to the SIM's **ICCID**, so every card has its own password — swap SIMs and you register again. |
| **"Remember me"** | Optional checkbox. Remembered sessions last a year and survive module reboots; otherwise it is a browser-session cookie that also expires server-side in 12 h. |
| **Change password / logout** | Changing the password invalidates every other logged-in device. |
| **Status bar** | Carrier, signal strength, SIM state, storage usage, last poll time. |
| **Auto-start** | A single symlink in the read-only rootfs; the service comes up by itself ~40 s after the module powers on. |
| **Tiny** | 2.6 MB binary, **~3 MB RSS** on a 43 MB device. |

---

## Requirements

- **A DJI Cellular 4G module, first generation** — Baiwang **QDC507** (sold as a DJI drone accessory for ~$5). A Quectel EG25-G with the same USB layout should also work.
- A SIM card with **data + SMS** (voice is not needed for SMS; calling requires a voice-enabled SIM and the experimental runtime).
- Any host to deploy from (macOS/Linux/Windows) with **`adb`** and a **USB data cable**.
- To build: **Go 1.21** (see [Why Go 1.21](#why-go-121--an-important-build-constraint)).

---

## Quick start

### 1. Enable ADB on the module

The module already exposes an ADB interface (`ff/42/01`) when its USB composition has the
ADB bit set. If yours does not, send these AT commands from a host that can reach the
module's AT port (on Linux: `/dev/ttyUSB2`; on macOS: over libusb), then replug:

```
AT+QCFG="usbcfg",0x2C7C,0x0125,1,1,1,1,1,1,0     # the 8th field turns ADB on
AT+CFUN=1,1                                       # restart the module
```

> Keep your own VID/PID in that command. On a factory-fresh DJI module it is
> `0x2CA3,0x4006`; write down the current value with `AT+QCFG="usbcfg"` **before** changing anything.

Then:

```bash
adb devices          # should list a device (serial may be empty)
adb shell id         # should print uid=0(root)
```

### 2. Build

```bash
./build.sh           # produces smsweb-armv7 (2.6 MB, static, Linux/ARMv7)
```

### 3. Deploy

```bash
./deploy/install.sh            # push + start (does NOT touch the rootfs)
./deploy/install.sh --autostart # also install the boot hook (writes ONE symlink to the rootfs)
```

The script prints the URL to open when it is done.

### 4. Use it

Plug the module into your iPad/iPhone (with a USB-C adapter)/laptop and open:

```
http://192.168.225.1:8787
```

- **First time:** the page shows your number and ICCID and asks you to set a password
  (minimum 4 characters) with a **"remember me on this device"** checkbox.
- **After that:** enter the password, or just walk straight in if you ticked remember.

> Tip: on an iPad, use **Share → Add to Home Screen** so it opens like an app.
> The module needs ~40 s after power-up before the page responds.

---

## Why PDU mode (and not the "simple" text mode)

Text mode (`AT+CMGF=1`) looks easier, but this module returns Chinese bodies as
**hex-encoded UCS2** — and a plain numeric verification code such as `123456` is *also*
a valid even-length hex string. There is no reliable way to tell them apart, which is
exactly the case that matters most.

This project uses **PDU mode** (`AT+CMGF=0`) instead: the encoding is stated explicitly in
the DCS field of GSM 03.40, so there is no ambiguity. It implements:

- GSM 7-bit packing/unpacking (including the extension table),
- UCS2 (UTF-16BE) encode/decode,
- UDH concatenation for long messages (both directions),
- alphanumeric and numeric originator addresses,
- SCTS timestamp decoding with timezone.

All of it is covered by unit tests, including a synthetic two-part SMS-DELIVER PDU vector
and the **official PBKDF2-HMAC-SHA256 test vectors**.

---

## Security

| | |
|---|---|
| Password storage | **PBKDF2-HMAC-SHA256, 20 000 iterations, 16-byte random salt.** Only the hash is stored — never the password. |
| Randomness | `/dev/urandom` |
| Sessions | 256-bit random token in an **HttpOnly** cookie (`SameSite=Lax`), so page scripts cannot read it. |
| Remember me | Token valid for 1 year **and persisted** to `/data/smsweb/auth.json`, so it survives reboots. |
| No remember me | Session cookie + 12 h server-side expiry. |
| Brute force | 5 failed attempts → 60-second lockout. Password comparison uses **constant-time** comparison. |
| Unauthenticated | `/api/state`, `/api/send`, `/api/delete` all return **401**; the page never receives SMS content before login. |
| Exposure | The UI is only reachable from the device the module is plugged into (it lives on the module's own USB network). It is not reachable from the internet. |

Traffic is plain HTTP over the module's private USB link. See [Known limitations](#known-limitations).

---

## How it works

```
 iPad / any browser
        │  HTTP :8787
 ┌──────┴──────────────────────────────────────────────┐
 │ QDC507 module — Linux 3.18.44, ARMv7, 43 MB RAM     │
 │                                                     │
 │  smsweb (Go 1.21, static)                           │
 │   ├── httpmux.go  hand-rolled HTTP/1.1 server       │
 │   ├── ui.go       embedded single-page UI           │
 │   ├── auth.go     PBKDF2 password + token sessions  │
 │   ├── at.go       AT engine: commands, URCs         │
 │   └── pdu.go      GSM 03.40 PDU codec               │
 │            │                                        │
 │            │  /dev/smd9  (free AT channel)          │
 │            ▼                                        │
 │        baseband / SIM  ──►  cellular network        │
 └─────────────────────────────────────────────────────┘
```

The module's internal AT channels are a nice detail: `/dev/smd7` is held by Quectel's
`ql_manager_server` and `/dev/smd8` by `quectel-remotefs-service`, but **`/dev/smd9` is
completely free** — that is the one we use, so nothing on the module is disturbed and the
host-side AT port keeps working.

### Repository layout

```
.
├── main.go        HTTP routes, message store/merge, code detection, auth endpoints
├── auth.go        PBKDF2 hashing, tokens, lockout, cookies
├── at.go          AT command engine, SIM identity (ICCID/IMSI/MSISDN)
├── pdu.go         GSM 03.40 PDU encode/decode (receive, send, concatenation)
├── httpmux.go     minimal HTTP/1.1 server (Set-Cookie support)
├── ui.go          embedded UI: setup / login / main, three views
├── build.sh       cross-compile for Linux/ARMv7
├── deploy/
│   ├── install.sh push + start (+ optional boot hook)
│   └── start.sh   boot script used by the autostart hook
├── pdu_test.go    PDU unit tests
└── auth_test.go   password/auth unit tests
```

---

## Why Go 1.21 — an important build constraint

**Go 1.24 and newer cannot be used for this target.** Their `crypto/*` packages pull in the
FIPS 140-3 module, whose initialisation **segfaults immediately on this kernel (3.18.44)**
— `crypto/sha256`, `crypto/rand` and `crypto/pbkdf2` all crash the process at startup, and
`GOFIPS140=off` does not help. This was isolated by bisecting imports with tiny probe
programs on the device itself.

The same class of problem bites the standard library's `net/http`, which **also segfaults
on init** here (while `net` alone is fine). That is why this project hand-rolls:

- an HTTP/1.1 server on top of `net` (`httpmux.go`),
- PBKDF2-HMAC-SHA256 on top of `crypto/hmac` + `crypto/sha256` (`auth.go`),
- randomness from `/dev/urandom` instead of `crypto/rand`.

Those three changes took the binary from 6.6 MB to 2.6 MB and the RSS to ~3 MB.

### Build

```bash
export GOTOOLCHAIN=local          # do not let go.mod pull a newer toolchain
export CGO_ENABLED=0 GOOS=linux GOARCH=arm GOARM=7
go build -trimpath -ldflags="-s -w" -o smsweb-armv7 .
```

### Test

```bash
go test ./...                     # any Go >= 1.21 on your host
```

> Note: Go 1.21's *host* binaries do not run on macOS 27+ (dyld rejects them for a missing
> `LC_UUID`), but cross-compiling to Linux/ARMv7 works perfectly. Use a newer Go for tests
> on such a host, and Go 1.21 for the target build.

---

## Uninstall

```bash
# remove the boot hook (the only change ever made outside /data)
adb shell "mount -o remount,rw /; rm -f /etc/rcS.d/S99smsweb; sync; mount -o remount,ro /"

# remove everything else
adb shell "kill \$(pidof smsweb); rm -rf /data/smsweb; sync"
```

No firmware is flashed, nothing in the boot chain is touched, and the module keeps working
exactly as it did before.

---

## Tested on

| | |
|---|---|
| Module | DJI Cellular 4G module, gen 1 — Baiwang **QDC507**, firmware `QDC507GLEFM21`, USB `2C7C:0125` |
| OS | Linux `3.18.44` (Quectel), ARMv7, 43 MB RAM, ~15 MB writable `/data` |
| Clients | iPadOS Safari, macOS Safari, desktop Chrome |
| Carrier | China Unicom (any carrier with SMS should work) |
| Verified | SMS receive (incl. two-part concatenation), SMS send, verification-code extraction, per-SIM password setup/login/wrong-password/lockout/remember-me persistence across reboot, autostart after reboot |

---

## Known limitations

1. **One host at a time.** The module is a point-to-point USB modem; while it is plugged into the iPad, your laptop cannot use it.
2. **~40 s after power-up** before the web UI responds (the module's Linux has to boot).
3. **Plain HTTP** on the module's private USB link. The password is stored with PBKDF2, but traffic is not encrypted in transit — acceptable given only the device the module is plugged into can reach it. HTTPS could be added.
4. **Archive capped at 300 messages**; the oldest are pruned.
5. **Experimental voice.** This branch adds a native iOS app and module-side audio runtime. The user has confirmed stable outgoing calls with two-way audio and no progressively increasing speech delay. Background incoming-call wakeup is not implemented. See [IOS_CALLING.md](IOS_CALLING.md).
6. **Modifying the module is not officially supported.** Enabling ADB changes an NV setting and reboots the module. It is reversible and widely done by the community, but you do it at your own risk.

---

## Credits & related projects

This project was built after studying the community that reverse-engineered this module:

- [cr-zhichen/DJOneHubNative](https://github.com/cr-zhichen/DJOneHubNative) — its `docs/MODULE_RESEARCH.md` documents the module's internals, the ADB route and why the firmware's audio path is crippled.
- [tinydream96/DJI-4G-Dongle-macOS-Config](https://github.com/tinydream96/DJI-4G-Dongle-macOS-Config) — the known-good approach for talking AT over libusb from macOS.
- [mccding/CellBridge](https://github.com/mccding/CellBridge) — a SIP/VoLTE gateway built on the same module.
- [wlzh/dji-4g-vohive-mac](https://github.com/wlzh/dji-4g-vohive-mac) — the VoHive route for SMS/proxy/eSIM on this hardware.

## License

[MIT](LICENSE).

Not affiliated with, endorsed by, or supported by DJI or Quectel. "DJI" and "Quectel" are
trademarks of their respective owners. Use at your own risk.
