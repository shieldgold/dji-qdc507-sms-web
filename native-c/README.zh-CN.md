# 独立 C 版短信管理服务

本目录提供独立的 C11/POSIX 后端，复用 `../web/` 原生 HTML/CSS/JavaScript。模块上不需要 Go、Node.js 或前端框架运行时。现有 Go 源码和构建入口保留。

这是经过电脑、隔离 ARM 实验和一次 QDC507 受控临时共存测试的候选实现。实机完成 30.105 秒、10 组 VPN 加密 ping 与本地短信 HTTP，C 短信 RSS 采样最高 700 KiB；随后已恢复原 Go 服务并清理候选。详见 [实机验收记录](DEVICE_COEXIST_2026-11-15.zh-CN.md)。未刷机、改启动文件、发送真实短信/通知或拨打真实电话。

## 功能和接口

30 个 API 的完整路径和鉴权边界见 [routes.json](compat/routes.json)。涵盖短信列表/验证码/PDU/分片/发送队列/删除，PIN 保护和换卡绑定，账户/持久会话/浏览器单次票据，网络/设备/流量，语音呼叫及 PCM 桥接，APNs ES256/TLS/HTTP2，应用时钟、导出/合并恢复和版本恢复。

`messages.json` 仍为 key→Message 对象映射；`auth.json`、`traffic.json`、`clock.json`、`push-state.json`、`call-attempts.json`、PIN guard 保持原格式。验证码、正文和 key 按长度保存，保留 NUL、中文、emoji 和 uint64。JSON 请求按 Go 的字段大小写、重复字段顺序、标量 null 和错误类型规则处理。损坏/无法读取的关键归档保留原文件，拒绝覆盖。

发送意图与拨号/PIN 保护先可靠保存，才允许 AT 动作。未知短信提交结果不重试；导入索引强制归零，queued/sending 转 uncertain，不生成发送任务。关闭/溢出的 AT 通道不再写命令。来电与音频均绑定当次 call ID，不能用旧票据挂断下一通电话。

原生音频沿用已有 `voice-runtime/network-pcm-bridge` 与厂商音频运行环境。`available` 要求监听器、设备节点、桥接文件和精确活校准进程同时成立；后端二进制不包含厂商音频库。APNs 保留开发环境/专用 topic/固定 Apple 端点、证书链与主机名校验；本轮只验证合成对端，未连接 Apple。

## 构建

需要 C 编译器、make、Python 3 和依赖源码所需的构建工具。依赖固定为 Mbed TLS 3.6.7、nghttp2 1.71.0、yyjson 0.13.0，下载 URL、官方或本地核对的 SHA、许可证见 [deps.lock.json](deps.lock.json)。依赖源码、库、构建产物放在缓存目录，不进入 Git。

在仓库根目录：

```sh
python3 native-c/tools/build_deps.py --target host
make -C native-c
```

可显式指定 `QDC_DEPS=/absolute/prefix` 和 `BUILD=/absolute/output`。静态资源在电脑构建时嵌入，四个文件逐字节不变。

ARMv7 soft-float 使用已验证的 GCC 10.2.1 / musl 1.2.6 工具链（不是模块内核编译用 GCC 4.9.3）。运行时不依赖模块 glibc。先在工具链环境构建依赖，再构建服务：

```sh
python3 native-c/tools/build_deps.py --target arm \
  --work /work/native-c-deps-v1 --prefix /work/native-c-deps-v1/prefix
make -C native-c BUILD=/out/build QDC_DEPS=/work/native-c-deps-v1/prefix \
  CC=/work/toolchain/bin/musl-gcc \
  CFLAGS='-std=c11 -Os -g -Wall -Wextra -Wpedantic -ffunction-sections -fdata-sections -pthread -march=armv7-a -mfloat-abi=soft -static' \
  LDFLAGS='-Wl,--gc-sections,-z,noexecstack'
arm-linux-gnueabi-strip -o /out/smsweb-c-armv7 /out/build/smsweb-c
```

交付前检查 `readelf -h -l -d -A`：ELF32/EABI5、soft-float、无 INTERP/NEEDED、非可执行栈。工具链和模拟器不需要安装到模块。

## 本地验证

```sh
QDC_GO_ROOT=/absolute/go QDC_GO=/absolute/go/bin/go \
  make -C native-c test QDC_SANITIZE=1 BUILD=build/asan
```

Go 仅用于兼容性对照，不进入 C 服务。测试包括公共内存/文件/HTTP、2,376 组出站和 6,000 组入站 PDU 差分、真实 PTY、Go↔C 账户文件、并发单票兑换、合成 TLS/DNS/APNs HTTP2、双向 PCM、三类持久化分配故障、300 条短信 2,000 轮内存上界，以及完整服务的 30 API/PIN 流程。网络夹具只监听/访问回环；凭据和证书均临时合成。

四个前端原文件另通过原项目的 13 项 Node 测试。本轮最终结果、体积/内存和验收范围见 [RESULT_2026-11-15.zh-CN.md](RESULT_2026-11-15.zh-CN.md)。

ARM 隔离实验入口位于 [../tools/native-c-validation](../tools/native-c-validation/README.zh-CN.md)。它复用原完整短信/16 线程 charon 夹具，保护阈值不变，不接触 USB 或 WAN。真实模块另已通过上述短窗口蜂窝 VPN 共存；实际语音、APNs、长时稳定性和性能仍需单独验收，不能由回环或短窗口结果代替。

## 运行边界

命令参数保持 `-at`、`-listen`、`-port`、`-data`、`-poll`、`-debug`；也接受双横线。默认 `/dev/smd9`、`192.168.225.1:8787`、`/data/smsweb`、3 秒轮询；监听地址只允许模块 USB 地址或 `127.0.0.1`。调试开关不输出 PIN、密码、令牌、私钥和短信正文。

```sh
# 仅示例：电脑上的合成 AT 通道和临时目录
native-c/build/host/smsweb-c -at /absolute/synthetic-pty \
  -listen 127.0.0.1 -port 8787 -data /absolute/private-test-data -poll 3s
```

应用时钟只用于 TLS/事件时间，不写系统时钟。会话期限保留原系统时间语义；AT 等待、音频票据、锁定和浏览器票据使用单调时间。Linux 3.18 的早期随机池可能尚未就绪；C 返回可重试错误而不无限阻塞，所有随机输出仍必须通过 `getrandom` 就绪检查。隔离实验对 C/Go 使用同一显式熵预检，不伪造随机、熵计数或成功标记。

保存使用同目录独占临时文件、文件 fsync、rename、目录 fsync；遗留常规 `.tmp` 保留供恢复且不阻断新保存，目录/链接形式的 `.tmp` 拒绝。rename 后目录 fsync 失败明确视为不确定提交，关键组件停止继续覆盖/发放凭据，不能声称磁盘一定没变。

常驻后台、HTTP 和音频线程栈为 128 KiB，HTTP 并发最多 8、音频握手最多 3，AT 单行/排队正文有 1 MiB 上限，音频诊断日志最多 64 KiB。保持原 300 条归档和请求限制；活跃发送记录仍可能突破普通归档裁剪数量，不能把这些常数当系统 RAM 硬上限。

本实现保留 50 分钟 JWT 缓存，TLS 按请求建立连接以释放空闲 TLS 内存；吞吐/握手成本尚需实机测量。Mbed TLS 的维护交接时间已记录在依赖锁文件，后续升级须重新过协议、证书和 ARM 验证，不应把固定版本视为长期免维护。
