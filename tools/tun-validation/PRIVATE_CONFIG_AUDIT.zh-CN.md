# 13 项 Quectel 私有配置对 TUN 的影响

2026-11-13：**在本次公开 3.18.44 候选源码范围内，这 13 项配置的缺失未改变 TUN 的预处理结果或目标文件。** 逐项恢复及合并恢复实验均通过。公开 EC20 SDK 中还定位了其中 12 项的定义和引用，没有发现它们改变网络公共头文件或 TUN 数据结构。

后续已从原厂内核定位 `CONFIG_QRAWDATA` 的 procfs/MTD 存储实现，并核对共享文件操作表布局，见 [QRAWDATA ABI 专项分析](QRAWDATA_ABI_ANALYSIS.zh-CN.md)。原始源文件仍未找到，也不能把这些局部静态证据扩展为“已证明 QDC507 全部私有补丁均兼容”。本轮没有上传或加载驱动，也没有修改模块配置、重启或刷机。

## 分析方法与边界

1. 构建源码仍为 Quectel 3.18.44 提交 `82ed00908b3e8efc3ff0de27d2b5a7c0524ecd7f`，不改变 TUN 源码或伪造版本号。
2. 采用 GCC 4.9.3 正式版，使用 Kbuild 实际生成的编译命令。在独立输出目录重放编译，不改动原 TUN 目标文件或模块文件。
3. 对 13 项配置逐项写入实验用强制包含头文件，再同时写入全部配置。`y` 对应 `CONFIG_X=1`，`m` 对应 `CONFIG_X_MODULE=1`，`n` 保持两个宏均未定义。没有把禁用选项定义成数值 0。
4. 以相同编译器分别生成预处理文件和 `tun.o`，比较完整字节 SHA-256。基线 + 13 项逐项恢复 + 全部恢复，共 **15 组**，两种产物分别完全一致。
5. 从实际 `.tun.o.cmd` 读取依赖闭包，核对 **515 个依赖文件**，对这 13 项私有配置的直接引用为 **0**。
6. 使用另一份公开 EC20 SDK（3.18.20）作为私有功能的源码旁证。其版本更旧，未被替换进 3.18.44 构建。扫描该 SDK 的 C、头文件、汇编、Kconfig、Kbuild 和 Makefile 后，12 项有定义或引用，`QRAWDATA` 没有对应实现；该组引用位于 `net/` 和公共 `include/` 的数量为 **0**。

实验头文件只恢复宏语义，**不等于恢复缺失的厂商代码**。实验能回答“这些宏是否改变当前候选 TUN 编译单元”，不能独立回答“未公开的内核补丁是否改变了所有共享结构”。

基线 `tun.o` SHA-256 为 `a8d8e428ee171dcd449614ce28b157ea6fcd904d16df4a2602e21a95e1366abf`，与 Kbuild 实际对象完全相同。所有实验组的目标文件均为该摘要；预处理文件均为 `a24abf207d197a37ffd08e883492da839090c5169ac2d53c40a19b8e43f07495`。

## 逐项结果

表中路径属于固定旁证 SDK，完整引用及行号保存在 [private-config-result.json](private-config-result.json)。

| 配置 | 实机值 | 已核对的定义或作用位置 | 对当前 TUN 的判断 |
| --- | --- | --- | --- |
| `CONFIG_QUECTEL_DRIVER` | y | `drivers/Makefile`、`drivers/quectel-drivers/Kconfig`：Quectel 驱动分组入口 | 不改变当前 TUN；没有网络头文件引用 |
| `CONFIG_QL_LPM` | m | `drivers/quectel-drivers/ql_lpm/`：GPIO 休眠/唤醒、独立 netlink 协议号 28 | 使用标准 `sock`/`sk_buff`，未修改其定义；宏恢复后 TUN 不变 |
| `CONFIG_QSTART` | y | `drivers/quectel-drivers/qstart/`：启动模式和分区恢复接口，misc 次设备号动态分配 | 当前 TUN 无引用；不是 TUN 固定次设备号 200 的定义者 |
| `CONFIG_QTZONE` | n | `drivers/quectel-drivers/qtzone/`、Kconfig：TrustZone 支持 | 实机禁用；恢复禁用宏语义不改变 TUN |
| `CONFIG_QSMD` | n | `drivers/quectel-drivers/qsmd/`、Kconfig：legacy 驱动 | 实机禁用；恢复禁用宏语义不改变 TUN |
| `CONFIG_QSMEM` | n | `drivers/quectel-drivers/qsmem/`、Kconfig：legacy 驱动 | 实机禁用；恢复禁用宏语义不改变 TUN |
| `CONFIG_QRAWDATA` | y | 已从原厂内核恢复 `/proc/quec_rawdata` 私有 ioctl 与 MTD 存储路径；原始源文件仍未找到 | 宏恢复不改变候选；二进制未见网络/TUN 耦合，共享文件操作表布局一致，详见专项分析 |
| `CONFIG_QUECTEL_AUDIO_DRIVER` | y | `qconfig/Kconfig`、`sound/soc/msm/`：音频平台、codec、AFE/PCM 控制 | 已见源码只影响音频；未发现 TUN 或网络结构条件分支 |
| `CONFIG_QUECTEL_PCM16K_SUPPORT` | y | `sound/soc/msm/`、`sound/soc/codecs/alc5616.c`：16k PCM 相关参数和接口 | 宏恢复后 TUN 不变 |
| `CONFIG_QUECTEL_POWER_DRIVER` | y | `drivers/power/reset/msm-poweroff.c`：电源控制属性 | 宏恢复后 TUN 不变；电源行为另属运行时环境 |
| `CONFIG_QUECTEL_POWER_MANAGER` | y | `drivers/misc/quectel_power_manager.c`：平台休眠驱动 | 宏恢复后 TUN 不变；休眠对隧道稳定性的影响未测试 |
| `CONFIG_QUECTEL_MODEM_RESTART_LEVEL` | y | `drivers/soc/qcom/subsystem_restart.c`：基带子系统重启行为 | 宏恢复后 TUN 不变；蜂窝重启后的 VPN 恢复仍需运行时验收 |
| `CONFIG_QUECTEL_MODEM_BOOT_TIME` | y | `init/main.c`：启动参数和基带启动相关逻辑 | 宏恢复后 TUN 不变 |

只编译 TUN 扩展模块，不会把运行内核中已经存在的这些功能删除。这里的“配置缺失”指公开构建源码的 Kconfig 未包含定义，不是对模块现有固件执行了关闭配置的操作。

## 只读实机补充核对

本轮只读检查了 `/proc/misc`、`/proc/config.gz` 的 SHA-256 和已加载模块列表：

- misc 次设备号 **200 未被占用**，未见当前设备注册表中的直接号码冲突。
- 配置摘要仍为 `1fea60b794568c65cd6dfe436fbc2b9dabdf8c16267f01585f171174d5361620`，与前一轮采集一致。
- 未发现已加载的 `tun` 模块。

这不是驱动加载测试，不能替代 `misc_register`、TUN 创建或报文收发的实际验收。

## 固定旁证来源

- [SDK 根目录，固定提交 dcd4977](https://github.com/Stark-Wan/EC20CEFAG/tree/dcd4977901517beb21973c520727b34fcc44a15a/ql-ol-sdk/ql-ol-kernel)
- [Quectel 全局配置](https://github.com/Stark-Wan/EC20CEFAG/blob/dcd4977901517beb21973c520727b34fcc44a15a/ql-ol-sdk/ql-ol-kernel/qconfig/Kconfig)
- [Quectel 可选驱动配置](https://github.com/Stark-Wan/EC20CEFAG/blob/dcd4977901517beb21973c520727b34fcc44a15a/ql-ol-sdk/ql-ol-kernel/drivers/quectel-drivers/Kconfig)
- [低功耗驱动](https://github.com/Stark-Wan/EC20CEFAG/blob/dcd4977901517beb21973c520727b34fcc44a15a/ql-ol-sdk/ql-ol-kernel/drivers/quectel-drivers/ql_lpm/ql_lpm.c)
- [启动驱动](https://github.com/Stark-Wan/EC20CEFAG/blob/dcd4977901517beb21973c520727b34fcc44a15a/ql-ol-sdk/ql-ol-kernel/drivers/quectel-drivers/qstart/qstart.c)

## 本机复现私有配置对照

先按主报告完成 GCC 4.9.3 构建，再准备固定 SDK 的内核目录到本机 `sources/quectel-ec20-private`。本次 SDK 归档 SHA-256 为 `43c3961b380accf26d42cbdf4adfb1aacb851e085d8fba14ed1a373fc75854a6`。

```sh
docker run --rm --network none --platform linux/amd64 \
  -v /Users/shield/Documents/ChatGPT/DJI-4G/tun-validation-local:/work \
  -v /Users/shield/Documents/ChatGPT/DJI-4G/tun-validation-local/capture:/work/capture:ro \
  -v /Users/shield/Documents/ChatGPT/DJI-4G/tun-validation-local/sources:/work/sources:ro \
  -v "$PWD/tools/tun-validation:/audit:ro" \
  qdc507-tun-gcc493:local python3 /audit/private_audit.py
```

结果写入电脑上的 `private-audit/`，不访问设备。
