# iOS App 直连模块通话

当前版本：0.1.0，开发验证版。模块运行服务和声音桥，iPhone 运行“模块电话”App；日常使用不依赖 Mac。

## 当前验收状态（2026-11-12）

- iPhone 15 Pro 上已安装签名 App，包含拨号、接听、拒接/挂断、静音、扬声器、通话按键和短信网页。
- 模块 QDC507GLEFM21 / Linux 3.18.44 已加载驱动、完成静态 ACDB 校准；重启后自动恢复。未修改固件、引导分区或 USB 组合。
- IMS 配置已由 `0` 改为 `1`，重启与用户在网页解锁 SIM 后读回 `1,1`；LTE 注册状态为 `1`。配置和能力不能单独证明 IMS 注册，真实呼叫另行验收。
- 短测试电话已出现语音 `active`，随后 `AT+CHUP` 挂断；两条数据连接保留。之后通过模块 USB 网卡实际访问外部 HTTPS 页面，返回 HTTP 200。
- 第二次授权声音测试接通后运行约 8 秒：上行送入 128,000 字节，下行收到 127,040 字节，其中 62,538 个样本非零，峰值 3,972。仅记录计数与幅度，未录音保存内容；接听端能否清楚听到提示音仍等待用户反馈。
- 模块空闲 PCM 曾无节流返回静音，已增加 8 kHz 时钟限速；新增回归用例先复现洪流再验证修复。修复后的实机空闲检查为上行 128,000、下行 126,720 字节/约 8 秒，正常退出。
- **iPhone USB 直连、双方可听见、来电接听、蓝牙/扬声器切换仍未验收。** 当前模块连接 Mac，用户暂不能切换接线。
- 来电发现依赖 App 前台轮询。CallKit 提供系统通话界面，未实现 PushKit/APNs 后台来电唤醒；不能当作普通手机电话的全天候替代。

## 在 iPhone 上使用

1. 将模块接到支持供电与 USB 网络的 iPhone 连接方式，确认能打开 `http://192.168.225.1:8787`。
2. 模块每次重新上电后，若 SIM 要求 PIN，在网页中手动输入。服务不保存 PIN，也不自动重试。
3. 打开“模块电话”→“连接设置”，使用原有模块网页访问密码登录。初次设置密码可在“短信与模块”页完成。
4. 同意本地网络和麦克风权限。音频运行时未就绪时禁用拨号；来电测试期间保持 App 前台。
5. 实测时分别确认：对端响铃、接通状态、双方声音、静音、扬声器、挂断后模块数据连接继续工作。

没有音频时先查看 `/tmp/smsweb-voice-prepare.log`、`/tmp/smsweb-alsaucm.log` 和 `/tmp/smsweb-voice.log`。`audio.available` 表示运行时准备完成；`streaming` 和字节计数表示数据流动，均不代替听感验收。

## 实现与边界

- HTTP 控制沿用原有 SIM 账号鉴权：`GET /api/calls`；`POST /api/calls/dial|answer|hangup|dtmf|audio`。
- 排除 `AT+CLCC` 的数据连接；每通电话有独立标识，旧标识不可操纵后续电话。第一版仅支持一路电话。
- 拨号请求先持久化再发送 ATD，防止超时重试/进程重启重复拨号。使用 `AT+CHUP` 只结束语音，维护和短信提交与拨号互斥。
- 原生 App 直接使用 AVAudioEngine 与 TCP PCM，**本版没有实现 WebRTC**。格式为 8 kHz、单声道、S16LE、每帧 320 字节。
- 音频仅监听 USB 网络地址 `192.168.225.1:8788`。凭据由已登录控制接口签发，30 秒失效、一次性使用且绑定当前电话；上/下行队列与日志有容量限制。
- 当前 HTTP/PCM 未加 TLS，仅面向受信任的本地 USB 链路；不要将端口映射到外网或公共网络。以后远程使用需另行增加加密传输。
- 认证音频连接在响铃或通话中断开时，仅尝试挂断它绑定的电话；不会拿旧凭据接入下一通。
- 模块厂商工具的 ACPH 在线调音初始化崩溃。本版仅对已校验哈希的固件库禁用可选在线调音客户端，保留静态校准加载和 ioctl；不覆盖系统库。就绪判据同时检查校准进程 PID、启动时间、命令与声卡节点。

## 构建和验证

模块服务使用 Go 1.21–1.23；不要用 Go 1.24+ 构建此旧内核目标。设置本机正确的 `GOROOT`、`GOCACHE` 后运行：

```sh
go test -race ./...
npm test
./build.sh
./voice/build.sh
swift test --package-path ios/Core
xcodegen generate --spec ios/project.yml
xcodebuild -project ios/ModulePhone.xcodeproj -scheme ModulePhone \
  -destination 'generic/platform=iOS' -allowProvisioningUpdates build
```

iOS 工程使用本机已有开发团队；其他用户应在 Xcode 的 Signing 中选择自己的团队。免费开发签名的有效期和设备许可由 Apple 决定，本工程未提供 App Store/TestFlight 发布。最新构建已安装，安装后的再次前台启动因设备锁屏暂未复验。

语音驱动来源：`moluncn/mavo`，提交 `0443dfdaf8aec086fd76ba2ee9152fd908114524` 的 `Resources/ModuleVoice`；本仓库保留 MaVo 许可并提供修改后的用户态桥源码。驱动文件不内嵌进服务，部署时校验 `voice/prepare.sh` 中的 SHA-256。

运行时目录 `/data/smsweb/voice-runtime` 需要：

- `qdc507_aprv3.ko`、`qdc507_voice.ko`（上述固定版本）；
- `network-pcm-bridge`、`offline-calibration.so`（`voice/build.sh` 生成）；
- 可执行的 `prepare.sh`。

监督脚本在启动时尝试初始化，失败保留日志并继续提供短信网页。启动目录和 API 默认地址为本模块专用，未对其他型号声明兼容。

## 回退

部署前已将模块 `/data/smsweb` 和启动入口归档到本机备份目录。短信、密码与 SIM PIN 均不进入 Git。

需要回退时先结束电话，停止监督服务，再还原备份中的服务文件和启动脚本，保留当前短信/账号数据。删除或停用语音初始化脚本后重启可卸载内存驱动；SIM 可能需要再次在网页解锁。若需恢复旧 IMS 配置，使用 `AT+QCFG="ims",0` 并重启，不能在有电话时操作。
