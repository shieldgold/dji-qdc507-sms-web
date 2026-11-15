# 独立 C 后端的 ARM 隔离验证

本目录只运行电脑上的一次性 ARM Linux 3.18.44 VM。基础验证复用
`tools/sms-validation` 的完整合成 PTY 夹具，共存验证复用
`tools/sms-validation/combined` 的原始夹具与原 16 线程 charon 配置。
历史夹具源码、内核、运行时内存保护均保持原样。

## 范围与保护

- 每台 VM 为 48 MiB，`maxcpus=1`；基础 VM 禁用网卡。
- 共存仅使用绑定 `127.0.0.1` 的 QEMU socket pair；没有宿主 NAT、WAN 或 USB。
- 内存准入为 8 MiB，绝对下限为 6 MiB，最大允许下降为 3 MiB。
- 每份串口日志上限为 1 MiB；进程结束后由 runner 回收 QEMU。
- 共存仍须通过原 30 秒窗口、10 组加密 ping/HTTP、短信恢复与子进程清理检查。
- 每次执行使用新目录；已有日志、二进制、证书与旧产物不会被覆盖。

这些验证使用合成 SIM、短信和实验 PKI，只证明电脑隔离环境中的行为。
它们不刷机、不加载物理模块、不建立自启动，不操作真实 SIM、ADB、USB、
RouterOS 或真实联网；没有实际短信、电话或 APNs 交付动作。

## 熵就绪预检

Linux 3.18 在启动早期可能已有输入池熵，但输出池仍未完成初始化；该内核默认
60 秒重播种间隔会使阻塞 `getrandom` 等待。预检将等待时间单独记录，只有真正
取得 32 字节 `getrandom(..., GRND_NONBLOCK)` 成功返回后，才启动原始夹具。
这不是服务启动延迟或业务请求延迟的性能测量。

预检最多等待 90 秒，保持上述 8/6/3 MiB 保护，不修改 sysctl 或内核设置。
它会读取并丢弃 `/dev/urandom` 的 32 字节以促使旧内核转移熵；丢弃的内容不会
作为安全随机数返回，也不能代替后续 `getrandom` 成功。日志仅包含返回值、
errno、耗时、尝试次数和内存值，不输出随机内容。

只接受精确的 `qdc.sms.lab=1`，或同时存在精确的
`qdc.coexist.lab=1` 与 `qdc.sms.combo=1`；冲突或重复标志会拒绝。
`build_guest.sh` 先调用历史 builder，再把原 init 原样保存在
`/fixtures/original-init`。独立 wrapper 挂载自己的 proc/dev、完成预检并释放挂载，
最后执行原 init，让原夹具完整执行自己的设置和保护。

## 单元测试

在仓库根目录执行，测试产物放在仓库外：

```sh
cc -std=c11 -Wall -Wextra -Werror -DQDC_ENTROPY_NO_MAIN \
  tools/native-c-validation/tests/test_entropy_wait.c \
  tools/native-c-validation/entropy_wait.c -o /tmp/qdc-entropy-test
/tmp/qdc-entropy-test
python3 -m unittest discover -s tools/native-c-validation/tests -p 'test_*.py'
```

C 测试覆盖实验标志、未就绪、真正成功、短读、失败关闭、90 秒超时和内存中止。
Python 测试覆盖证据顺序、日志失败标志、网络隔离与 Go GC 元数据要求。

## 基础 VM

先设置实验根目录 `QDC_LAB_BASE` 和待验证的 ARM 文件 `QDC_LAB_BINARY`，然后执行：

```sh
python3 tools/native-c-validation/run_vm.py "$QDC_LAB_BASE" \
  --binary "$QDC_LAB_BINARY" --label native-c-v3 --scenario ready --attempt 1 \
  --backend native-c
python3 tools/native-c-validation/run_vm.py "$QDC_LAB_BASE" \
  --binary "$QDC_LAB_BINARY" --label native-c-v3 --scenario pin --attempt 1 \
  --backend native-c
```

Go 公平基线传入其已核验哈希的既有 ARM 文件，并使用 `--backend go`。
两个后端使用同一预检二进制与完整夹具；Go 基础验证仍要求 GC 元数据，
且 Go heap 不会作为进程 RSS 使用。

## VPN 与短信共存

`QDC_INTEGRATION_BASE` 指向既有 newguard 实验根目录，`QDC_SYNTHETIC_PKI`
指向既有合成实验 PKI。runner 会检查其生成标记和证书剩余有效期；不输出私钥内容，也不会轮换已有证书。

```sh
python3 tools/native-c-validation/run_coexist.py "$QDC_INTEGRATION_BASE" \
  --binary "$QDC_LAB_BINARY" --label native-c-v3 --attempt 1 \
  --pki "$QDC_SYNTHETIC_PKI" --backend native-c
```

预检预算与原运行预算分别计时；不会为熵等待放宽原运行保护。
`result.json` 记录二进制、内核、夹具和预检源哈希，单独记录预检耗时，
并保留功能、内存、VPN 流量与恢复证据。
