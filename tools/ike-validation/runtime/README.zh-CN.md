# IKEv2 实机临时测试预备工具

先阅读 [完整方案](../TEMP_RUNTIME_TEST_PLAN.zh-CN.md)。这些源码和电脑端预备包不代表获得实机执行授权。本次构建、打包和 QEMU 检查都没有连接模块。

2026-11-14 新增 [短信与 VPN 共存测试方案](../COEXIST_TEST_PLAN.zh-CN.md)：`build-coexist.sh` 独立输出电脑 `coexist-build`，`prepare_coexist.py prepare` 生成未绑定包，缺少共存启动卡；后续必须使用当轮新鲜身份执行电脑端 `bind` 才会生成卡。共存接口为 `coexist RAM_ROOT RUN_COEX 30 TARGET VIP`，固定 30 秒、默认 12 MiB 准入、6 MiB/下降超过 3 MiB 停止线。`http-once LOCAL_IPV4` 仅检查本机 8787 的网页入口；`resume-clock` 恢复时钟后再 CONT 原监管。电脑工具准备及隔离实验已完成，实机准入结果见下文。

随后执行的 [D02 实机准入记录](../COEXIST_TEST_2026-11-14.zh-CN.md) 在暂停后 12132 KiB、低于 12288 KiB 时停止；短信已恢复、临时工具已清理，未上传客户端或加载 TUN，未进入共存窗口。

用户另行批准单轮 11.5 MiB 后，`prepare_coexist.py prepare` 可显式使用 `--client-admission-kib 11776 --approve-lower-admission-for RUN_ID`，两个 run ID 必须一致；默认和其他停止条件保持原值。绑定器同时检查例外与 8192/6144/3072 KiB 运行门槛，拒绝跨轮例外和篡改。[D03/D03B 实机记录](../COEXIST_TEST_2026-11-14_D03.zh-CN.md) 保存了前期年龄保护拒绝及其恢复，以及实际 9 组共存后触发内存下降停止的结果。

关键阶段应在开始独立恢复守护前完成电脑端准备；暂停后逐卡等待并核验结果，避免调度空档耗尽原有 180 秒守护启动年龄限制。LOGGER_READY 前退出会按原控制器策略标记连接不可重试，应关闭后用新的恢复连接先读回现场，再按归属清理，不得自动重放未知操作。

RouterOS 的 `print as-value` 返回值应经 `:put [...]` 明确输出；裸命令的空输出不能证明 peer 空闲。身份采集与专用 `in,ipsec` 计数应分别核验，最终只保留目标会话的必要元数据。

通过 macOS PTY 启动电脑控制器时，应在其专属终端关闭规范行缓冲和回显，并确保写入完整请求（本轮采用每次最多 400 字节）。1996 字节清理 JSON 超过该终端行缓冲时，控制器可能根本收不到换行；须先核对 begin 事件/阶段 raw 文件再决定处理，不能把它误认成 USB 错误或盲目重放模块操作。

## 构建与离线测试

```sh
sh tools/ike-validation/runtime/build.sh /Users/shield/Documents/ChatGPT/DJI-4G/ike-validation-local
```

该脚本只运行无网络、无 USB 挂载的 Linux 容器和主机 Go CPU 测试。输出 ARM 静态 qdc-ike-guard、原 USB 控制器的相同实现和 build.json；不重编 tun.ko。新的 C 监督器复用已有的日志解析及 smaps 解析，原 USB 控制器、信号防护、1 MiB 传输上限均未改变。

2026-11-14 起另输出同源码静态 musl qdc-tun-probe-musl（70940 字节），节省 454404 字节 RAM 暂存。它使用原 probe.c/packet.c/log_watch.c，不改变驱动或协议。当前 VM 用它再验证自检、无 GO 不执行、日志门控、features、创建、10 组双向小包；连同监督器既有 12 项场景共 18 项。prepare.py --compact-probe 必须读到摘要匹配的成功 VM 回执才接受它；默认仍保留原 glibc 测试器以重现历史记录。

纯电脑 ARM/Linux 3.18.44 集成验证：

```sh
docker run --rm --network none --platform linux/amd64 -v /Users/shield/.config/superpowers/worktrees/dji-qdc507-sms-web/tun-validation/tools:/src:ro -v /Users/shield/Documents/ChatGPT/DJI-4G/ike-validation-local:/export:ro -v /Users/shield/Documents/ChatGPT/DJI-4G/ike-validation-local/runtime-build:/out -v qdc507-ike-build-v1:/work qdc507-ike-builder:local sh /src/ike-validation/runtime/make-vm.sh
python3 tools/ike-validation/runtime/verify-vm.py /Users/shield/Documents/ChatGPT/DJI-4G/ike-validation-local --attempt 6
```

每次 attempt 使用新日志目录，重复编号拒绝覆盖。实验虚拟机没有网卡、外网、USB 直通或物理磁盘。实验内核使用已有通用 ARM 配置，单独关闭模块也未启用的 IPV6_SIT，避免通用配置自动生成 sit0；它不是模块固件。BusyBox、vm-fixture、initramfs 和 lab-only-runtime-zImage 全部排除出实机预备包。VM 制作过程只读主机 export，临时编译位于 Docker 命名卷。

## 工具接口

- qdc-ike-guard selftest：只读自身 proc 的 CPU/解析自检，不打开 TUN 或创建网络空间。
- describe PID：只读输出 PID/starttime/state/exe，供本轮取值。
- identity PID START EXE：只读核对身份，0 为相符，非 0 必须停止对应信号操作。
- idle RAM_ROOT TOKEN SECONDS：root 限定的受控运行，SECONDS 为 1–60。只接受 0700、root 所有的 /run/qdc-tun-* tmpfs；强制新网络空间、空连接配置、TUN 和 VICI 核验。未收到 GO 不创建运行目录或子进程。运行后有界回收，异常不会自行卸载驱动。
- resume SUP_PID START SECONDS：SECONDS 为 1–300，只允许精确短信 supervisor 的 argv/exe/starttime；忽略 HUP，定时或 TERM/INT 后重新核对并发 CONT。必须先确认 READY 和守护身份，再暂停 supervisor。它不会自行停止任何业务。

监督器并非通用命令执行器：idle 只执行同一 RAM 目录的 charon/swanctl，CLI 只查询 --stats、--list-conns、--list-sas。配置及程序摘要由打包器和实机阶段卡校验；监督器不内置 SHA-256，也不接受任意远端地址、连接文件或 shell 命令。

## 离线请求卡

```sh
python3 tools/ike-validation/runtime/prepare.py /Users/shield/Documents/ChatGPT/DJI-4G/ike-validation-local --tun-local /Users/shield/Documents/ChatGPT/DJI-4G/tun-validation-local --run IKE20261113PREP01 --out /Users/shield/Documents/ChatGPT/DJI-4G/ike-validation-local/prepared-IKE20261113PREP01
```

prepare.py 只读本机产物并写入新的本机目录，不导入 USB 库、不运行子进程，不会推送或执行任何卡。已有产物采用固定摘要，新监督器采用 build.json 的摘要；全部用户态程序检查 ELF。分片和合并均核对摘要，每个分片 ≤1 MiB。卡中 local 为绑定本机目录的绝对路径；移动预备目录或换电脑后应重新生成，不能直接沿用旧 local 路径。

按 manifest.stages 阅读和逐张发送，留出人工核对、短信恢复和字符节点清理步骤。没有 execute-all、一键安装、开机启动或刷机入口。执行阶段发生传输失败时仍按原控制器语义停止，不自动重试。

当前电脑端验收及摘要见 [preparation-result.json](preparation-result.json)。原始失败日志保留；没有被认定通过的尝试不能替代最终构建的集成验收。

模块原生空载实测已完成，见 [2026-11-14 执行记录](../RUNTIME_TEST_2026-11-14.zh-CN.md) 和 [runtime-result-2026-11-14.json](runtime-result-2026-11-14.json)。所有临时对象已清理，历史 preparation-result.json 仍保留当时仅准备、未实机执行的状态。

最终第 5 次集成验证包含 12 项：无 GO、正常回收、启动日志实时性、提前退出、kmsg 异常、CLI 超时、运行中内存骤降、输出阻塞硬时限、低内存准入拒绝、短信定时恢复、短信提前恢复，以及原网络地址/socket/sysctl 对照。stdout 方案曾因上游 file logger 对标准流的缓冲处理而延迟日志，最终空连接配置采用 stderr；不改动已验证客户端二进制。
