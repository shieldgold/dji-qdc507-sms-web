# 完整短信服务的电脑隔离 ARM 实验

本目录仅包含电脑上的实验工具，不能复制到真实模块执行。它运行完整的 `smsweb` ARM 二进制，使用 `/dev/ptmx` 建立的 raw PTY 合成基带，不读取真实 SIM、短信、凭据或 AT 通道。

前提是本地已有 `qdc507-ike-builder:local`、`qdc507-ike-build-v1` 和私有实验内核 `runtime-build/lab-only-runtime-zImage`。内核是通用 Linux 3.18.44，现有配置支持 UNIX98 PTY、devtmpfs、devpts 和完整 smaps，无需修改内核。容器禁网，没有 USB/主机设备挂载；QEMU 使用 `-nic none`，仅访客内部回环接口开启。

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tools/sms-validation -p 'test_*.py'
PYTHONDONTWRITEBYTECODE=1 python3 tools/sms-validation/run_vm.py \
  /absolute/ike-validation-local --binary /absolute/smsweb-armv7 \
  --label before --scenario ready --attempt 1
```

`--scenario pin` 从合成的 SIM PIN 状态开始，通过网页 API 解锁一次。合成身份、PIN、口令及消息都固定在夹具中，绝不替换为真实资料。测试覆盖未登录访问保护、注册登录 cookie、合成长短信与验证码、无换行 CMGS 提示符、合成发送完成及请求 ID 去重。纯协议单元测试另覆盖 ESC 取消与错误 PIN。

两组对照固定为 48 MiB guest、一个启用 CPU、Go `GOMAXPROCS=1`、`GOGC=20`、12 MiB Go soft memory target、3 秒轮询；不增加诊断 goroutine，不改二进制。采样阶段为 HTTP 首次就绪、功能流程后继续轮询、64 个静态资源请求、2 MiB 合成内存压力、压力释放。压力阶段执行 8 MiB 准入、6 MiB 绝对下限与下降不超过 3 MiB 的检查。这里没有放宽既有 VPN guard 的任何条件。

输出放在私有 `sms-opt-build/logs/<label>-<scenario>-<attempt>/`，不会覆盖旧 `coexist-build` 或 `coexist-opt-build`。每次构建只临时写入共享 Docker volume 的独立 `/work/sms-runtime-root.*`。`serial.log` 留存各阶段完整 smaps 与 Go GC 行；`result.json` 汇总二进制、内核、夹具、initramfs 的 SHA 和纯数值元数据。未知成功标记、缺阶段、未发生轮询/提交或运行时异常均导致失败。

`python3 tools/sms-validation/compare.py /private/before/result.json /private/after/result.json` 可生成逐阶段 RSS/PSS/Anonymous 差值。只有场景、内核、夹具二进制及其完整运行源码哈希、Go/内存参数完全相同且两次都通过时才比较。HTTP 客户端验证 Content-Length 与实际正文完整长度，拒绝截断、重复长度及溢出值。

度量口径：

- `rss_kib` / `pss_kib` 为完整 smaps 所有映射之和，`anonymous_kib` 为 Anonymous 字段之和。
- `file_rss_kib` 定义为 RSS 减 Anonymous，表示非匿名驻留页，含 vdso 等少量非文件页；精确文件归因应查看保留的原始映射。
- `peak_observed_rss_kib` 取已采到的 smaps RSS 与 `/proc/PID/status` VmRSS 最大值；后者在旧内核上是近似数，结果不等于连续测得的绝对峰值。
- `go_gc` 描述 GC 堆的整数 MiB 数值，低于 1 MiB 会显示为 0；`go_pacer` 另保留 GC 触发及标记结束时的字节数。两者都不能当作进程 RSS。Go soft memory limit 也不是进程或系统硬上限。

通用虚拟机的驱动、页缓存与固件环境不同，结果证明的是该 Linux 版本下的完整应用行为及相同环境中的内存对照，不代表真实模块已完成部署或短信/VPN共存验收。

兼容 Go 编译使用仓库 `build.sh`，固定 Go 1.23.12，与电脑分配量对比保持一致。脚本隔离本机持久 Go 配置、禁止工具链下载，并保持 ARMv7/CGO0、去除调试信息和 4 KiB ELF 布局；可显式保留 64 KiB 布局对照：

```sh
QDC_GO=/absolute/go1.23.12/bin/go QDC_OUTPUT=/private/output/smsweb-armv7 sh build.sh
# 对照布局：QDC_ELF_ROUND=65536
# 小体积配置：QDC_BUILD_PROFILE=memory（显式关闭内联，CPU/内存取舍见结果报告）
```

原部署二进制与固定 Git 基线的源码、VCS 元数据可能不同。应保留并分别标记原部署 SHA、Git 基线重建 SHA、当前优化 SHA；源码逐文件相同与二进制哈希相同是两项不同的证据。

其他独立验证：

- [完整短信服务与真实 charon 组合](combined/README.md)：加密 ICMP 与本地短信 HTTP 分开计数，后者不走 VPN。
- [时钟恢复失败及 SIGUSR1 委托](clock-failure/README.md)：真实 ARM capability 故障注入，不改变宿主时钟。
- [临时文件绑定与回退工具](swap/README.zh-CN.md)：身份、挂载、复制期限和内存/存储拒绝；不构成实机执行授权。
- [实机存储复核](STORAGE_REVIEW_2026-11-15.zh-CN.md)：已做只读备份，模块原文件仍在，当前容量不满足复制准入。
