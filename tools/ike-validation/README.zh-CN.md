# QDC507 IKEv2/IPsec 离线验证工具

本目录构建客户端、运行 ARM 模拟实验并提供 [实机临时测试方案](TEMP_RUNTIME_TEST_PLAN.zh-CN.md) 与 [预备工具](runtime/README.zh-CN.md)。2026-11-14 已在模块 RAM 中通过 10 秒及 60 秒空载启动与回收，见 [实机记录](RUNTIME_TEST_2026-11-14.zh-CN.md)；真实 VPN 连接、业务共存和长期稳定性尚未验证，没有常驻安装或开机启动入口。原离线编译结论见 [评估报告](ASSESSMENT.zh-CN.md) 和 [result.json](result.json)。

当前产物为 strongSwan 6.1.0、静态 musl、GMP、用户态 IPsec 后端，ARMv7/EABI5/soft-float。依赖现有 `qdc507-tun-gcc493:local` 基础镜像、Docker、Python、QEMU。基础镜像的来源见 [TUN 构建记录](../tun-validation/README.zh-CN.md)。

```sh
python3 tools/ike-validation/download.py /Users/shield/Documents/ChatGPT/DJI-4G/ike-validation-local/sources
tools/ike-validation/build.sh /Users/shield/Documents/ChatGPT/DJI-4G/ike-validation-local
python3 -m unittest discover -s tools/ike-validation -p 'test_*.py'
```

下载只接受 inputs.json 中固定的大小和 SHA-256。首次取得 strongSwan、musl、GMP 归档时，已在独立 GNUPGHOME 核对官方发布签名和指纹，记录在本机 `logs/*.verify.txt`。构建镜像使用固定 Debian 工具包和固定 pkgconf 源码，编译容器关闭网络、没有 USB 挂载。命名卷 `qdc507-ike-build-v1` 保留电脑端缓存，Linux 文件系统用于保存有大小写区别的头文件。

`build-inside.sh` 对两个程序目标显式使用 libtool `-all-static`；`inspect_artifact.py` 拒绝动态加载器/动态段、错误机器类型或硬浮点 ABI。构建结果留在本机 `output/`，可执行文件内容由 SHA-256 标识。`strongswan.conf.example` 仅为未激活的配置参考，不含真实连接或凭据。

模拟实验包含通用 Linux 3.18.44 内核和精简 BusyBox；它们的输出名均带 `lab-only`，不属于客户端包，也不是模块固件。`lab-kernel.sh`、`lab-busybox.sh` 在上述 Linux 卷内执行；`make_lab_fixtures.py` 只生成两天有效的实验 RSA 凭据，拒绝覆盖已有目录；`make-rootfs.sh` 使用唯一 tag 防止覆盖旧实验目录。

实验凭据到期后应在新的空目录生成并构建新 tag 的根文件系统。改变固定输入或工具链时应使用新构建卷，避免沿用旧依赖缓存。

已有本轮实验材料时，电脑端复现示例：

```sh
python3 tools/ike-validation/run-lab.py /Users/shield/Documents/ChatGPT/DJI-4G/ike-validation-local --auth pubkey --threads 16 --attempt 6
python3 tools/ike-validation/run-lab.py /Users/shield/Documents/ChatGPT/DJI-4G/ike-validation-local --auth psk --threads 16 --attempt 2
```

实验只使用 loopback 上的私有 QEMU 链路，客体没有互联网或物理 USB。每次使用新的日志目录，失败保留日志；退出时只停止本次创建的虚拟机进程。8 工作线程组合未通过配置装载，不作为可用配置推荐。

`lab_mem.c` 在客体读取 status、完整 smaps 与子进程 rusage，采集器自身及宿主模拟器的 RSS 不计为客户端进程占用。`analyze_lab.py` 要求协议、收发、阶段和完整采样方法标记都满足条件，拒绝旧版部分 smaps 样本、缺少结束标记或 OOM/进程异常的记录。结果用于单连接资源评估，不提供高流量或实机运行保证。
