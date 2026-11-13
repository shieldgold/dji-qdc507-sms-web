# USB 日志中止的定位与防护

2026-11-13，Asia/Shanghai。本文记录电脑端 IOKit/Go 采集程序的信号中断问题；TUN 的运行结果另见实测记录。

## 可复现的问题

首轮实测在加载后约 15 秒，`ReadPipeTO` 返回 `0xe00002eb`，即 `kIOReturnAborted`。设备没有重新枚举，boot_id 没有改变。当时没有记录到主机信号，因此不能事后断言具体是哪一个信号、由哪个线程发出。

Apple 的 [IOUSBLib.h](https://github.com/apple-oss-distributions/IOUSBFamily/blob/main/IOUSBFamily/Headers/IOUSBLib.h) 明确说明：同步 `ReadPipeTO` 在线程被中断时可返回 `kIOReturnAborted`；错误时 size 参数不更新、缓冲区数据无效。Go 1.25.6 的 [signal_unix.go](https://github.com/golang/go/blob/go1.25.6/src/runtime/signal_unix.go) 使用 `SIGURG` 进行异步抢占。它们提供了需要实测的主机侧故障路径，不能单凭错误码就归因于模块或 TUN。

在没有加载 TUN、没有发送 ADB 命令的情况下，通过相同 IOKit 接口对空闲 ADB IN 端点读取，并在 100 毫秒后向**读取线程**定向发送 SIGURG：

| 对照 | 次数 | 结果 |
| --- | --- | --- |
| 原始实现，不屏蔽信号 | 3 | 均在约 102–110 ms 返回 `0xe00002eb`，回归程序非零退出 |
| 原始实现，仅外层暂缓 SIGURG | 3 | 均正常等到约 1 秒的空闲超时，信号随后送达 |
| 修复实现，调用者不额外屏蔽 | 3 | 均正常等到约 1 秒的空闲超时，回归程序退出 0 |

原始实现中止后的 GetPipeStatus 为 `0xe000404f`。这只是该失败路径的观察值，没有据此执行 ClearPipeStall、USB reset 或重启。错误消息中的 returned 数值是 IOKit 留下的 size 参数，**不代表有效读取字节数**。

早先另做过向整个 Go 进程发送 10 次 SIGURG 的对照，没有中止；进程级投递无法保证信号送达正在读取的那个线程，不能替代上面的定向复现。

## 最小防护

`runtime/host/signal_guard.h` 仅在当前 C 线程执行同步 ReadPipeTO/WritePipeTO 期间暂缓 SIGURG，并在返回 Go 前恢复原有信号掩码。其他信号保持原状，SIGINT/SIGTERM 不受影响。读取保持原来的有限超时；没有把 abort 当作成功或超时，没有自动重试、重连或重放远端命令。

因此，已确认并修复的是**同步 USB I/O 易受 SIGURG 打断的主机实现缺陷**。首轮历史事件与该故障路径一致，但缺少当时的信号记录，不把历史归因写成已经完全证明。

另增加可选 `-usb-trace`：将每次传输的主机时间、方向、请求/有效长度、耗时、阶段剩余时限和错误存入独立 0600 文件，逐条 fsync；不在此文件存放 ADB payload。该记录落盘失败同样停止操作。IOKit 的实际单次读取超时仍上限 500 ms；trace 的 deadline_ms 是 ADB 阶段剩余时限。正常空闲轮询可返回 0 字节，不计作断线。

## 可重复的检查

普通 `runtime/build.sh` 在电脑上运行不接设备的信号掩码测试：验证 SIGURG 在阻塞等待期间延期送达、返回后送达、所有原有信号掩码恢复。Go 测试增加了部分数据/错误保留和 trace 落盘失败时禁止继续发送命令的检查。

`runtime/test_usb_signal_darwin.c` 是**需要实机 USB 的人工诊断程序**，不会由 build.sh 自动执行。它必须在其他控制器关闭后单独运行，只读空闲 IN 端点，不发送 ADB 命令。该文件固定本轮已核对的 VID/PID/location；换设备必须重新核对和修改。编译时显式链接 `runtime/host/usb_darwin.c`、IOKit 和 CoreFoundation。

原始证据只保留在电脑 `tun-validation-local/runtime-logs/R20261113USB01/`，包括修复前后回归输出、外层屏蔽对照及进程级信号试验；不把原始设备日志提交到 Git。后续长时间采集和 TUN 实测结果应以运行记录为准，不能用这项回归代替长期稳定性证明。
