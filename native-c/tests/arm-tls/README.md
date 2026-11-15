# ARM 回环 TLS / HTTP/2 夹具

这是一次性电脑虚拟机测试，不能安装到设备。客户端直接编译已有
`tests/test_tls.c` 和生产 `tls.c`、`crypto.c`、`clock.c`、`common.c`；
仅启用已有 `QDC_TESTING` 的 127/8 端点钩子，不跳过主机名、证书、ALPN、
TLS 版本或 HTTP/2 结果检查。

从仓库根目录运行：

```sh
python3 -m unittest discover -s native-c/tests/arm-tls -p test_runner.py
python3 native-c/tests/arm-tls/run.py \
  /Users/shield/Documents/ChatGPT/DJI-4G/ike-validation-local/native-c-build \
  --label unique-attempt-name
```

构建固定使用 `--network none` 的 `qdc507-ike-builder:local` 和卷内
`/work/native-c-deps-v1/prefix`，不下载依赖。QEMU 仅有 48 MiB 内存、
一个 CPU、`-nic none`、回环接口和 virtio-rng。启动要求严格匹配
`qdc.sms.lab=1 qdc.tls.lab=1`、Linux 3.18.44、ARM 架构；只在该客体内
设置当前主机时间以正常验证合成证书的有效期。

原样复用 `tools/native-c-validation/entropy_wait.c` 的真实 getrandom
就绪预检，90 秒预检与 65 秒运行期分开计时。准入 8192 KiB、绝对停止线
6144 KiB、最大下降 3072 KiB 均未放宽。串口和 QEMU 错误日志各限制在
1 MiB，客体进程与 socket 都有绝对时限，退出后验证子进程、监听端口和
临时日志已清理。

四个连接分别验证强制 TLS 1.2、强制 TLS 1.3 的 HTTP/2 POST，以及错误
主机名、错误 CA 在 TLS 1.2 下拒绝。成功必须同时具备服务端实际协商的版本与 ALPN、
完整六个请求字段、`{}` 请求体、请求 END_STREAM、200 + Date + JSON +
响应 END_STREAM，以及生产客户端收到完整结果。服务端读取 HTTP/2 确认帧
直到客户端关闭，避免立即关闭带来的真实 TCP 重置。负向用例必须同时具备
客户端验证拒绝与真实字节流中的 TLS 1.2 证书类 fatal alert（描述值 42、46 或 48），
且没有应用请求。服务端可能在读取 alert 之前先遇到写失败，所以必须核验收到的
具体 alert 记录，普通网络错误不能通过。
同库 TLS 1.3 服务端在收到客户端证书拒绝时可返回无效 MAC，夹具不把该结果
算成证书拒绝证明；TLS 1.3 的负向互通另由已有主机 Node 夹具覆盖。仅配置 TLS 版本
或仅握手成功不能通过。

RSS/PSS 以约 20 ms 间隔读取 `/proc`，报告各进程最大观测值；读取开销
和调度会使实际间隔略长，不能当作连续采样的绝对峰值，也不能替代设备上
真实 APNs、USB、SMS/VPN 并存或生产服务内存验收。所有证书、凭据和请求
均为合成数据，私钥只保留在权限受限的私有实验目录，日志不输出密钥或随机值。

结果目录独占 `native-c-build/arm-tls/<label>`，保留 ELF 检查、实际依赖与
源码 SHA-256、构建及 QEMU 命令、原始串口、结果 JSON 和一次性根文件系统。
失败尝试保留原始证据，不覆盖成功结果。
