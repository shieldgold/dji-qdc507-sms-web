# QDC507 TUN 驱动离线构建验证

2026-11-13 的结论：**可以编译候选 `tun.ko`，已通过下列离线静态检查；尚未验证实机加载、创建 TUN 或数据收发。**

本次仅通过现有 IOKit ADB 工具读取设备文件。没有上传产物、加载或卸载驱动、修改配置、重挂载文件系统、重启或刷机。构建容器不接入 USB，编译阶段关闭网络。

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
| 驱动大小 | 26,736 字节，约 26.1 KiB；这是磁盘文件大小，不是运行内存占用 |
| 驱动 SHA-256 | `94eff0f57e4f773ea08d47ed912580fdeb8d595d259e035c878d6ee746537c77` |
| 验证脚本测试 | 10 项单元测试通过，覆盖 vermagic、ELF ABI、结构大小、缺失符号及配置漂移的拒绝路径 |

机器可读结果见 [result.json](result.json)。未将设备启动分区、原厂驱动、原始配置或其他私有状态提交到 Git。

## 需要保留的限制

1. **源码并非已确认的 QDC507 原厂完整源码。** 候选使用公开 Quectel 源码的初始提交，原生版本为 3.18.44。将实机配置设为 `CONFIG_TUN=m` 后运行 `olddefconfig`，除这一项外，13 项 Quectel 私有配置因公开源码未包含对应定义而被移除；没有其他配置差异。这支持继续验证，但不足以证明所有厂商内部结构和补丁相同。
2. **编译器不同。** 原厂模块标记为 GNU GCC 4.9.3；镜像中下载的 Linaro 4.9-2016.02 工具链实际输出 GCC 4.9.4 20151028 prerelease。没有把它写成 4.9.3，也不把相同 vermagic 视为编译器完全相同。
3. **没有内核符号类型 CRC 可比对。** 实机 `CONFIG_MODVERSIONS=n`，因此从镜像重建的 `Module.symvers` 只用于核对导出名称，CRC 置零。它不是原厂原始构建文件。GPL 导出分类在重建中未保留；候选 TUN 驱动采用 GPL 许可证。
4. 静态检查只能核对接口存在、部分 ABI 元数据及配置。不能证明网络结构所有字段偏移、驱动加载、TUN 创建或稳定性。后续临时加载也属于设备改动，应在单独授权的阶段执行。
5. 本结果只针对 TUN 驱动构建。IKEv2/IPsec 客户端的编译、内存、路由、吞吐和断线恢复均未在本轮验证。

13 项源码缺口和全部配置差异已记录在 `result.json`，校验器会拒绝名单之外的配置变化。

## 固定输入

- [Quectel 源码提交](https://github.com/the-modem-distro/quectel_eg25_kernel/tree/82ed00908b3e8efc3ff0de27d2b5a7c0524ecd7f)：`82ed00908b3e8efc3ff0de27d2b5a7c0524ecd7f`。
- 源码归档 SHA-256：`baaea65a96b0151fcc96e83c34415bc10b8443b0907ecb0769f25b0d04d809ee`。
- [Linaro 工具链镜像](https://ftp.embeddedts.com/ftp/ts-arm-sbc/ts-7680-linux/cross-toolchains/gcc-linaro-4.9-2016.02-x86_64_arm-linux-gnueabi.tar.xz)。Linaro 原下载地址目前返回 HTML，实际使用 embeddedTS 的 HTTPS 镜像。
- 工具链归档 SHA-256：`fdef102e754b2aa9e7d0b2ad465216a41b421694e1ce0cc972621214f8ac4bd5`。这是本次记录的下载摘要，并非已核对上游签名的认证结论。
- 构建容器：`qdc507-tun-builder:local`，镜像 ID `sha256:28b74c392ccc4c28f76544779d19bd6ddcc60a863e46a9129a5e038ddec4e4fb`；基于项目现有语音构建镜像扩展 make、bc、kmod、patch、xz-utils。

## 本地复现

本次电脑上的工作目录为 `/Users/shield/Documents/ChatGPT/DJI-4G/tun-validation-local`。其中 `capture/` 保存只读采集文件，`sources/` 保存归档及解压源码，`build/` 保存中间文件，`output/` 保存驱动和日志，`inspection/` 保存校验结果。这些目录不属于提交内容。

校验器不访问设备。`build.sh` 只操作本地文件和 Docker，也不提供部署入口。

```sh
python3 -m unittest discover -s tools/tun-validation -p 'test_*.py'
sh -n tools/tun-validation/build.sh

# 已有本次 qdc507-voice-builder:local 镜像；从其他环境准备时先构建项目 voice/Dockerfile。
docker build --platform linux/amd64 -t qdc507-tun-builder:local tools/tun-validation

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

固定容器路径 `/work/sources` 后再次构建，产物 SHA-256 相同。驱动的诊断字符串包含源码路径，因此早期使用其他容器挂载路径的试编译产物摘要不同；以上大小和摘要以最终复现脚本产物为准。
