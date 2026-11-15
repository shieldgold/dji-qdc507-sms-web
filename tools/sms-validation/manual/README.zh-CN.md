# C 版可回滚手动迁移工具

这套工具把 C 程序安装到独立的 `/data/smsweb-c/smsweb`，只在当前开机通过文件 bind mount 切换运行。原 `/data/smsweb/smsweb`、`start.sh` 和 `previous/smsweb` 不覆盖；不刷机、不添加启动项。重启自然解除挂载，原入口仍运行 Go。程序和业务数据的电脑备份须先验证完成。

产品二进制继续使用已通过短时实机验证的 C 成品。本目录只提供迁移、监测及验收工具。**电脑 ARM 模拟测试不能替代实机短信收发或连续 60 分钟验收。**

## 电脑构建与验证

需要已有 `qdc507-ike-builder:local`、`qdc507-ike-build-v1` Docker 工具链卷、隔离实验用证书、ARM 内核和此前已验收的短 clock guardian。`LOCAL` 是电脑私有 `ike-validation-local` 目录，`OUTPUT` 必须为尚不存在的绝对路径；下列命令不接触 USB 或真实 VPN 服务器。

```sh
python3 -m unittest discover -s tools/sms-validation/manual -p 'test_*.py' -v
sh tools/sms-validation/manual/build.sh "$LOCAL" "$OUTPUT" "$SHORT_GUARD"
python3 tools/sms-validation/manual/run_vm.py "$LOCAL" "$OUTPUT" --only all
```

输出分为生产 `qdc-sms-manual-arm`、`qdc-ike-soak-arm`，以及只能在电脑使用的 `lab-only-*`。`build.json` 封存源码、静态 ARM ELF、短 guardian 和 initrd 摘要；运行器拒绝摘要不符或覆盖既有场景证据。只有全部场景通过才产生 `acceptance.json`。

- 手动迁移 ARM 场景：提前终止、C 运行后回退、缺失验收证明、提交后仍可回退、符号链接拒绝、先停止本轮 VPN、未知覆盖挂载保留、VPN 无成功证明退出、真实 120 秒准备超时与部分上传清理、恢复空间不足时保持原监管暂停。
- 双 ARM 客户端/服务器场景：正常共存流程、篡改启动前内存基线、未 GO、短信 HTTP 失败、显式中断。验证真实 IKE 重连、双向 ESP SPI 更新和网络/凭据清理。
- 实验版仅将观察窗口和探测排期加速 30 倍，真实 I/O、CLI 超时及准备回退期限不加速。实验版只写 `soak.test-result`，真实经过约 120 秒；生产 helper 不接受它为 3600 秒证明。生产版时钟倍率固定为 1，不含内存注入。
- 隔离 guest 使用三份一次性证书的共同有效时间，构建清单固定该实验时间，仍执行正常证书有效期验证；不使用电脑今天的日期判断历史实验室证书，不改实机时钟。实验网卡完成 IPv6 地址检测后才记录严格网络前态，结束仍逐字比较全部地址及路由。

原 `swap/build.sh`、`swap/run_vm.py` 的短租约也要回归。新协议没有增加原 `QDC_SMS_SWAP_V1` 的 300 秒权限。

## 实机准入与备份

本轮必须新鲜核验现场仍能切断全部实际供电。此前的异常重启授权沿用。USB、boot ID、内核/配置摘要/taint、HTTP/SIM、原监管及短信 PID/start/exe/argv、原文件 SHA、磁盘和网络均重新采集；测试证书必须有效且没有其他连接占用。

通过同一 USB 控制器完成原程序和启动脚本的私有流式备份：`backup-program` 只接受固定 `/data/smsweb/smsweb`（最多 8 MiB）或 `/data/smsweb/start.sh`（最多 64 KiB），固定 60 秒，必须给出预期字节数及 SHA。备份内容直接写电脑 0600 文件，不进入控制器输出尾部或 Git。

三轮各 30 秒日志采集通过后，先启动已验证短 guardian `resume-clock SUP SUP_START 300`，取得 READY，再精确暂停原 SUP、终止其 Go 子进程。原生准入要求三次相隔 5 秒的 MemAvailable 至少 12288 KiB；不自动沿用历史 11.5 MiB 例外，不清缓存，不停额外服务腾内存。

暂停后，使用既有六 JSON `backup` 接口做一致性备份及前后 SHA 核对：认证、短信、推送状态、通话尝试、时钟、流量。它仍限制为六个固定文件、256 KiB、15 秒。备份和号码/正文/认证/私钥只留在电脑私有目录。

先把 C 成品流式复制到本轮唯一临时目录，校验字节数及 SHA，确认独立安装路径不存在，再将自己的目录原子移到 `/data/smsweb-c`（目录 0700、文件 0500）。不在 RAM 解包完整程序，不覆盖任何已有安装。保留至少 1 MiB 存储余量。

## 当前开机切换顺序

把生产迁移 helper 放到 `/run/qdc-sms-RUN/qdc-sms-swap`，依实际 PID/start、设备 inode/字节数生成 0600 `lease.control`：

```text
QDC_SMS_MANUAL_V1
RUN RUN
BOOT 当前boot-id
SUP 原监管pid 原监管start
ORIGINAL 原程序dev 原程序ino 原程序bytes
SOURCE 已安装C的dev 已安装C的ino 已安装C的bytes
CLOCK 短guardian-pid 短guardian-start
SECONDS 5400
```

1. 执行 `manual-check RUN`，确认同挂载命名空间、原监管已暂停、原 Go 子进程消失、原文件和已安装 C 身份精确匹配、无既有覆盖挂载，且短 guardian 年龄不超过 180 秒。
2. 后台执行 `manual-hold RUN`。helper 钉住原文件 fd，建立自己的 bind mount，使用 USR1 取得短 guardian 的时钟恢复证明；短 guardian 退出，原 SUP 继续暂停。收到 `MANUAL_PREP_READY` 才进入下一步。记下 `MANUAL_ORIGINAL_FD`，挂载期间可由该 fd 只读核对原 Go 摘要。
3. **120 秒准备窗口内**暂存全部 VPN 文件、证书和生产长测 guard，核对哈希，加载已验证 TUN 并核验；失败立即精确 TERM 本轮 helper。此时恢复已由手动 helper 接管，不能再让短 guardian CONT 原服务。
4. 全部准备完成，原短信仍暂停时，原子写入 0600 `/run/qdc-sms-RUN/arm.manual`：

```text
QDC_SMS_MANUAL_ARM_V1
RUN RUN
```

5. helper 此时才采集真正运行基线，要求至少 8192 KiB，记录 `MANUAL_BASELINE ... before_c_start=1`，随后 CONT 原 SUP 启动 C，输出 `MANUAL_ARMED`。不能把基线改到 C 启动之后。确认单一实际 C 子进程的 exe 指向已安装 C 的 dev/ino/bytes，登录会话及业务数据正常。

## VPN 重连与 60 分钟共存

将以下 0400 `soak.control` 放在 `/run/qdc-tun-RUN`，基线必须等于上述 native 日志记录；新 guard 还会检查 PREP→BASELINE→ARMED 顺序及仍存活的迁移 helper 身份。

```text
QDC_SMS_SOAK_V1
RUN RUN
BOOT 当前boot-id
SUP 原监管pid 原监管start
SMS 实际C-pid 实际C-start
SOURCE 已安装C的dev 已安装C的ino 已安装C的bytes
BASELINE C启动前基线KiB
HTTP 192.168.225.1
SECONDS 3600
```

后台执行：

```sh
/run/qdc-tun-RUN/qdc-ike-soak soak /run/qdc-tun-RUN RUN_SOAK 3600 目标IP 测试VIP
```

guard 自己创建本机 `soak.log`、`RUN_SOAK.state`，取得 `SOAK_LOGGER_READY` 后才写 0600 `soak.go` 内容 `SOAK_GO RUN\n`。15 秒没有 GO 即停止。长任务不占 USB 标准输出；主机每 30 秒用独立短卡取回小型 `soak.status`，阶段结束取完整日志。

- 每 250 毫秒左右核验内存、短信及监管身份、内核日志；完整 smaps 每 10 秒，HTTP 200 与保护路由 ICMP 每 10 秒。主机持续显示实际经过分钟、最小可用内存、C/charon/CLI 峰值及成功组数。
- 稳定观察开始后第 60、120、180 秒分别终止本测试 IKE，确认 SA、VIP、目标路由消失，再重新建立并验证加密收发。只动本轮测试连接。
- 第 240 秒执行 CHILD rekey，确认入站及出站 ESP SPI 都变化，并再次验证保护流量。
- 持续至少真实 3600 秒且至少 300 组 ping/HTTP 成功；停止 VPN、收回全部子进程、清理凭据、地址/路由/接口并核对 C HTTP 后，最后才写生产 `soak.result`。
- 通过正常网页鉴权会话只发送一条带唯一标识的测试短信，号码使用用户指定可控接收端或模块已识别的自身号码。202 排队不算收发成功；确认提交结果和对应新入站消息。提交结果未知不重试。旧短信、号码和令牌不输出到验收日志。

## 停止条件、回退及清理

运行时仍为 6144 KiB 绝对下限、相对启动前基线最多下降 3072 KiB；内核异常/日志丢序、身份/挂载变化、未知资源、HTTP/保护流量失败、日志预算或期限越界均停止。本机长测日志最多 960 KiB，迁移日志最多 64 KiB（提前 4 KiB 触发停止以留回退记录）；总预算 1 MiB。长测最多 3720 秒再加 3 秒硬退出，未提交迁移最长 5400 秒。

正常中止仅向精确匹配 PID/start/exe 的本轮迁移 helper 发 TERM。它先精确停止本轮 VPN guard，最多等 12 秒，确认无 charon/swanctl、`/run/qdc-ike`、`ipsec0`；再释放本轮客户端文件及已知部分上传，停止 C、普通卸载自己的 mount、核对原 inode及完整时钟证明。**MemAvailable 至少 10240 KiB 才 CONT 原 SUP**，为原 Go 约 3.1 MiB 的恢复占用留空间，运行时停止线没有降低。

收到 `MANUAL_ROLLBACK_COMPLETE source_retained=1` 后独立核验原 Go 子进程、SHA、原 SUP、HTTP/SIM、数据及网络。C 安装仍保留，可稍后再手动使用。未知覆盖挂载、进程或恢复空间不足时保持 SUP 暂停并报告 `RECOVERY_REQUIRED`；不强制/懒卸载，不 killall、不清缓存。只有硬锁时按已核实的现场全断电和异常重启条件处理，重连后重新核对原程序。

验收通过后先取完整日志、停 VPN、普通卸载零引用的 TUN，清理本轮客户端和真实凭据，但**保留**迁移 helper/租约/日志及短 guardian 的时钟证明目录。然后写 0600 `commit.manual`：

```text
QDC_SMS_MANUAL_COMMIT_V1
RUN RUN
C_VERIFIED 1
VPN_CLEANED 1
```

helper 还要求真实 `soak.result`、本轮 guard 已退出、C 活身份、VPN 已清理及完整时钟证明，才输出 `MANUAL_COMMITTED boot_only=1 no_autostart=1`。此后仍保留 TERM 回 Go 能力和内存/日志/身份监测；当前开机继续 C，重启运行原 Go。不能执行旧包会删掉整个 clock 目录的最终清理卡，也不能点击网页旧的 previous 版本恢复来代替这条回退路径。

数据核对区分合法变化：本次测试短信、推送状态、时钟或流量允许新增/更新，但迁移前已有记录不得丢失。不把旧备份盲目覆盖回正在增长的业务文件。没有单独手工触发 APNs 或语音测试。
