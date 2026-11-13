# TUN 临时测试工具

这些工具用于执行 [临时加载方案](../TEMP_LOAD_TEST_PLAN.zh-CN.md)，不属于短信网页或设备的常驻服务。构建脚本只在电脑和不接 USB 的容器里编译、运行模拟/CPU 测试；不会连接模块。

```sh
tools/tun-validation/runtime/build.sh /Users/shield/Documents/ChatGPT/DJI-4G/tun-validation-local
```

主机需要 Go、macOS/cgo/IOKit 和已有 `qdc507-tun-gcc493:local` 镜像。Go 子进程单独移除错误的 GOROOT 环境覆盖，未修改电脑全局配置。ARM **用户态**测试器使用镜像里的 Debian GCC 10.2.1 和静态 libc 构建；原内核 `tun.ko` 仍是 GCC 4.9.3 产物，没有改用这套编译器重新构建。

## 主机控制器

`host/` 只依赖 Go 标准库及 IOKit。启动时必须指定精确 VID、PID、USB location、唯一运行 ID 和全新的主机日志目录。只打开匹配设备的 ADB bulk 接口，不打开 AT 接口，不 seize、不 reset、不改 USB 配置，不自动重连或重放命令。

使用逐行 JSON 输入，类型为 `shell`、`push`、`quit`。`shell` 的 `id` 必须唯一，命令有长度和时限约束；`push` 只允许已人工核对为 tmpfs 的 `/run/qdc-tun-*` 目录，最大 1 MiB，使用 ADB sync 服务而不反复 fork shell。目录是否位于 RAM 仍须先按方案只读核验，路径前缀本身不能证明这一点。

每个 WRTE 输出在 ACK 前写入主机独立 `.raw` 文件并 `fsync`。旧版 shell 协议无远端退出码，因此加入本轮独有 BEGIN/END 标记和 RC，缺失标记或传输异常不算成功。原始文件拒绝覆盖，超时或断线保留已收到的部分输出，传输失败后该客户端停止接受新操作。恢复需人工关闭旧连接、重新核对设备并打开新连接，不重放旧动作。

`ready` 字段用于执行门控：完整收到指定 LOGGER_READY token、输出成功落盘后才向同一 ADB stream 发送 `GO\n`。写文件失败时不会发送 GO。准备、每阶段动作及恢复判断仍由操作者逐项执行，不提供一键连续加载/收发/卸载脚本。

输入应通过管道或关闭 canonical 限长的专用 PTY。macOS 默认 canonical PTY 会截断长 JSON 行；本轮曾遇到一条请求在主机侧被截断，控制器拒绝解析，未进入设备执行。通过交互终端运行时，先在该专用终端执行 `stty -icanon -echo`；不要对用户其他终端修改设置。

重新建立连接时可能残留旧 stream 数据。当前版本在有界握手/OPEN 等待中留存这些包到主机事件文件，再等待当前确认；这修复了把首个旧 WRTE 当成握手失败的问题。它**没有证明最初 IOKit `kIOReturnAborted` 的原因**，也不会忽略该错误。

后续已单独复现同步 IOKit 调用被 SIGURG 打断的故障，并在当前 C 线程的 USB 读写期间暂缓该信号，返回前恢复原掩码。其他信号和所有真实 I/O 错误保持原行为，不自动重试，见 [USB 专项记录](../USB_LOG_CHANNEL.zh-CN.md)。该记录也保留首轮事件缺少当时信号证据的限制。

可选 `-usb-trace` 在主机目录中另建 0600 的 `usb-transfers.jsonl`，逐条记录并 fsync 传输元数据，不含 payload；落盘失败即停止。deadline_ms 是阶段剩余时限，单次 IOKit 读取仍上限 500 ms。空闲时 0 字节读取是正常轮询；发生写入错误时不能根据 transferred 字段推定远端没有收到数据，仍按未知状态处理并禁止重放。

## ARM 测试器

`qdc-tun-probe` 是 525,344 字节的 ARMv7/EABI5/soft-float 静态程序。首轮只执行 `selftest` 和 `watch`；第二轮已实机完成下面各功能模式，并清理所有临时对象。

- `selftest`：CPU 内部的数据包/校验和及日志解析自检，不访问 TUN。
- `watch TOKEN OBSERVE_SECONDS COMMAND_TIMEOUT ABSOLUTE_PROGRAM [ARGS...]`：先只读打开 `/dev/kmsg` 并定位到当前末端，收到主机 GO 才启动一个子命令。采集原始 kmsg 记录、序号、内存及父/子 RSS。日志丢失、错误、低内存或超时均返回失败，不主动卸载驱动。新内核异常时可能暂停仍在运行的子进程并保留现场，必须按 PID 和日志进入恢复流程；不能假设异常返回已经释放全部资源。
- `features PRIVATE_NODE`：打开、TUNGETFEATURES、关闭；要求 root 拥有、0600、非符号链接的专用 `c 10:200` 节点。
- `create PRIVATE_NODE RUN_ID`：进入新网络命名空间，创建单队列、非持久 `qdcvt0`，关闭后通过本命名空间的网络 API 检查删除。
- `traffic PRIVATE_NODE RUN_ID`：在上述私有网络中设置文档测试地址和小包 UDP 回环模拟，逐一验证 10 组双向内容，再关闭并核对接口删除。不开多队列、GSO 或持久接口。

测试器对虚拟地址空间、栈、FD、核心转储和功能模式时长设置上限；这些用户态限制不能取消已经进入内核的调用，也不能保证异常内核可以恢复。内核模块和命名空间测试仍必须满足方案的日志、内存、恢复和业务条件。

## 验证边界

Go 的 15 项测试覆盖即时输出、部分日志保留、不重试、门控、错误包、远端状态、sync 推送、文件覆盖保护、残留包恢复及 USB trace 落盘失败时停止；C/Python 检查覆盖包编码/校验和/损坏包/边界、日志序号及错误识别、GO 前不执行、EOF 不执行、CLI 非法请求、CPU 自检和主机信号掩码恢复。数据包测试启用 AddressSanitizer/UBSan，容器模拟环境下禁用 LeakSanitizer。

`test_usb_signal_darwin.c` 是需要实机的人工诊断，不由 build.sh 自动运行；必须先关闭其他 USB 控制器，详见 USB 专项记录。

离线检查不能代替实机验收。首次实测中止及正常卸载见 [首轮记录](../RUNTIME_TEST_2026-11-13.zh-CN.md)；修复后完整的最小 IPv4 TUN 验收及适用范围见 [第二轮记录](../RUNTIME_TEST_2026-11-13_ROUND2.zh-CN.md)。
