# IKEv2 实机临时测试预备工具

先阅读 [完整方案](../TEMP_RUNTIME_TEST_PLAN.zh-CN.md)。这些源码和电脑端预备包不代表获得实机执行授权。本次构建、打包和 QEMU 检查都没有连接模块。

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

最终第 5 次集成验证包含 12 项：无 GO、正常回收、启动日志实时性、提前退出、kmsg 异常、CLI 超时、运行中内存骤降、输出阻塞硬时限、低内存准入拒绝、短信定时恢复、短信提前恢复，以及原网络地址/socket/sysctl 对照。stdout 方案曾因上游 file logger 对标准流的缓冲处理而延迟日志，最终空连接配置采用 stderr；不改动已验证客户端二进制。
