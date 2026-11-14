# QDC507 IKEv2/IPsec 实机临时测试方案

2026-11-13。**本轮只在电脑准备方案、程序、请求卡及模拟验证；没有连接模块、上传文件、加载驱动或运行客户端。** 后续实机执行须另行获得用户授权。本方案只验收无 VPN 连接的短时启动；真实服务器、凭据、加密流量和常驻安装是后续独立阶段。

**2026-11-14 执行补充：** 用户已要求继续下一步，并再次确认现场可切断全部实际供电。A01 在客户端上传门槛测得 12144 KiB，按方案停止，未上传客户端或加载 TUN；短信和 RAM 清理已验收。随后将同一份 TUN 测试器源码改用现有静态 musl 工具链构建，由 525344 缩小为 70940 字节，减少 454404 字节暂存开销。18 项 ARM/Linux 3.18.44 VM 场景（含新增测试器的门控、TUN 创建和收发）通过后，可在新运行编号中显式选择 --compact-probe。12/8/6 MiB 门槛、停止策略、客户端、tun.ko、USB 控制器均不改变；原构建和结果保留。

A02 随后完成两段原生空载启动及清理，见 [执行记录](RUNTIME_TEST_2026-11-14.zh-CN.md)。由于同一暂停窗口执行两段观察，恢复请求接近 240 秒上限；未来执行时应将观察段拆成独立的短信暂停窗口，每段重新核对准入，并预留至少 30 秒业务恢复余量。300 秒守护期限不等同于业务暂停窗口，不能只凭守护还活着继续下一段。

## 依据与测试范围

客户端编译、RSA/PSK 模拟协商和资源参考见 [离线评估](ASSESSMENT.zh-CN.md)。TUN 已完成的实机验证见 [第二轮记录](../tun-validation/RUNTIME_TEST_2026-11-13_ROUND2.zh-CN.md)，日志通道行为见 [USB 记录](../tun-validation/USB_LOG_CHANNEL.zh-CN.md)。这些是历史证据，执行前必须重新读取设备身份、内核配置和当天基线。

本轮准备的客户端仍为 strongSwan 6.1.0，ARMv7/EABI5/soft-float、静态链接、16 工作线程，采用 kernel-libipsec + kernel-netlink。没有真实连接、凭据、配置 include 或自动加载连接的操作。配置关闭默认 syslog 输出，应用日志回传电脑；不向模块数据分区写测试日志。

[上游说明](https://docs.strongswan.org/docs/latest/plugins/kernel-libipsec.html) 和本机源码表明该后端使用 TUN；因此启动 charon 本身就是驱动/网络操作。监督器必须先 unshare 新网络命名空间，核对只有 lo，再启动客户端；该空间没有物理接口、veth、外网出口或默认路由。创建的 ipsec0、UDP 500/4500 和可能涉及的网络 sysctl 均限制在该空间，最终通过进程退出释放。本方案不调用 ip netns add，不创建持久命名空间绑定。

## 准入与停止条件

| 项目 | 执行前要求 |
|---|---|
| 授权 | 明确允许本轮 RAM 上传、TUN 加载和客户端短时执行；本次准备请求不代表已授权执行 |
| 恢复 | 用户此前已同意现场断电和异常时重启；执行时核实现场仍具备全供电切断条件 |
| 设备 | 精确 USB VID 0x2ca3/PID 0x4006/location 0x100000、QDC507 身份；只打开 ADB bulk，不使用 AT/reset/recover |
| 内核 | Linux 3.18.44，配置 gzip SHA-256 为 1fea60b794568c65cd6dfe436fbc2b9dabdf8c16267f01585f171174d5361620；变化即停止复核 |
| 独占条件 | 无 charon/swanctl；/run/qdc-ike、候选暂存目录、TUN 驱动及 /dev/net/tun 均未存在；已有对象不接管、不覆盖 |
| 文件系统 | /run 为真实、可执行、可写 tmpfs；/dev 为可创建字符节点的 tmpfs。/tmp 不能当作 RAM 使用，/data 不用于暂存 |
| 恢复工具准备 | 暂存小型监督器及原 TUN 测试器前 MemAvailable 至少 8 MiB；分别合并校验，避免同时保留全部分片和成品 |
| 客户端准入 | 日志 canary 通过后、上传客户端前，连续 3 次 MemAvailable ≥12 MiB，间隔 5 秒；达不到就不上传客户端 |
| 启动准入 | 暂存完成、即将启动时以及收到 GO 后均要求至少 8 MiB；本机监督器执行此门槛 |
| 运行保护 | 250 ms 检查 MemAvailable；低于 6 MiB，或比启动基线下降超过 3 MiB，停止本轮用户进程 |
| 日志 | 每次输出先在电脑落盘并 fsync，再 ACK；LOGGER_READY 落盘后才 GO。kmsg 丢序、EPIPE、读取错误、新增异常或超过日志预算均失败 |

12/8/6 MiB 和下降 3 MiB 是本方案的保守准入/停止策略，不是测得的峰值保证。上轮 22:08 末次可用内存约 9.5 MiB，当前状态尚未重新采样；4–6 MiB 预算也不能覆盖任意流量。新增监督器约 75 KiB，原 TUN 测试器约 513 KiB，以及命名空间、分片合并峰值，都要计入实测余量。恢复工具准备本身不满足客户端准入。

## 电脑上准备及复核

以下两个命令只在电脑构建和生成文件，不连接 USB：

```sh
sh tools/ike-validation/runtime/build.sh /Users/shield/Documents/ChatGPT/DJI-4G/ike-validation-local
python3 tools/ike-validation/runtime/prepare.py /Users/shield/Documents/ChatGPT/DJI-4G/ike-validation-local --tun-local /Users/shield/Documents/ChatGPT/DJI-4G/tun-validation-local --run IKE20261113PREP01 --out /Users/shield/Documents/ChatGPT/DJI-4G/ike-validation-local/prepared-IKE20261113PREP01
```

生成目录必须不存在；新的实测应生成新的运行 ID 和目录。程序固定校验原 charon、swanctl、tun.ko、TUN 测试器摘要，并核对新监督器的构建记录。ELF 检查拒绝动态加载器、动态段、错误架构和硬浮点 ABI。每个传输分片不超过 1 MiB，不改变已经验证的 USB 控制器。

使用精简测试器时，在 prepare.py 命令后增加 --compact-probe，并使用新的运行 ID/目录。该选项要求构建元数据与已完成的 VM 验收回执摘要一致，拒绝未验证或被替换的产物；预备目录同时保存 compact-probe-vm.json。legacy 默认产物和原固定摘要仍可复现。

预备目录含 manifest.json、build.json、payload 和逐项 cards。**manifest.stages 才是顺序；不要按文件名排序后批量发送，也不要把整个目录接到控制器 stdin。** 每个分片组上传后立即验证、合并、再验证整件并移除分片，之后才进入下一组。客户端程序始终先校验后 chmod 可执行。重复阶段和摘要不符均停止，不能自动重试覆盖。

## 实机分阶段执行

下面是未来获授权后的步骤，本轮没有执行这些请求卡。使用独立主机日志目录，控制器的 VID/PID/location/run 与当天记录一致。通过专用交互 PTY 输入时先对该 PTY 使用 stty -icanon -echo，避免长 JSON 被 canonical 模式截断；不修改用户其他终端。

1. **基线：00-readonly。** 读取内核/配置摘要、boot_id、taint、原模块、挂载、三次内存、地址/IPv4/IPv6 路由、UDP socket、DNS 摘要、防火墙和相关进程。另从电脑以 5 秒超时读取短信网页 HTTP 状态，只访问首页，不发短信或拨号。保存 supervisor 和 smsweb 的 PID、starttime、exe、cmdline、父子关系。不得沿用历史 PID 431/460。缺少已核验的只读命令时先诊断，不安装工具补齐。
2. **恢复工具准备：10-create-ram 及首两组 20/21 卡。** 确认 RAM 和空闲对象，创建 /run/qdc-tun-运行ID（0700）及 RUN_ID；先只传监督器和 TUN 测试器。每组 SHA-256 通过后再继续，运行 22-bootstrap-selftest。此时仍未上传客户端。
3. **日志稳定性：22-log-canary-1/2/3。** 不加载 TUN，分别采集 30 秒并执行 /bin/busybox true。每轮要有 LOGGER_COMPLETE、远端 RC=0、完整结束标记，USB trace 零错误、kmsg 无丢序。传输异常立即停止，不自动重连或重放。
4. **必要时暂停短信业务。** 只使用下节的身份校验和限时恢复守护；沿用用户已明确允许的 smsweb 暂停范围，不停语音/蜂窝服务，不运行完整启动脚本、不修改启动文件。随后运行 23-client-memory-gate。达不到 12 MiB 就恢复业务、清理准备文件，结束本轮。
5. **客户端暂存：其余 20/21 卡、25-selftest。** 按 manifest 顺序逐件暂存 charon、swanctl、原 tun.ko 和空连接配置。每件合并前检查至少 8 MiB；保留逐片与整件校验输出。不写 /opt、/etc、/lib/modules、/data 或启动配置。
6. **驱动加载：30-load-tun。** 原 TUN 测试器在日志 READY/GO 后执行 /sbin/insmod，命令最多 5 秒，结束后再观察 10 秒。失败或异常不进入客户端步骤。31-check-tun 核对 Live/refcnt、misc 注册 10:200、自动生成的 /dev/net/tun、节点所属和权限、内存。设备未自动生成节点时停止，先单独查明，不让 charon 触发自动加载。
7. **10 秒启动检查：40-idle-10s。** 监督器先只读打开 kmsg 并等待 GO；GO 后重查内存，创建唯一 .state，进入独立网络空间，再独占创建 /run/qdc-ike。启动 charon，确认 ipsec0 和 VICI ready，调用 --stats/--list-conns/--list-sas，只做查询。人工核对连接和 SA 均为空。完成 10 秒观察后正常退出、关闭 TUN、回收子进程、移除本轮 /run/qdc-ike。
8. **60 秒观察：41-idle-60s。** 只有上一段完整通过且短信恢复守护剩余时间大于 95 秒才进行。相同机制观察 60 秒；主机每段上限 90 秒，监督器在 GO 后最多观察时长 +20 秒发 TERM，再过 3 秒发 KILL 并退出。启动/查询变慢也会导致失败，不延长时限掩盖故障。
9. **正常回收：80、85、短信恢复、86、90、99。** 普通卸载后先释放客户端文件，再恢复短信并观察日志，最后清理工具和对照基线。每一步核对后才继续；不能直接跳到清文件卡。

本方案不做 5–10 分钟连续运行、真实 IKEv2 连接、带宽压力或业务网络接管；先取得本轮原生启动证据，再制定后续实验。

## 短信暂停、自动恢复及现场恢复

只有需要腾出内存且用户原授权仍适用时使用。监督器 resume 模式只接受当前 /bin/busybox，且 cmdline 精确为 /bin/busybox、/bin/sh、/data/smsweb/start.sh、supervise；PID 或 starttime 不符就拒绝发送信号。它只发 CONT，不启动服务或修改配置。

操作员从基线确定本轮 RAM_ROOT、SUP_PID、SUP_START、SMS_PID、SMS_START；用 qdc-ike-guard describe PID 读取身份，用 identity PID START EXE 返回码核对，不能从历史记录复制数值。以下是动作顺序，变量由本轮记录填写后才执行：

1. 在独立 shell 中先忽略 HUP，后台运行 RAM_ROOT/qdc-ike-guard resume SUP_PID SUP_START 300，将至多几行输出保存到 RAM_ROOT/sms-guard.log。记录恢复守护本身的 PID/starttime/exe；等到 RESUME_GUARD_READY，重新确认守护还存活且身份相符。
2. 验证 supervisor 的 cmdline/身份，先发 STOP 并读回状态 T；验证 smsweb 是它的当前子进程后，仅 TERM 该子进程，等待退出（或已僵尸且 RSS=0）。不杀按名字模糊匹配的其他进程。
3. 从 STOP 起整个窗口控制在 240 秒内；300 秒恢复守护到期会重新校验身份并 CONT。若主机断线，它继续计时。若提前收到 TERM/INT，也会先校验再 CONT。恢复守护不受测试命名空间和客户端硬超时管理。
4. 正常结束时先停止客户端、普通卸载 TUN，85 卡释放客户端 RAM 文件，再按 PID/starttime/exe 核对原 supervisor 并 CONT；随后对身份核验通过的恢复守护发 TERM，等待它输出 RESUME_SENT 并退出。确认原循环拉起新的 smsweb，父进程不变，首页 HTTP 200。异常情况下优先结束客户端并恢复业务，不为了等待后续清理延长暂停窗口。不要执行 start.sh start，避免重复执行语音准备逻辑。
5. 若恢复守护已经到期，先验证业务已恢复和内存门槛，不继续假定短信仍停用。若 supervisor 身份改变，停止自动操作，读取新的业务状态再恢复；不得向复用 PID 发信号。

guard resume 的恢复只在操作系统仍可调度时有效。内核硬锁时走现场断电流程。短信服务本身恢复时可能正常更新其日志/PID 文件，本方案没有主动改写它的程序或启动配置。

## 日志与内存数据

- 主机目录 0700；每阶段 .raw、events.jsonl、usb-transfers.jsonl 为独立 0600 文件，拒绝覆盖。原始日志不提交 Git；摘要、脱敏验收结果和工具版本可以提交。每段必须核对控制器 status=ok、remote_rc=0，缺少 END/RC 就是未知状态。
- 监督器输出 LOGGER_READY、NETNS 原/新 ID、NS_INTERFACE、OWNED 进程身份、APP/VICI、带序号 KMSG、MEM、SMAPS、MEMORY_SUMMARY、IDLE_COMPLETE；其 state 文件记录 guard/charon/CLI 的 PID/starttime/预期 exe，供异常恢复核对。
- 系统余量每约 250 ms 读取；smaps 每约 1 秒读取至 EOF，128 KiB 缓冲不足时拒绝该样本，不使用部分数据。charon 或监督器的 smaps 采集失败会中止；短时 CLI 可能在读取前退出，此时回退到退出时 wait4 的 ru_maxrss。记录 RSS/PSS/匿名页/私有脏页，区分守护进程、管理工具和监督器；对子进程退出的 ru_maxrss 与成功采样择大。
- VSS 不是实际占用，tmpfs 程序页与进程 RSS 有重叠；不能把所有文件大小和完整 RSS 相加。报告峰值需标注采样周期，短时 CLI 分配可能漏采，ru_maxrss 也保留旧内核计数的限制。整机余量下降是实际准入依据。APP/VICI 在监督器合并流中按读取块加前缀，.raw 原样保存此合并流，不能当作单独应用流的逐字节文件；内核记录保留完整 kmsg 内容和序号。
- 每段内核与应用输出合计最多 1 MiB；主机旧控制器另有 8 MiB 保留上限。kmsg 读取失败/丢序、新 WARN/Oops/BUG/OOM/分配失败、任一子进程异常、内存门槛失败或期限到达，均停止向后推进。

## 正常清理验收

1. 每段必须回读 IDLE_COMPLETE rc=0 children_reaped=1、命名空间中只剩 lo，以及 /run/qdc-ike 消失。原网络空间不得出现 ipsec0、额外 UDP 500/4500 或新的路由；不能只凭进程列表中没有 charon 判定清理完成。
2. 核对本轮所有 guard/charon/swanctl PID 已消失；如 PID 已复用，按身份判断，绝不终止新进程。核对没有 namespace bind mount、进程或打开的 namespace FD 保留测试空间。监督器不创建任何持久绑定；异常时读取 /proc/*/ns/net 和 fd 链接核实。
3. 确认 TUN 是本轮从无到有加载、refcnt=0，再执行 80-unload-tun 的普通 rmmod 和日志观察。未加载且确认驱动仍不存在时跳过 80，不为清理重新加载。**不用 rmmod -f。** 失败、D 状态或引用未归零时保留现场，不删除驱动文件或伪造成功。
4. 核对客户端进程已退出，先执行 85-release-client-files，释放 charon/swanctl/配置和驱动文件，保留监督器、TUN 测试器和身份记录。恢复短信 supervisor、停止恢复守护并确认 HTTP 200；86-post-restore-log 观察 30 秒。核对无本轮后台进程后执行 90-clean-owned-files；该卡只删除 RUN_ID 清单内的文件并 rmdir，遇到未知文件会失败，绝不递归删除目录。部分上传/失败也沿同一身份核对和清单清理路径处理。
5. /dev/net/tun 若由 mdev 自动移除，无需处理。若遗留，必须对照基线确认本轮创建、仍为 root 所有的 c 10:200、驱动已卸载且没有其他用户，再单独删除该节点；仅当 /dev/net 原先不存在且当前为空时 rmdir。不能删除既有节点或整个 /dev/net。
6. 99-readonly-after 加 HTTP 检查，对照 boot_id、taint、原语音模块、地址、配置路由、网络 sysctl、DNS 摘要和防火墙；IPv6 动态缓存单列，不把正常缓存变化认作配置写入。内存等业务恢复后重新采样，不要求缓存瞬间逐字节回到基线。86 卡的 30 秒内核后观察在删除工具前完成。

## 异常恢复

| 情况 | 操作 |
|---|---|
| 没有 GO/准备失败 | 不启动客户端；确认没有新增状态/运行目录，恢复短信（若已暂停），清理已确认的准备文件 |
| 低内存/应用异常 | 监督器 TERM 本轮直接子进程组，最多 4 秒后仅对该组 KILL，再等 1 秒；全部回收后检查 TUN/运行目录。失败即标记 RECOVERY_REQUIRED，不能继续下一段 |
| 新内核异常/kmsg 丢序 | 立即停止测试，保留日志；监督器尝试终止本轮用户进程，但不卸载驱动。先判断内核是否仍健康，再按确切身份恢复业务；不能把监督器返回失败当作已完全恢复 |
| USB 中止/超时/主机落盘失败 | 控制器保持未知状态并拒绝重放；不要马上重发 insmod/idle/rmmod。原生监督器在可调度且信号可达时受硬时限限制；300 秒短信守护独立 CONT。重新打开通道前核验设备身份、boot_id 和进程状态，再决定清理 |
| 监督器被硬终止 | charon/CLI 设置了父进程死亡 SIGKILL；仍须核对它们的实际退出、命名空间及 TUN。/run/qdc-ike 可能保留：核对 .owner 对应本轮 token，所有本轮进程/FD 已退出后，只删 charon.pid/charon.vici/.owner 并 rmdir；存在未知文件则停止，不 rm -rf |
| D 状态/内核无响应 | 停止追加驱动/网络调用，不强制卸载。依此前授权在现场切断全部实际供电，再恢复供电；重新读取 boot_id、原模块/业务/网络，说明这是异常恢复，不把本轮记为通过 |

硬时限、进程信号与 RAM 清理不能修复失去调度能力的内核。设备没有已验证的硬锁自动恢复机制，本方案不写 panic/watchdog/OOM/sysctl 参数来改变它。

## 本轮交付与后续验收

电脑端结果记录见 [runtime/preparation-result.json](runtime/preparation-result.json)，执行时用 [记录模板](TEMP_RUNTIME_RUN_RECORD.template.md)。本轮只对工具的离线测试结果负责，不填写实机“通过”。完成实机启动、内存和清理验收之后，才准备指定测试服务器的单连接加密收发方案；不自动扩展至持久安装、刷机或开机启动。
