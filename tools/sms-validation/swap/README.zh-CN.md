# 短信程序临时副本与文件绑定切换：电脑准备方案

当前阶段只允许电脑端编写、构建、静态检查和隔离 Linux 测试。本文和工具不构成实机操作授权；未经用户针对临时 NAND 写入及恢复条件的新确认，不连接、上传或执行任何设备命令。

## 要解决的问题与边界

完整 Go 程序约 6 MiB；放入 `/run` 会直接占用原本用于短信与 VPN 的 RAM。计划把已验证优化程序流式写入 `/data` 的本轮私有目录，再由小型原生 C helper 对原程序路径执行一次文件 bind mount。原 `/data/smsweb/smsweb` 字节不覆盖、不替换、不改权限，`start.sh` 不修改。设备重启会自然移除挂载；NAND 上的本轮副本仍须在正常结束或后续人工恢复时删除。

2026-11-15 主 agent 新鲜只读核验发现：`/data`、`/cache`、`/usrdata` 是同一 UBIFS 卷，仅约 2864 KiB 可用，**当前不足以容纳约 6 MiB 完整临时程序，本方案目前不满足实机存储准入**。不能把其他同卷路径当成额外容量；不删除原程序、旧备份、日志或其他用户数据来腾空间。现内核 `BLK_DEV_LOOP=n/FUSE=n/NFS=n`，不把已有 `SQUASHFS_XZ=y` 解释为可直接挂载 RAM 压缩镜像，不引入新内核模块。后续存储方案由主 agent 在电脑估算和复核，本轮 helper 不扩张范围。

所有路径由一个最多 48 字节、仅含字母、数字、`_`、`-` 的 RUNID 唯一确定：

| 用途 | 固定路径 |
|---|---|
| 原程序及原监管脚本 | `/data/smsweb/smsweb`、`/data/smsweb/start.sh` |
| 本轮数据副本 | `/data/qdc-sms-RUNID/smsweb` |
| 本轮 helper、控制卡、日志 | `/run/qdc-sms-RUNID/` |
| 原独立 clock guardian | `/run/qdc-tun-RUNID/qdc-ike-guard` |
| 原时钟恢复证明 | `/run/qdc-tun-RUNID/sms-guard.log` |

SMS 和 TUN 的 RAM 目录独立；helper 不清理或改写 TUN 目录。原程序目录、SMS 两个私有目录和原 clock guardian 目录必须为 root 所有的真实目录、权限严格为 0700。`/data` 和 `/run` 必须为 root 所有、不可被其他用户写入的真实目录。任何条件不满足就拒绝，不自动 chmod 现有目录。

## 准入与控制卡

实机准入必须在另一次明确授权后收集新鲜证据。电脑预生成的控制卡必须绑定当轮 boot ID、原监管 PID/starttime、原文件 dev/inode/size、本轮副本 dev/inode/size、原 clock guardian PID/starttime；不能填入猜测或旧 PID。helper 只接受固定版本、固定字段次序、固定 300 秒租约的控制卡，不接受任意路径或命令。

绑定前须核对原 f34 二进制 SHA-256、优化二进制 SHA-256 和构建记录。C helper 以打开的原文件描述符和 dev/inode/size 证明回退对象身份；绑定后另持有 target 上的 fd，钉住本 mount 引用，避免长租约期间挂载 ID 被回收复用，最终核对后关闭该 fd 再普通卸载。文件内容哈希由当轮操作卡在挂载前后独立核对。它不把 inode 相等解释为已经验收短信业务。

原监管必须精确匹配 `/bin/busybox /bin/sh /data/smsweb/start.sh supervise`，PID/starttime 不变且已 STOP；原短信进程必须已经终止，不能还存在使用原 inode 的活进程。helper 自己再次扫描并拒绝任何活的监管子进程。只读检查通过后，helper 才创建自己的独占租约日志、固定 source fd 和原 fd，再自行 `MS_BIND`，记录挂载 ID 和两组 inode。

## 上传和文件缓存

`copy RUNID BYTES` 是固定路径流式复制命令，唯一输入为 stdin；每次只持有小块输入。它只创建不存在的本轮 0700 数据目录及本轮 `smsweb`，不覆盖任何已有文件。每 64 KiB 对本轮文件做 `fdatasync` 和 `POSIX_FADV_DONTNEED`；结束做 `fsync`、目录 `fsync` 和再次针对本轮文件的 fadvise，然后输出 dev/inode/size。不得调用 `drop_caches`、全局 sync 或对其他文件 fadvise。

复制前必须用同一 `/data` 文件系统的 `statvfs` 确认可用空间至少为完整副本字节数加 1 MiB 保留量，且先检查再创建目录或文件；空间不足就拒绝。该保留量不保证 UBIFS 的可压缩性或并发业务写入结果，复制过程中任何写入/同步失败均中止，不进入挂载。只回收本工具此次新建且身份仍一致的不完整副本，禁止清理其他资源。

复制还有独立 90 秒单调期限，stdin 使用 100ms poll；即使 host 保持管道打开、不再发送数据也不无限期阻塞，SIGTERM/SIGINT 可中断输入等待。开始前、每 64KiB 同步前后、最终同步前后都检查 `MemAvailable >= 6144 KiB`，读取失败也视为不准入。低于底线立即只清理自己的 partial，目录或文件身份被替换则保留并报告恢复需求。内核不可中断的 I/O 本身仍不能由用户态严格限时；不把 90 秒描述为设备故障下所有 fsync 都必然按时返回。

这会在 NAND 临时写入约一个完整程序及必要元数据，可能因写回产生短暂额外内存；每块同步与回收只能约束本轮文件缓存，不能保证整个系统内存不变。必须在复制结束、校验完成、原始测量准入满足后再做共存实验。原 binary 及启动脚本哈希必须保持一致。

## 两个独立守护与顺序

`resume-clock ... 300` 的普通 TERM/TTL 行为保持原计划：恢复原实时钟加单调经过时间，再精确 CONT 原监管。实机必须采用另行修复并验收的新 guard；它还提供 SIGUSR1 的 restore-only 协议：恢复时钟、复核原监管、输出 `RESUME_DELEGATED verify_supervisor_before_cont=1` 后退出，不发 CONT。swap 回滚只请求这个委托协议，把最后 CONT 留给再次核对挂载/inode 的 swap helper。新 swap helper 使用单调时钟，独立最长 300 秒；收到 SIGTERM/SIGINT 也进入同一回滚。SIGHUP/SIGPIPE 不结束租约，ADB 会话断开不能撤销独立守护。

原 clock guardian 的 300 秒从它自己的起点计时。为了避免它在卸载窗口并发 CONT，swap 准入要求原 guardian 年龄不超过 180 秒；按真实 startticks 和严格匹配的 READY300 行计算，最迟在年龄 270 秒开始回滚，距原期限至少保留 30 秒。即使它已经提前完成时钟恢复，也不延长这一回滚上限；新 helper 自身的 300 秒只作额外上限。新 helper 不暂停或替换原 guardian。原 guardian 提前完成后，必须看见完整、无错误且顺序正确的时钟恢复证明；否则提前回滚并保持监管 STOP。

回滚顺序固定：

1. 重验原监管 PID/starttime/exe/argv，精确 STOP，确认确实处于 T/t 状态。
2. 仅处理其直接子进程；逐个复核 PID/starttime/父 PID 和 `/proc/PID/exe` 的 dev/inode 等于本轮 source。先 TERM、限时等待，仍为同一身份才 KILL；僵尸视为已退出，不使用 killall/pkill。
3. 再次确认监管 STOP、没有新的本轮子进程、没有未知直接子进程或其他父进程持有本轮 executable。若原 guardian 在窗口内 CONT，则重新 STOP 并复核；无法稳定确认就停止回滚动作并报告恢复需求。
4. 严格确认 target 顶层挂载仍为本 helper 记录的 mount ID，target inode 仍等于本轮副本，原 fd 身份仍等于原文件。只做普通 `umount`，禁止 lazy/force。未知覆盖挂载不卸载。
5. 确认 target 不再是独立挂载、重新打开后 dev/inode/size 与原 fd 一致，记录 `ORIGINAL_RESTORED`。
6. 原 clock guardian 若仍是精确身份，则通知它 SIGUSR1，等待它恢复时钟和退出；本次请求必须看见 `CLOCK_RESTORED ... before_supervisor_cont=1` 在 `RESUME_DELEGATED verify_supervisor_before_cont=1` 前，且没有 `RECOVERY_REQUIRED` 或 `RESUME_REFUSED`。若进入回滚前它就已退出，则可接受此前完成的 `CLOCK_RESTORED` → `RESUME_SENT verify_sms_health_separately=1`，或已完成的 delegated proof；不能用旧的 SENT 代替刚发出的 restore-only 请求结果。
7. 等待时钟守护结束后，再次核对 target 无独立挂载、原 fd/路径仍为原 inode、本轮 source 身份未变和监管身份，才 CONT 原监管；不能沿用等待前的路径验证。随后只删除 dev/inode/size 仍匹配的本轮数据副本，移除已空的本轮数据目录。审计日志保留在 RAM，供电脑取回和人工复核后清理。

任何未知挂载、进程身份、文件身份、时钟证明或普通卸载失败，都输出 `RECOVERY_REQUIRED`；不尝试清理未知资源，不以 CONT 掩盖失败。SIGKILL、内核挂死、断电不能由用户态 helper 拦截；重启移除 bind mount、原文件从未被覆盖，但这不替代当轮人工恢复条件。

未知覆盖或身份故障的 `RECOVERY_REQUIRED` 路径也会向精确匹配的原 guardian 请求 restore-only，随后再次 STOP，保持挂载/副本不动；日志分别记录 `clock_proven` 与 `guardian_retired`。若无法证明原 guardian 已退出或恢复成功，不能把 STOP 解释为永久锁止：它仍可能在原有 300 秒期限恢复时钟并 CONT。实机操作必须有人能在预留窗口内处理此类失败，必要时按另行确认的恢复条件重启；本 helper 不擅自暂停、kill 或改写原 guardian。

旧 clock guardian 有“clock_settime 失败仍 CONT”的缺陷，且不支持 USR1 委托。新 helper 不修改旧 guard 源码；实机准入必须固定采用主 agent 另行修复并通过失败/委托回归的新 guard SHA-256，核对最终构建收据后再启动它。不得向不支持协议的旧 guard 发送 USR1：默认信号行为可能使它退出而没有恢复时钟。旧故障下无法承诺从未短暂启动短信进程；swap 的坏时钟证明用例仍保留，验证本 helper 不会自行放宽恢复条件。

## 日志与成功含义

本轮 `swap.log` 为 root/0600、不可预先存在的普通文件；记录 boot/RUN、helper/监管/guardian 身份、原与副本 inode、mount ID、回滚原因、原文件恢复、clock proof、CONT、仅本轮数据清理。禁止短信正文、PIN、登录 token、证书私钥或原持久化文件内容进入日志。

`SWAP_READY` 仅表示临时挂载建立且独立租约已就绪；`SWAP_COMPLETE` 仅表示原文件恢复、时钟证明、原监管 CONT 与本轮副本删除完成。原 f34 恢复后的新短信子进程身份、只有一个实例、HTTP 入口和业务功能仍需另行只读验收；不凭该日志宣称实机共存已成功。

## 电脑验证和交付

新增源码和测试全部位于本目录，不修改旧 IKE/TUN guard。测试先验证有效控制卡、错误 RUN/身份/日志/挂载、租约提前回滚；在一次性、无网络 Linux 容器中验证流式复制、开放 stdin 下 TERM 中断和真实 90 秒到期、源文件身份替换拒删、6MiB 分块同步、3MiB 文件系统空间拒绝。仅测试的 linker wrap 注入 6143KiB，分别验证开始前/同步前/同步后低内存只清理本轮 partial，不编入正式 ARM helper。

独立 ARM guest 中用合成 supervisor、短信进程和支持新协议的 clock guardian 验证真实 bind mount/普通 umount、mount fd 钉住所有权、USR1 委托成功/失败、已完成 SENT 兼容、源文件清理、时钟等待窗口再次核对、未知覆盖挂载保留且 guardian 委托退出。模拟 guardian 不修改电脑或宿主系统时间。最后交叉编译 ARMv7 soft-float 静态 musl ELF，检查无动态解释器并生成 SHA-256 记录；helper 和 initrd 哈希共同密封，运行器在启动 QEMU 前拒绝任何变更。

构建只使用独立 `/work/sms-swap-*` 临时目录；不改旧构建产物，不与公平内存测量并行运行 QEMU。设备卡以不可执行文本交付，明确标注“待新的实机授权及当轮身份绑定”；本轮不运行卡、不连接设备、不上传文件、不写 NAND。

电脑构建命令：`sh tools/sms-validation/swap/build.sh /absolute/private-output`。ARM 验收命令：`python3 tools/sms-validation/swap/run_vm.py /absolute/private-output --kernel /absolute/lab-only-runtime-zImage`。Docker 内的 amd64 进程在 Apple Silicon Rosetta 下会向其他进程显示 emulator 的 `/proc/PID/exe`，因此不降低精确 exe/inode 检查来迁就它：容器只验复制/空间/目录身份；真正的进程和挂载验收在独立 ARM guest 中完成。

300 秒本地租约和原 clock 起点 270 秒上限由同一 C 策略的边界单元测试检查；ARM VM 验证 SIGTERM 触发的同一回滚路径，不把秒级 VM 功能验收描述为已经等待完整 270/300 秒的实时时长验收。
