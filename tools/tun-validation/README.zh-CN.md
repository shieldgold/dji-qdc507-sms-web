# QDC507 TUN 驱动离线构建验证

2026-11-13 的结论：**已使用 GCC 4.9.3 正式版重新编译候选 `tun.ko`，并继续核对原厂关键 ABI，已覆盖项未发现明确冲突。13 项私有宏的恢复实验未改变候选 TUN；实机加载及完整厂商 ABI 仍未验证。**

GCC 4.9.3 构建阶段曾只读核对模块 misc 设备号、配置摘要和 TUN 加载状态；随后 QRAWDATA 与关键 ABI 分析完全使用已有本地采集文件，没有访问模块。没有上传产物、加载或卸载驱动、修改配置、重挂载文件系统、重启或刷机。构建容器不接入 USB，编译阶段关闭网络。

## 已验证的证据

| 项目 | 结果 |
| --- | --- |
| 实机内核 | Linux 3.18.44 / ARMv7 / PREEMPT |
| 配置来源 | 实机 `/proc/config.gz` |
| 配置 SHA-256 | `1fea60b794568c65cd6dfe436fbc2b9dabdf8c16267f01585f171174d5361620` |
| 启动镜像核对 | 启动分区镜像内嵌的 IKCONFIG 与运行内核配置逐字节相同 |
| 内核导出符号 | 从启动镜像解压后的 ARM Image 提取 6,144 项 |
| 提取交叉检查 | 原厂 `test-iosched.ko` 的 38 个必需外部符号全部可解析 |
| 候选驱动外部符号 | 101 个必需符号全部存在于原始内核导出表 |
| 模块 vermagic | 与原厂模块完全相同：`3.18.44 preempt mod_unload ARMv7 p2v8 `（末尾含空格） |
| ELF | ARM、ELF32、小端、EABI5，与原厂模块 flags 相同 |
| `struct module` | 候选和原厂模块的 `.gnu.linkonce.this_module` 均为 312 字节 |
| 编译器 | Ubuntu/Linaro GCC 4.9.3-13ubuntu2 正式版；与原厂主版本、次版本、补丁版本均一致 |
| 驱动大小 | 26,664 字节，约 26.0 KiB；这是磁盘文件大小，不是运行内存占用 |
| 驱动 SHA-256 | `2b44764dc5bee73faab5671bc5575cb08cb08c8cb68854fb8ef1a85b5ca8637d` |
| 验证脚本测试 | 38 项单元测试通过，覆盖编译器身份、私有宏、QRAWDATA 和有界 ABI 分析 |
| 私有配置对照 | 基线、13 项逐项恢复、全部恢复，共 15 组；预处理文件和目标文件分别完全一致 |
| TUN 依赖闭包 | 515 个实际依赖文件均未引用这 13 项私有宏 |
| QRAWDATA 二进制分析 | 596 条可达指令、19 个外部 API；未见网络/TUN 调用，共享文件操作表布局一致 |
| 关键 ABI 函数对照 | 检查 82 个函数，包含 68 个 TUN 导入函数；29 个函数扣除指定链接地址位后逐字一致 |
| 对象访问旁证 | 484 组共同参数访问模式；net_device 分配大小 1232、私有区对齐位置 1248 与原厂一致 |

机器可读构建结果见 [result.json](result.json)，私有配置分析见 [专项报告](PRIVATE_CONFIG_AUDIT.zh-CN.md) 和 [对照实验记录](private-config-result.json)。最新关键 ABI 检查见 [完整分析](TUN_ABI_AUDIT.zh-CN.md) 与 [机器可读记录](abi-result.json)。前一轮 GCC 4.9.4 记录见 [历史结果](https://github.com/shieldgold/dji-qdc507-sms-web/blob/691e151/tools/tun-validation/result.json)。未将设备启动分区、原厂驱动、原始配置或其他私有状态提交到 Git。

## 需要保留的限制

1. **源码并非已确认的 QDC507 原厂完整源码。** 候选使用公开 Quectel 源码的初始提交，原生版本为 3.18.44。将实机配置设为 `CONFIG_TUN=m` 后运行 `olddefconfig`，除这一项外，13 项 Quectel 私有配置因公开源码未包含对应定义而被移除；没有其他配置差异。这支持继续验证，但不足以证明所有厂商内部结构和补丁相同。
2. **GCC 版本差异已消除，但不声称原厂工具链构建完全相同。** 新模块 `.comment` 为 `GCC: (Ubuntu/Linaro 4.9.3-13ubuntu2) 4.9.3`，原厂为 `GCC: (GNU) 4.9.3`。`compiler_version_matches=true`，`compiler_identical=false`。发行版补丁和原厂编译器完整来源仍未知。汇编器和链接器使用 Debian Binutils 2.35.2，不能据此声称重现了原厂整套工具链。
3. **没有内核符号类型 CRC 可比对。** 实机 `CONFIG_MODVERSIONS=n`，因此从镜像重建的 `Module.symvers` 只用于核对导出名称，CRC 置零。它不是原厂原始构建文件。GPL 导出分类在重建中未保留；候选 TUN 驱动采用 GPL 许可证。
4. 后续已核对一批网络结构访问、回调槽位和分配边界，但不能证明所有字段、原厂类型总大小和差异函数的全部语义。驱动加载、TUN 创建及稳定性未验证。后续临时加载也属于设备改动，应在单独授权的阶段执行。
5. 本结果只针对 TUN 驱动构建。IKEv2/IPsec 客户端的编译、内存、路由、吞吐和断线恢复均未在本轮验证。

13 项源码缺口和全部配置差异已记录在 `result.json`，校验器会拒绝名单之外的配置变化。旁证 SDK 定位了其中 12 项的定义和使用处；随后从原厂内核定位了 `CONFIG_QRAWDATA` 的 procfs/MTD 实现，并核对共享文件操作表布局，见 [QRAWDATA ABI 专项分析](QRAWDATA_ABI_ANALYSIS.zh-CN.md)。原始源码仍未找到，不把静态分析扩展为整个厂商内核 ABI 的完整保证。

后续设备测试的日志、分阶段验收、恢复和清理步骤见 [受控临时加载方案](TEMP_LOAD_TEST_PLAN.zh-CN.md) 与 [运行记录模板](TEMP_LOAD_RUN_RECORD.template.md)。目前只准备方案，实时执行/采集通道和测试器仍需在电脑上准备并验收，未实际上传或加载。

## 固定输入

- [Quectel 源码提交](https://github.com/the-modem-distro/quectel_eg25_kernel/tree/82ed00908b3e8efc3ff0de27d2b5a7c0524ecd7f)：`82ed00908b3e8efc3ff0de27d2b5a7c0524ecd7f`。
- 源码归档 SHA-256：`baaea65a96b0151fcc96e83c34415bc10b8443b0907ecb0769f25b0d04d809ee`。
- GCC 4.9.3 编译器、头文件及依赖包均取自 Ubuntu 官方 HTTPS 归档。六个文件的 URL、大小和固定 SHA-256 在 [gcc493-inputs.json](gcc493-inputs.json)；下载器和 Docker 构建都校验摘要。摘要来源是本次从官方归档下载的文件，不声称已完成上游签名链认证。
- GCC 包版本：`4.9.3-13ubuntu2cross1`；本机通过 `-dumpversion`、`--version`、ARM 编译探针和产物 `.comment` 四处核对正式版身份。
- 使用 [Dockerfile.gcc493](Dockerfile.gcc493) 构建 `qdc507-tun-gcc493:local`，基于第一轮 `qdc507-tun-builder:local`。仅在容器 `/opt/gcc493` 解包旧编译器，不替换电脑或模块的系统编译器。
- 旁证 SDK：[Stark-Wan/EC20CEFAG 固定提交](https://github.com/Stark-Wan/EC20CEFAG/tree/dcd4977901517beb21973c520727b34fcc44a15a/ql-ol-sdk/ql-ol-kernel)，版本为 3.18.20；**只用于私有配置源码追踪，未替代 3.18.44 构建源码**。归档 SHA-256：`43c3961b380accf26d42cbdf4adfb1aacb851e085d8fba14ed1a373fc75854a6`。

## 本地复现

本次电脑上的工作目录为 `/Users/shield/Documents/ChatGPT/DJI-4G/tun-validation-local`。其中 `capture/` 保存只读采集文件，`sources/` 保存归档及解压源码，`build/` 保存中间文件，`output/` 保存驱动和日志，`inspection/` 保存校验结果。这些目录不属于提交内容。

校验器不访问设备。`build.sh` 只操作本地文件和 Docker，也不提供部署入口。

```sh
python3 -m pip install --target /Users/shield/Documents/ChatGPT/DJI-4G/tun-validation-local/qrawdata-analysis/python-deps -r tools/tun-validation/qrawdata-requirements.txt
PYTHONPATH=/Users/shield/Documents/ChatGPT/DJI-4G/tun-validation-local/qrawdata-analysis/python-deps python3 -m unittest discover -s tools/tun-validation -p 'test_*.py'
sh -n tools/tun-validation/build.sh

# 已有本次 qdc507-voice-builder:local 镜像；从其他环境准备时先构建项目 voice/Dockerfile。
docker build --platform linux/amd64 -t qdc507-tun-builder:local tools/tun-validation
python3 tools/tun-validation/download_gcc493.py /Users/shield/Documents/ChatGPT/DJI-4G/tun-validation-local/sources/gcc-4.9.3-debs
docker build --platform linux/amd64 -f tools/tun-validation/Dockerfile.gcc493 -t qdc507-tun-gcc493:local /Users/shield/Documents/ChatGPT/DJI-4G/tun-validation-local/sources/gcc-4.9.3-debs

tools/tun-validation/build.sh /Users/shield/Documents/ChatGPT/DJI-4G/tun-validation-local
```

所需只读采集输入：

| 本地文件 | 模块来源 |
| --- | --- |
| `capture/config.gz` | `/proc/config.gz` |
| `capture/vendor-test-iosched.ko` | `/usr/lib/modules/3.18.44/kernel/block/test-iosched.ko` |
| `capture/boot-partition.bin` | 当前设备 `/proc/mtd` 确认名为 `boot` 的 `/dev/mtd10` |

`mtd10` 是本次实机核对后的分区号，不能直接套用其他设备。采集通过 `base64` 输出到主机，由主机校验结束状态、解码及写入本地文件；模块端没有创建临时文件。

构建使用原配置开启 TUN 模块，`CC` 直接指定交叉编译器，跳过源码自带 Python 2 编译器包装脚本；`HOSTCFLAGS=-O2 -fcommon` 用于解决旧 dtc 在现代宿主 GCC 下的 `yylloc` 重复定义。没有修改 TUN 驱动源码或伪造内核版本。驱动最后仅移除调试信息，保留模块符号和元数据。

离线镜像符号扫描面向本次 ARM32、绝对地址格式、无 MODVERSIONS 的内核，使用长连续导出表及原厂模块依赖交叉验证。遇到其他镜像格式、配置、ABI 或未解析符号时，应停止并调查，不能通过忽略校验来宣布兼容。

驱动的诊断字符串包含源码路径，复现必须保留容器内固定路径 `/work/sources`。以上大小和摘要以 GCC 4.9.3 复现脚本产物为准。
