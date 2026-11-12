# dji-qdc507-sms-web

> 本分支提供模块独立运行的短信管理和实验性 iOS 通话 App。部署与回退见 [IPHONE_DIRECT.md](IPHONE_DIRECT.md)，通话实现与验收状态见 [IOS_CALLING.md](IOS_CALLING.md)。iPhone 真实外呼稳定且双方有声已获用户确认；持续讲话延迟修复已部署，待复测。

**用一块几十块钱的大疆 4G 模块，让 iPad、iPhone 或任何只能开浏览器的设备收发短信、拿到验证码。**

[English](README.md) · [简体中文](README.zh-CN.md)

---

## 问题：iPad **收不到** USB 4G 模块里的短信

你花几十块买了一块大疆一代 4G 模块（Baiwang **QDC507**），想让 iPad 在外面也能上网。插上 iPad 的 USB-C 口——**能上网**：模块走标准 **CDC-ECM**，iPadOS 免驱识别成有线网卡，开箱即用。

**但这台 iPad 收不到短信验证码。** 而且这不是配置问题，是 iPadOS / iOS 的一道硬墙：

| 你需要的途径 | 为什么在 iPadOS / iOS 上走不通 |
|---|---|
| 装个 App 去读模块的 USB AT 串口 | iPadOS **没有 DriverKit**（那是 macOS 独有的）。第三方 App 无法打开通用 USB 串口设备。Apple 开发者论坛的原话：*"Can an iOS app access a generic FTDI USB-serial device? (Works on Android, not on iPhone)"* —— 不行。 |
| 走 MFi 配件框架 | `ExternalAccessory` 只认 **MFi 认证**配件，这块模块不是。 |
| 写个网页去读 USB 口 | iOS / iPadOS 上所有浏览器内核都是 WebKit，而 **WebKit 既不支持 WebUSB 也不支持 WebSerial**。 |
| 打开模块自带的网页界面 | 模块是纯猫固件。对它网关做全端口扫描，**只开了一个端口：DNS(53)**。根本没有网页可开。 |

所以插在 iPad 上，它就只是一张网卡。要读短信必须走串口发 AT 指令（`AT+CMGL`、`AT+CMGR`）——而 iPadOS 不给你任何通往串口的路。macOS 可以（DriverKit / libusb），iPadOS 不行。**验证码只能继续飞到那台你没带出门的手机上。**

## 解决思路：把网页服务器塞进模块**内部**

QDC507 不是一块傻网卡，它**本质上是一台完整的 Linux 小电脑**——高通 MDM9607、ARMv7、内核 3.18.44、43MB 内存，跑的是移远 Quectel 固件，而且**不刷机就能通过 ADB 拿到 root shell**。

所以别跟 iPad 硬碰，我们把问题挪个位置：

```
              改造前                                        改造后
  ┌────────────────────┐                        ┌──────────────────────────┐
  │       iPad         │                        │          iPad            │
  │  完全够不到短信     │                        │  Safari →                │
  │                    │                        │  http://192.168.225.1:8787
  └─────────┬──────────┘                        └────────────┬─────────────┘
            │ USB-C                                          │ USB-C（ECM 网络）
  ┌─────────┴──────────┐                        ┌────────────┴─────────────┐
  │      QDC507        │                        │        QDC507            │
  │  只是猫，无网页     │                        │  ├─ 基带                 │
  │  无接口可调         │                        │  ├─ AT 通道(/dev/smd9)   │
  └────────────────────┘                        │  └─ smsweb  ← HTTP :8787 │
                                                └──────────────────────────┘
```

**`smsweb` 是一个 2.6MB 的 ARMv7 静态二进制，直接跑在模块内部。** 它：

- 通过 `/dev/smd9` 与基带对话收发短信，
- 在模块自己的 USB 网卡上跑一个极小的 HTTP 服务，
- 把短信归档到模块可写的 `/data` 分区，
- 每次开机自动启动。

于是**任何能开浏览器的设备**——iPad、iPhone、安卓、Windows、Linux——都能用这张 SIM 卡收发短信。不需要 App、不需要云、不需要电脑、不需要外网。

---

## 功能

| | |
|---|---|
| **短信收件箱** | 实时列表，最新在上。新短信约 1 秒内出现（由基带 `+CMTI` 通知驱动），另有 3 秒轮询兜底。 |
| **验证码识别** | 自动识别（结合「验证码/校验码」等关键词，4–8 位数字），大字显示，**一键复制**。这就是本项目存在的理由。 |
| **发短信** | 支持中英文；长短信自动 UDH 分片。 |
| **删除** | 同时从模块存储和本机归档删除。 |
| **按号码设密码** | 首次打开让你设置访问密码；账号主键是 SIM 卡的 **ICCID**，**每张卡一个密码**，换卡就要重新设置。 |
| **「在本机记住登录」** | 可选勾选框。勾选后有效期一年，**模块重启也免输入**；不勾则是会话 Cookie，服务端 12 小时过期。 |
| **改密码 / 退出登录** | 改密码会把其他所有已登录设备踢下线。 |
| **状态栏** | 运营商、信号强度、SIM 状态、存储占用、最近轮询时间。 |
| **开机自启** | 只在只读系统分区加一个符号链接，模块上电约 40 秒后服务自动就绪。 |
| **极小** | 2.6MB 二进制，在 43MB 内存的设备上占用约 **3MB RSS**。 |

---

## 环境要求

- **大疆一代 4G 模块**（Baiwang **QDC507**，闲鱼几十块）。USB 布局相同的 Quectel EG25-G 理论上也行。
- 一张**能上网、能收短信**的 SIM 卡（短信不需要语音；实验性通话功能需要开通语音业务及部署额外运行时）。
- 一台用于部署的电脑（macOS / Linux / Windows）+ **`adb`** + **能传数据的 USB 线**。
- 编译需要 **Go 1.21**（原因见 [为什么必须用 Go 1.21](#为什么必须用-go-121)。

---

## 快速开始

### 1. 打开模块的 ADB

只要模块的 USB 组合里 ADB 位是开的，它就会暴露一个 ADB 接口（`ff/42/01`）。如果不是，从能访问模块 AT 口的主机（Linux 上是 `/dev/ttyUSB2`，macOS 上走 libusb）发：

```
AT+QCFG="usbcfg",0x2C7C,0x0125,1,1,1,1,1,1,0     # 第 8 个字段 = 打开 ADB
AT+CFUN=1,1                                       # 重启模块
```

> 请填**你这块模块自己的 VID/PID**。出厂状态的大疆模块是 `0x2CA3,0x4006`；改之前一定要先用 `AT+QCFG="usbcfg"` 把当前值记下来。

然后：

```bash
adb devices          # 应能看到设备（序列号可能为空）
adb shell id         # 应输出 uid=0(root)
```

### 2. 编译

```bash
./build.sh           # 生成 smsweb-armv7（2.6MB，静态，Linux/ARMv7）
```

### 3. 部署

```bash
./deploy/install.sh             # 推送并启动（不动系统分区）
./deploy/install.sh --autostart # 顺便装开机自启（只往系统分区写一个符号链接）
```

脚本跑完会打印该打开的网址。

### 4. 使用

把模块插到 iPad / iPhone（配 USB-C 转接头）/ 笔记本上，打开：

```
http://192.168.225.1:8787
```

- **第一次打开**：页面显示你这张卡的号码和 ICCID，让你设置访问密码（至少 4 位），下面有 **「在本机记住登录」** 勾选框。
- **以后打开**：输密码；如果之前勾了记住，直接进。
- iPad 上建议 **「分享 → 添加到主屏幕」**，以后像 App 一样打开。
- 模块上电后约需 **40 秒**网页才能访问。

---

## 为什么用 PDU 模式（而不是"更简单"的文本模式）

文本模式（`AT+CMGF=1`）看起来更省事，但这块模块会把中文正文输出成**十六进制 UCS2**——而纯数字验证码 `123456` **本身也是一个合法的偶数长度十六进制串**。两者无法可靠区分，偏偏这又是最需要准确的场景。

所以本项目改用 **PDU 模式**（`AT+CMGF=0`）：编码方式由 GSM 03.40 的 DCS 字段显式标明，绝无歧义。实现内容包括：

- GSM 7 位打包/解包（含扩展字符表）；
- UCS2（UTF-16BE）编解码；
- 长短信 UDH 分片与重组（收发双向）；
- 字母数字与数字两种发件人地址；
- SCTS 时间戳解码（含时区）。

全部有单元测试覆盖，包括一条合成的两片 SMS-DELIVER PDU 向量，以及 **PBKDF2-HMAC-SHA256 的官方测试向量**。

---

## 安全设计

| | |
|---|---|
| 密码存储 | **PBKDF2-HMAC-SHA256，20000 轮 + 16 字节随机盐**，只存哈希，**永不存明文**。 |
| 随机数 | 取自内核 `/dev/urandom`。 |
| 会话 | 256 位随机 token，放在 **HttpOnly** Cookie（`SameSite=Lax`），页面脚本读不到。 |
| 记住登录 | token 有效期 1 年，并**持久化**到 `/data/smsweb/auth.json`，模块重启仍有效。 |
| 不记住 | 会话 Cookie + 服务端 12 小时过期。 |
| 防爆破 | 连错 5 次锁定 60 秒；密码比对使用**恒定时间比较**。 |
| 未登录 | `/api/state`、`/api/send`、`/api/delete` 全部返回 **401**，登录前页面拿不到任何短信内容。 |
| 暴露面 | 网页只在模块自己的 USB 网络里可达，**外网访问不到**。 |

传输是明文 HTTP（跑在模块私有的 USB 链路上），详见「已知限制」。

---

## 工作原理

```
 iPad / 任意浏览器
        │  HTTP :8787
 ┌──────┴──────────────────────────────────────────────┐
 │ QDC507 模块 — Linux 3.18.44, ARMv7, 43MB RAM        │
 │                                                     │
 │  smsweb（Go 1.21 静态编译）                          │
 │   ├── httpmux.go  自写极简 HTTP/1.1 服务器           │
 │   ├── ui.go       内嵌单页界面                       │
 │   ├── auth.go     PBKDF2 密码 + token 会话           │
 │   ├── at.go       AT 引擎：指令、URC 通知             │
 │   └── pdu.go      GSM 03.40 PDU 编解码               │
 │            │                                        │
 │            │  /dev/smd9（空闲 AT 通道）               │
 │            ▼                                        │
 │        基带 / SIM  ──►  蜂窝网络                     │
 └─────────────────────────────────────────────────────┘
```

一个有意思的细节：模块内部的 AT 通道里，`/dev/smd7` 被移远的 `ql_manager_server` 占着、`/dev/smd8` 被 `quectel-remotefs-service` 占着，**只有 `/dev/smd9` 完全空闲**——我们用的就是它，所以不打扰模块上任何原有服务，主机侧的 AT 口照样能用。

### 目录结构

```
.
├── main.go        HTTP 路由、短信存储/合并、验证码识别、密码接口
├── auth.go        PBKDF2 哈希、token、锁定、Cookie
├── at.go          AT 命令引擎、SIM 身份（ICCID/IMSI/MSISDN）
├── pdu.go         GSM 03.40 PDU 编解码（收、发、长短信分片）
├── httpmux.go     极简 HTTP/1.1 服务器（支持 Set-Cookie）
├── ui.go          内嵌界面：注册 / 登录 / 主界面 三态
├── build.sh       交叉编译到 Linux/ARMv7
├── deploy/
│   ├── install.sh 推送 + 启动（可选装开机自启）
│   └── start.sh   开机自启调用的启动脚本
├── pdu_test.go    PDU 单元测试
└── auth_test.go   密码/鉴权单元测试
```

---

## 为什么必须用 Go 1.21

**这个目标平台不能用 Go 1.24 及以上。** 它们的 `crypto/*` 包会连带引入 FIPS 140-3 模块，而该模块的初始化**在这个 3.18.44 内核上会立刻段错误**——`crypto/sha256`、`crypto/rand`、`crypto/pbkdf2` 全都在进程启动阶段就崩，`GOFIPS140=off` 也没用。这是我在设备上用一堆极小探针程序逐个包二分定位出来的。

同一类问题也发生在标准库的 `net/http` 上：它**也在这个内核上一初始化就段错误**（而单独用 `net` 完全正常）。所以本项目自己手写了：

- 基于 `net` 的 HTTP/1.1 服务器（`httpmux.go`）；
- 基于 `crypto/hmac` + `crypto/sha256` 的 PBKDF2-HMAC-SHA256（`auth.go`）；
- 用 `/dev/urandom` 替代 `crypto/rand`。

这三处改动把二进制从 6.6MB 降到 2.6MB，内存占用降到约 3MB。

### 编译

```bash
export GOTOOLCHAIN=local          # 别让 go.mod 去拉更新的工具链
export CGO_ENABLED=0 GOOS=linux GOARCH=arm GOARM=7
go build -trimpath -ldflags="-s -w" -o smsweb-armv7 .
```

### 测试

```bash
go test ./...                     # 宿主机上任意 Go >= 1.21 均可
```

> 注意：Go 1.21 编译出的**宿主机**二进制在 macOS 27+ 上跑不起来（dyld 报缺 `LC_UUID`），但**交叉编译到 Linux/ARMv7 完全正常**。在这种系统上可以用较新的 Go 跑测试、用 Go 1.21 出目标产物。

---

## 卸载

```bash
# 移除开机自启（这是唯一一处 /data 之外的改动）
adb shell "mount -o remount,rw /; rm -f /etc/rcS.d/S99smsweb; sync; mount -o remount,ro /"

# 移除其余全部文件
adb shell "kill \$(pidof smsweb); rm -rf /data/smsweb; sync"
```

全程不刷固件、不碰启动链，模块恢复成插上之前的样子。

---

## 实测环境

| | |
|---|---|
| 模块 | 大疆一代 4G 模块 — Baiwang **QDC507**，固件 `QDC507GLEFM21`，USB `2C7C:0125` |
| 系统 | Linux `3.18.44`（Quectel），ARMv7，43MB 内存，可写 `/data` 约 15MB |
| 客户端 | iPadOS Safari、macOS Safari、桌面 Chrome |
| 运营商 | 中国联通（任何支持短信的卡应该都可以） |
| 已验证 | 收短信（含两段长短信拼接）、发短信、验证码提取、按号码设密码/登录/错误密码/锁定/记住登录跨重启、模块重启后自动拉起 |

---

## 已知限制

1. **一次只能插一台设备**。模块是 USB 点对点猫，插在 iPad 上时电脑就用不了。
2. **上电后约 40 秒**网页才能访问（模块里的 Linux 要启动）。
3. **明文 HTTP**（跑在模块私有 USB 链路上）。密码本身是 PBKDF2 存储，但传输未加密——考虑到只有插着模块的那台设备能访问，风险可控。需要的话可以加 HTTPS。
4. **归档上限 300 条**，超出自动清理最旧的。
5. **实验性语音**。本分支已增加 iOS 原生 App 与模块内音频运行时，用户已确认真实外呼稳定且双方有声。持续讲话延迟修复待复测，后台来电唤醒尚未实现，详见 [IOS_CALLING.md](IOS_CALLING.md)。
6. **改装不受官方支持**。打开 ADB 会改动一项 NV 设置并重启模块，可逆、社区普遍在做，但请自担风险。

---

## 致谢与相关项目

本项目是在研究这批社区成果之后做出来的：

- [cr-zhichen/DJOneHubNative](https://github.com/cr-zhichen/DJOneHubNative) —— 其 `docs/MODULE_RESEARCH.md` 记录了模块内部结构、ADB 路线，以及固件为何被裁掉音频。
- [tinydream96/DJI-4G-Dongle-macOS-Config](https://github.com/tinydream96/DJI-4G-Dongle-macOS-Config) —— macOS 上用 libusb 发 AT 指令的可行做法。
- [mccding/CellBridge](https://github.com/mccding/CellBridge) —— 基于同一模块的 SIP/VoLTE 网关。
- [wlzh/dji-4g-vohive-mac](https://github.com/wlzh/dji-4g-vohive-mac) —— 这条硬件跑 VoHive（短信/代理/eSIM）的路线。

## 许可

[MIT](LICENSE)。

与 DJI、移远（Quectel）无任何隶属、背书或支持关系。"DJI"、"Quectel" 是各自所有者的商标。使用风险自负。
