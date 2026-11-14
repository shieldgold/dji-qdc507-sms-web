# QDC507 IKEv2/IPsec 离线适配与内存评估

后续进展：2026-11-14 已完成 [模块原生空载启动与回收](RUNTIME_TEST_2026-11-14.zh-CN.md)。以下保留 11 月 13 日离线实验的数据及当时边界；空载实机结果不替代真实 VPN 认证与流量验收。

2026-11-13，Asia/Shanghai。**适配编译已完成；两个静态 ARM 程序在电脑上的 ARM/Linux 3.18.44 模拟环境中通过了 RSA 证书和 PSK 的 IKEv2 协商、100 包加密往返及 CHILD_SA 重协商。** 守护进程采样峰值约 1.7 MiB，管理工具另占约 0.8–1.0 MiB。建议按额外 4–6 MiB 做整机预算；模块最新可用内存约 9.5 MiB，因此具备继续受控验证的基础，长期流量和业务并存余量仍须实机确认。

本轮**只对模块进行了只读检查**，没有上传或执行客户端，没有重新加载 TUN、停止短信服务、改路由/DNS、刷机、重启或设置开机启动。所有编译和 VPN 协议测试均在电脑进行，使用自行生成的实验凭据，未接入真实 VPN 服务。机器可读记录为 [result.json](result.json)。

## 适配依据与产物

只读确认模块为 ARMv7 / Linux 3.18.44 / glibc 2.22；内核配置摘要与前次 TUN 验证一致。原厂未启用 XFRM_USER、IPv4 ESP 和 IPv4 多路由表，已启用 IPv6。采用 strongSwan 的 [kernel-libipsec 用户态后端](https://docs.strongswan.org/docs/latest/plugins/kernel-libipsec.html)，配合已验证的 TUN 和 kernel-netlink 网络接口；常规内核 IPsec 后端不作为本方案的依赖。

| 项目 | 实际结果 |
| --- | --- |
| strongSwan | 官方当前版本 [6.1.0](https://www.strongswan.org/download.html)，发布归档签名已核验 |
| libc | musl 1.2.6，附上游 iconv、qsort 修复；不依赖模块的 glibc 2.22 |
| 大整数库 | GMP 6.3.0，发布签名已核验 |
| 编译器/目标 | GCC 10.2.1；ARMv7、EABI5、软浮点调用约定、`-Os` |
| 链接 | 静态、monolithic 插件；没有 PT_INTERP 或动态段 |
| charon | 1,453,620 字节；SHA-256 `96bf484a445a1d9332a9614b231c5beeeb82bc47c8a9cf0aaf3bbc35642acf37` |
| swanctl | 906,892 字节；SHA-256 `0493bd368affea54ecf950701175ec6531c6d6de8c22b4889f7867a4ac81af7f` |
| 两程序合计 | 2,360,512 字节，约 2.25 MiB；这是文件占用 |

编译支持 IKEv2、PSK、RSA/X.509/PEM、EAP-Identity/MSCHAPv2、AES/GCM、SHA1/SHA2、GMP、Curve25519、VICI 等。RSA 和 PSK 已进行协议测试；EAP、Curve25519 协商、其他证书类型/封装和真实服务器互通未在本轮验收。现有 `tun.ko` 仍为前次 GCC 4.9.3 候选，没有重新构建或向模块加载。

构建使用独立 Linux Docker 卷，避免 Linux 头文件中的大小写文件名在 macOS 上冲突。libtool 的普通 `-static` 没有消除最终加载器依赖，已改为程序目标的 `-all-static` 并用 ELF 校验拒绝不完整产物。更新后的构建脚本再次执行，两个最终文件与协议测试所用文件逐字节一致。

[示例配置](strongswan.conf.example) 保留 16 个工作线程，选择用户态 IPsec，并使用 `routing_table=0` 适配模块缺少 IPv4 多路由表的条件。它没有真实连接或凭据，仍留在电脑，未应用到模块。

## 电脑端 ARM 验证

两台完整 QEMU ARM 虚拟机通过仅监听电脑 loopback 的私有以太链路连接，没有客体互联网出口或物理 USB 设备。使用通用 Linux 3.18.44 内核，关闭 XFRM_USER/IPv4 ESP，内置 TUN、启用 IPv6；该镜像**只用于模拟器，不是 QDC507 固件**。虚拟控制器呈现两颗 CPU 以满足旧 GIC 探测，`maxcpus=1` 使客体仅 CPU0 在线。每台 VM 分配 48 MiB，客体 MemTotal 为 41,300 KiB。

测试使用单个 IKE_SA/CHILD_SA、RSA-2048 实验 CA/终端证书或实验 PSK；协商 AES128-GCM16、PRF-HMAC-SHA256、MODP2048，ESP 走 UDP 封装，CHILD_SA 重协商验证 MODP2048 PFS。100 个 1,024 字节 echo 负载逐包往返，丢包为 0。每轮均正常终止守护进程，电脑端全部 QEMU 实例已结束。

8 工作线程的对照中，守护进程完成启动，但 VICI 配置装载未在主机时限内返回，没有计入通过结果。本轮采用验证通过的 16 工作线程，客体观察到 17 个进程线程。源码中多个常驻工作任务使减少线程需要单独验证；不根据“线程越少越省内存”直接采用 8 线程配置。

## 内存结果及计量方式

下表单位均为 KiB，来自客体 Linux 的进程数据，**没有把宿主机 QEMU 的 RSS 算成客户端占用**。`swanctl` 是执行管理操作时的临时进程。

| 场景，16 工作线程 | 装载凭据后空闲 RSS | 协商阶段采样 RSS 峰值 | 收发阶段采样 RSS 峰值 | 重协商阶段采样 RSS 峰值 | CLI 峰值 | 守护进程峰值 + CLI 峰值 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| RSA-2048 证书 | 1,620 | 1,748 | 1,756 | 1,748 | 980 | 2,736（2.67 MiB） |
| PSK | 1,476 | 1,716 | 1,728 | 1,736 | 836 | 2,572（2.51 MiB） |

最后一列是将两个峰值相加的保守估算，**不声称二者在同一瞬间出现**。RSA 与 PSK 守护进程的匿名内存采样峰值分别为 348 / 328 KiB，其他驻留部分主要为静态程序的文件页等。

`/proc/PID/status` 每约 20 ms 读取，完整 `smaps` 每约 100 ms 读取至 EOF；CLI 使用约 10 ms 的 smaps 采样并与 wait4/rusage 峰值择大。采用完整 smaps 是因为 [内核文档](https://www.kernel.org/doc/html/v5.7/filesystems/proc.html) 指出 RSS 计数存在可扩展性优化，页面明细更适合精确快照。早期未读至 EOF 的样本已作废，未进入上表。采样峰值仍可能漏过极短瞬时分配，不属于理论最大内存保证。

模拟环境的内核配置和业务背景与模块不同；这些是**同架构、同核心版本的参考测量**，并非 QDC507 上实际运行客户端的结果，也不用于推算模块 CPU 性能或吞吐。

## 模块余量与预算结论

本轮初始三次 MemAvailable 为 9,616 / 9,648 / 9,648 KiB；22:08 的末次三次均为 **9,724 KiB，约 9.50 MiB**。MemTotal 为 43,460 KiB，无 swap，本轮未暂停原业务。boot_id、taint、内核配置、原语音模块保持一致，短信网页返回 HTTP 200；没有发现本轮客户端目录或 TUN 模块。

建议将**额外 4–6 MiB** 作为第一轮实机试运行的规划预算，包括程序、运行对象、TUN 和内核/队列余量。若未来在 RAM 暂存，两程序的文件页约 2.25 MiB；它们与进程 RSS 存在重叠，不能再把完整 RSS 重复相加。参考匿名内存以及约 1–3 MiB 的内核/队列规划余量，形成上述预算；这部分余量是规划值，不是本轮实测的整机峰值。

按该预算计算，模块剩余可用内存约 3.5–5.5 MiB，余量偏紧。轻量单连接具备继续验证的基础；目前不能直接把模拟器通过等同于实机常驻稳定。尤其是 [官方对 libipsec 的说明](https://docs.strongswan.org/docs/latest/plugins/kernel-libipsec.html) 指出该后端会在内存中缓存数据包，CPU 不足和突发流量可能造成内存增长，4–6 MiB 预算不构成高流量硬上限。

本轮没有验证真实 VPN 端点、运营商链路、路由/DNS 接管、IPv6 加密收发、IKE_SA 重协商/重新认证、长期内存增长或吞吐。后续实机运行需要另行进行受控测试；本轮停止在电脑端产物和评估阶段。

## 留档

源码、构建脚本、校验器、VM 实验脚本和脱敏结果保存在本目录。源归档及固定摘要见 [inputs.json](inputs.json)，原始编译/VM 日志保留在电脑 `ike-validation-local/logs/`，模块只读日志为 `tun-validation-local/runtime-logs/R20261113IKE01/` 和 `R20261113IKE02/`。实验私钥、原始设备日志、固件镜像均未提交 Git。
