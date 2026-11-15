# QDC507 根挂载父编号兼容修复

CN20261115H01 已在真实模块通过原生内存准入：三次均为 12384 KiB，间隔 5 秒，门槛保持 12288 KiB。三轮各 30 秒 USB 日志、六份业务状态私有备份及读取前后摘要核验通过。C 候选仅复制到本轮独立目录并核验 SHA；切换前的 `check` 返回 `preexisting_or_invalid_mount`，未建立 bind mount、未启动 C、未加载 TUN、未启动 VPN。不能把本轮当成 C 共存成功。

实机 `/proc/self/mountinfo` 第一行为：

```
10 0 0:11 / / ro,relatime - ubifs ubi0:rootfs ro,bulk_read,chk_data_crc
```

旧解析器对每条记录要求 parent 非零，因此在无关根挂载处失败。修复只允许挂载点恰为 `/` 的记录具有 parent=0；目标文件和其他挂载点仍拒绝 parent=0。mount ID 非零、整数边界、完整换行、分隔符、目标路径精确匹配、既有目标挂载拒绝、mount ID/inode 保持及普通卸载逻辑不变。

实机原始行作为回归先复现断言失败，修复后通过；新增零 mount ID、目标/非根父编号零和父编号溢出拒绝用例。重新构建的 Linux 复制、空间、低内存测试 8 项通过，验证器 3 项通过，6 项真实 ARM guest 挂载/恢复场景全部通过，包括运行中新程序恢复、时钟证明拒绝、符号链接拒绝、时限拒绝及未知覆盖保留。新 helper 仍为 74844 B，SHA-256 `ec452f757488dd7a25863a5b536cddea2f5bd92a0f25c050e6d0c0ba1a824a3c`。C 程序及 VPN guard 未改动。

H01 已精确终止本轮 clock guardian，确认 CLOCK_RESTORED 在 RESUME_SENT 前；原监管 428/826 恢复，单一原 Go 程序 1286/10113353，原执行文件仍 dev16/inode898。原程序、启动脚本和直接回退文件 SHA 均不变；认证、短信、推送和通话四份业务数据摘要不变，流量状态随原服务运行更新，未用旧备份覆盖。HTTP 200、SIM READY；boot、内核配置、taint 和规范化防火墙一致。恢复后独立采集 30 秒通过，候选目录、RAM 工具/凭据及本轮目录全部清理，没有 `/dev/net`、TUN 或 VPN 残留。

恢复初期尚保留 VPN 暂存文件时一次 MemAvailable 为 6412 KiB，仍高于 6144 KiB 下限；释放本轮客户端文件后回到 8680 KiB。后续若在客户端尚未启动时拒绝，优先释放已验证未使用的客户端暂存文件，再触发原服务恢复；保留 clock guardian、目录及日志直至恢复证明完成。所有内存门槛和时限维持不变。

原始日志、六份私有备份、构建及 ARM 证据位于电脑私有 `device-live-CN20261115H01` 和 `swap-root-parent-20261115` 目录，不提交凭据或业务内容。修复后的下一轮使用新运行编号、重新采集身份和准入。
