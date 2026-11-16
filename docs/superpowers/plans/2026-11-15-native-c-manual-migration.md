# C 版手动迁移与 60 分钟共存验证计划

> **For agentic workers:** 按已授权任务在当前独立工作树逐项执行，使用 verification-before-completion 与 test-driven-development；不需要再次申请执行整个计划的批准。

**Goal:** 保留原 Go 程序、启动脚本及电脑数据备份，安装独立 C 程序，当前开机手动切换并验证真实短信、VPN 重连和 60 分钟共存；不刷机、不设置自启。

**Architecture:** C 单文件安装到 `/data/smsweb-c/smsweb`，复用已验证的 inode/mount/进程身份检查，在本次开机通过文件 bind mount 运行。原程序及 start.sh 字节不改。新手动租约先通过 USR1 接管短守护的时钟恢复，保持原监管暂停，在独立 120 秒准备窗口内暂存 VPN；准备完成、TUN 加载后才记录基线并 ARM 启动 C。这样准备期间中止也能先清理 RAM 再恢复 Go。整体租约 5400 秒；60 分钟验收和 VPN 清理均成功后，才允许提交为本次开机持续手动运行。TERM、内存/身份/日志异常或未提交到期均停止本轮 VPN，普通卸载自有挂载并恢复原 Go；已安装 C 保留。

**Tech Stack:** 原 C 短信成品 8a458d、Linux 3.18.44、静态 ARMv7/musl 工具、既有 USB 控制器、既有 strongSwan 16 线程配置。原 30 秒/300 秒工具和原接口保持不变，新能力使用独立协议和入口。

**当前阶段：已完成实机迁移及验收。** C 当前开机手动运行，3600.100 秒/360组、三次重连、一次双 SPI rekey、真实短信双向验证均通过；原 Go/start/previous 保留，六份暂停数据和结束数据私有备份及一致性核验完成。默认准入首次因少8KiB触发恢复，迁移助手延后暂存后重新三次12372KiB通过，门槛和运行停止条件未降低。VPN/TUN/客户端及凭据已清理、网络恢复，已取得当前开机提交证明，无C自启。见[实机结果](../../../DEVICE_MIGRATION_2026-11-16.zh-CN.md)与[操作说明](../../../tools/sms-validation/manual/README.zh-CN.md)。

## 1. 私有备份和只读核验

- [x] 已新鲜确认 USB、boot/配置/taint、HTTP/SIM、原 SUP/SMS 身份、磁盘/内存、有效空闲证书。用户提供测试接收端，确认现场恢复能力并覆盖正式计时至11:25。
- [x] `tools/tun-validation/runtime/host/runner.go` 仅增加精确原程序和 start.sh 私有备份：`op=backup-program`，固定路径 `/data/smsweb/smsweb` 或 `/data/smsweb/start.sh`，60 秒，原程序最多 8 MiB、脚本最多 64 KiB，expected_bytes/SHA 必须一致。既有 `op=backup` 六份业务文件的路径/256 KiB/15秒限制不变，内容继续直接写 0600 私有文件，不写采集日志。
- [x] 在 `runner_test.go` 先增加合法原程序/脚本、错误路径、超限/字段、摘要不符、不覆盖及隐私测试，确认 RED，再实现后跑 `go test -race ./...`、`go vet ./...`。
- [x] 当前原 Go 6,226,072 B 的准确部署哈希须读回后备份，不拿同大小重建二进制冒充；六份数据暂停时备份，前后摘要不变才切换。备份不提交 Git。

## 2. 独立长租约

- [x] 新增 `tools/sms-validation/manual/manual_core.[ch]` 和 `test_core.c`。控制卡字段：`QDC_SMS_MANUAL_V1 / RUN / BOOT / SUP pid start / ORIGINAL dev ino bytes / SOURCE dev ino bytes / CLOCK pid start / SECONDS 5400`。仅此协议允许长租约，原 QDC_SMS_SWAP_V1 仍只能 300 秒。测试新协议接受、旧协议隔离、错误时限/身份/溢出/尾随字段拒绝、5400 秒到期、提交后状态边界，先 RED 后 GREEN。
- [x] 新增 `manual_swap.c`，以受限包装复用 `swap/swap.c` 的固定身份、fd 钉住、clock 证明和普通卸载实现，避免复制其修复。新 `manual-check/manual-hold` 使用独立协议和固定 `/data/smsweb-c`，短 clock guardian 仍为已验证 `resume-clock ... 300`，进入年龄不超过 180 秒。5400 秒以本 helper 的单调起点计，不续借短守护。
- [x] 新手动模式在挂载和时钟接管后保持原 SUP 暂停，全部 VPN 暂存完成、TUN 加载核验后，收到 ARM 才记录 MemAvailable 基线并启动 C。启动要求 8192 KiB，继续使用 6144 KiB 绝对下限和 3072 KiB 最大下降。记录受限在 64 KiB；错误立即回滚，不读 SMS/PIN/认证内容。
- [x] 回滚先通过同 RUN 的 `.state` 核验并终止 VPN guard，确认没有 charon/swanctl/运行目录/接口，再释放本轮客户端 RAM 文件；随后停止 C、普通卸载、核对原 inode、验证 clock 证明，MemAvailable 至少 10240 KiB 才 CONT 原 SUP。此前恢复 Go 实测占用约 3.1 MiB，因此新手动模式保留 10 MiB 恢复空间，运行时 6 MiB/3 MiB 停止线不变。未知身份、挂载或资源保留并报告 RECOVERY_REQUIRED；不 force/lazy/killall/drop_caches。
- [x] 新手动模式的回滚保留 C 安装文件，原短租约仍删除自己的临时副本。测试阶段到期或 TERM 默认回退；提交文件必须同时有本轮 SOAK 成功证明、VPN 已清理、C 活身份和完整时钟证明，才能保持当前开机运行。重启自然恢复原入口，不增加启动项。

## 3. 连续 VPN 共存与重连

- [x] 新增 `soak_guard.c`，复用原 `runtime/guard.c` 的进程启动/停止、凭据目录、网络范围、日志解析和 VICI 操作；不修改原 guard 源文件或放宽旧命令期限。新 CLI 只接受 `soak ROOT RUN_SOAK 3600 TARGET VIP` 及新鲜身份控制卡。
- [x] 基线读取自手动租约在 C 启动前的记录，不能重设成 C 启动后的值。每次检查内存与短信/SUP 身份，完整 smaps 每 10 秒，HTTP 与加密 ICMP 每 10 秒，APP/kmsg 与手动日志合计预算不超过 1 MiB。
- [x] 实现 60/120/180 秒三次受控 IKE 终止、SA/VIP/目标路由清除核验、重新发起及加密收发；仅操作本测试连接。随后显式 CHILD rekey，确认双 SPI 变化。生产要求至少 3600 秒/300组；电脑实验倍率30，真实完成120.087秒/360组及上述重连/rekey，实机本轮完成3600.100秒、360组及上述重连/rekey。
- [x] 实现后台独立日志和小型原子状态文件，主机单张 USB 卡继续最多90秒；可每30秒取状态，结束取完整有限日志。独立 helper 的期限与停止机制不依赖主机持续采样。实际 USB/kmsg 三轮稳定性已重验，一小时原生日志完整取回。
- [x] 十项手动 ARM 和五项网络 ARM 场景通过，含真实重连/rekey、严格网络/凭据清理、错误基线/未GO/HTTP失败/中断、缺证明退出回退、低恢复内存、真实120秒期限、提交及回退、未知挂载保留。生产二进制无实验加速或内存注入；旧六项swap完整回归，原guard源码及其构建证据保留。实际模块日志通道和一小时期限已另行实测。

## 4. 实机执行和留档

- [x] 电脑准备、单测和 ARM 验证通过后提交并推送工具；精确哈希封存后才上传。
- [x] 现场条件及认证具备后重核设备，三轮各 30 秒采集，短 clock READY→暂停原服务→三次 12288 KiB 准入（实际12372）→迁移助手暂存→六份数据私有备份→流式复制与独立安装→新鲜身份绑定→手动租约接管时钟并保持暂停→VPN 暂存及 TUN 加载→写 ARM 文件，由 helper 记录基线并继续原 SUP 启动 C→确认实际 C inode。
- [x] 通过正常网页登录使用原认证；不重置密码、不提取会话令牌。只向用户指定可控号码或模块自身已识别号码发送一条带唯一编号的普通测试短信；正文/号码不写公共日志。收到对应测试短信并核对提交状态才记收发通过，不重试未知提交状态。
- [x] 运行 60 分钟实际共存，完成三次 VPN 重连及显式 rekey。定期报告实际分钟数、内存/峰值和计数组数。异常按既定回退停止，不用重启掩盖失败。
- [x] 验收成功后停止 VPN、普通卸载 TUN、删除本轮凭据/客户端暂存，只保留手动 C 租约运行所需文件及 clock 证明，再提交本次开机 C 手动运行。原 Go/数据备份及手工回退卡保留；没有 C 自启。
- [x] 独立读回 C 进程、原文件及启动脚本哈希、业务数据一致性、网络/DNS/防火墙、无测试 VPN 残留；记录安装路径与当前运行状态。测试未完成则不宣称完成迁移验收。
- [x] 报告与脱敏 JSON 已以 `feb92fb` 提交并推送 personal，远端 HEAD 精确匹配。十四项 Python、host race/vet、来源摘要、实机结果/数据/网络与隐私检查通过。私有备份、手机号、短信正文、认证和私钥不进 Git。
