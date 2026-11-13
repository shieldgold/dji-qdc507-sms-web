# QDC507 IKEv2 临时实测工具实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在电脑上准备可审阅、可分阶段执行的实机 RAM 临时启动方案和工具，本轮不连接 USB、不上传、不执行模块命令。

**Architecture:** 复用已验证的 USB 控制器及 TUN 日志门控，不改控制器传输上限；离线打包器将大文件分片，并生成逐项请求卡。新增小型静态 ARM 监督器，在独立网络命名空间中启动无连接、无凭据的 charon，限制观察时长、监测内存和内核日志、退出时回收直接子进程。驱动加载/卸载与短信业务的暂停/恢复仍是单独验收步骤。

**Tech Stack:** Python 3、C99、已有 musl ARM 工具链、Go/IOKit 控制器、电脑端 ARM/Linux 3.18.44 QEMU。

---

## 影响范围与边界

只修改 tools/ike-validation 及索引文档，不修改短信网页、生产配置、固件或启动文件。沿用专用 worktree；产物、原始日志留在电脑的 ike-validation-local，代码和脱敏验证结果提交并推送 personal 远端。内存预算须包含新增测试工具；预备包不包含凭据或实验内核。

## Task 1：离线打包和阶段请求卡

- [x] 先写 Python 测试：分片重组、摘要不符、非法运行 ID、重复目录、符号链接、命令限长，以及无自动执行入口。
- [x] 运行 `python3 -m unittest discover -s tools/ike-validation/runtime -p 'test_*.py'`，确认新增行为未实现时失败。
- [x] 实现 prepare.py：固定已有产物摘要、每片至多 1 MiB、唯一目录、0600 请求卡；生成 RAM 检查、暂存/合并校验、TUN 加载、监督运行和清理卡。输出文件不自动发送到任何设备。
- [x] 重跑测试，检查生成命令的 shell 语法及控制器 3300 字节限制。

## Task 2：静态 ARM 监督器

- [x] 先写 C 测试：读取完整 proc 数据、超限拒绝、带括号进程名的 starttime 解析、PID 复用拒绝、内存阈值。
- [x] 编译测试并确认缺少实现时失败。
- [x] 实现 proc.c/proc.h 和 guard.c：GO 门控、只读 kmsg 连续序号、250 ms 内存监测、完整 smaps、无外网网络命名空间、VICI 只读查询、有限期停止及有界清理；提供按身份验证的短信 supervisor 延时恢复功能。
- [x] 容器内运行单元测试并构建静态 ARM 文件；检查 ELF/UAPI 和大小。

## Task 3：电脑端集成验证

- [x] 在不挂载 USB、无网络的容器构建实验 initramfs；实验内核只给 QEMU，明确排除出预备包。
- [x] ARM/Linux 3.18.44 VM 覆盖正常启动/退出、没有 GO、低内存拒绝、客户端提前退出和 supervisor 超时恢复；记录日志与验收 JSON。
- [x] 复跑已有 Python、C 内存解析及 USB 控制器测试，检查不改动原有传输行为。

## Task 4：交付和留档

- [x] 编写 TEMP_RUNTIME_TEST_PLAN.zh-CN.md 与记录模板，列清准入、日志、内存、停止、恢复、清理和验收边界。
- [x] 准备带摘要的电脑端预备包，核对不含凭据、实验固件或自动安装入口。
- [ ] 逐项审查实现与方案一致，完成所需测试后提交 Git，推送 personal 并回读远端提交。

本计划按用户已授权范围在当前会话串行执行；实机执行不包含在本次授权内。
