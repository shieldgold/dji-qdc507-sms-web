# QDC507 短信与 VPN 共存工具 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox syntax for tracking.

**Goal:** 在电脑上准备可审阅、可恢复的 30 秒短信网页与 IKEv2/IPsec 共存测试工具；本轮不连接、上传或运行模块。

**Architecture:** 复用已验证的静态 ARM 客户端、TUN 和 USB 日志控制器。原生守护程序先连接 VPN，释放已退出的 swanctl 文件，再由独立恢复守护恢复原时钟和原短信监管进程；同时监测短信身份、HTTP、加密 ping、内核日志和内存。准备包默认缺少启动卡，只有后续绑定新鲜的启动 ID 和进程身份才生成。默认准入 12 MiB，停止线仍为 6 MiB 或相对基线下降超过 3 MiB。

**Tech Stack:** C99 / ARM soft-float musl、Python unittest、Linux QEMU ARM 隔离实验环境、现有 Go USB 日志控制器。

## 影响与边界

- 只修改隔离 worktree 内的验证工具和文档；不改短信业务程序、监管脚本、模块文件系统、服务器规则、固件或启动项。
- 保留现有 idle/connect 接口及既有实机验证二进制；新构建和实验记录输出到电脑上的独立 coexist-build 目录。
- 原始证书、密钥、短信和实机日志不进入 Git；虚拟机使用一次性实验身份，网络只在两个本地 ARM guest 之间。
- 第一次共存只测试固定网页 GET / 和目标 /32 的 ICMP；不发送短信、不测试电话、不更改默认路由和 DNS。

## Task 1: 原生有界 HTTP 探针

- [x] 写行为测试并对旧接口运行，记录预期失败。
- [x] 新增 http_probe.c/h，固定端口 8787 和路径 /，只允许本机 RFC1918/loopback IPv4；2 秒单调时钟期限、响应大小限制，不输出正文。
- [x] 验证正常响应、拒绝/错误状态、截断、超时和地址限制。

## Task 2: 恢复交接与共存守护

- [x] 写控制文件语法/身份拒绝测试，再实现 coexist.c/h 和 guard.c 的小范围接入。
- [x] 绑定启动 ID、原短信/监管/恢复守护 PID 与 starttime；拒绝陈旧 PID、非原监管、非本机 HTTP 地址、过期恢复期限和未知控制字段。
- [x] VPN 成功后释放 swanctl，精确通知恢复守护；核实先 CLOCK_RESTORED 后 RESUME_SENT、原监管恢复且只出现一个新短信子进程。
- [x] 保持 30 秒共存，完成 10 次加密 ping 与 HTTP，持续采集日志、内存、RSS/PSS；失败只停止本轮 VPN。
- [x] 保持独立 80+3 秒截止、无 GO 不动作、严格归属清理和原接口回归。

## Task 3: 电脑准备包与新鲜身份绑定

- [x] 写准备包行为测试；增加 prepare_coexist.py，检查内容哈希和相应 ARM 实验验收记录。
- [x] 默认生成未绑定包，无可执行的共存启动卡；准入固定 12 MiB。
- [x] 独立 bind 子命令只接受 60 秒内的公开身份快照，生成控制文件和单步请求卡，不进行 USB 或网络操作。
- [x] 清理白名单包含控制文件、分片、状态和日志；遇到未知文件保留并报告。

## Task 4: 隔离实验和回归

- [x] build-coexist.sh 输出独立二进制和源码哈希，不覆盖旧实机产物。
- [x] 运行 CPU 单元测试、原守护实验回归；用本地两台 ARM guest 验证真实 IKEv2 和模拟短信 HTTP 的 30 秒共存。
- [x] 实验覆盖身份不符、无 GO、短信 HTTP 错误、短信重启、内存停止线、信号中止与恢复清理。
- [x] 生成只对应本次二进制哈希的验收记录，再准备受保护的未绑定本地包。

## Task 5: 操作方案、复核与留档

- [x] 编写 COEXIST_TEST_PLAN.zh-CN.md，列明日志、内存、准入、恢复、清理、验收、实机授权前置条件。
- [x] 审查代码和文档的范围、进程身份、日志敏感信息和失败路径，修复所有实质问题后重跑相关验证。
- [x] 检查 Git 无密钥/原始日志/二进制，提交并推送 personal 分支；回读远端 SHA。
- [x] 交付工具、方案和电脑实验结论，明确模块共存仍未验收。

## 验证记录

最终守护 SHA-256 `be9e3c4f79042e42bd6a85696f38fc82cae936176d83ad65dae78aca253c0844`；CPU 44 项通过、1 项 binfmt 环境跳过；独立 C 解析自检通过；ARM 20 项回归和 7 项共存实验通过。正常共存 30.009 秒。回执另核对两份 initrd 内嵌守护哈希。完整公开结果见 `tools/ike-validation/runtime/coexist-preparation-result-2026-11-14.json`。模块本轮未连接或执行。
