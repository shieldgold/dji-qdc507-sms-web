# QDC507 短信网页与 IKEv2/IPsec 共存临时测试方案

本文原为电脑准备阶段方案，下文“本轮不执行”指该准备阶段。用户随后授权按本方案重新核验、满足门槛后执行临时测试；[D02 实机准入结果](COEXIST_TEST_2026-11-14.zh-CN.md) 未达到默认内存门槛，已恢复清理，未进入共存窗口。

本方案和工具只在电脑上准备。本文的步骤属于后续实机测试，**本轮不向模块上传文件、不运行客户端、不连接模块采集状态，不刷机、不安装、不设置自启**。准备完成不等于模块共存验收通过。

## 1. 测试目标和影响范围

上一轮已证明短信暂停时可通过 4G 直连建立证书 IKEv2、取得测试 VIP、加密收发和重新协商。共存仍待验证。本次工具面向首次 **30 秒** 共存窗口：原短信监管循环恢复并启动新短信进程时，VPN 仍能持续通过固定目标 `/32` 加密收发，网页 `GET /` 正常，日志完整、内存不越线。

沿用原用户空间 `kernel-libipsec` 客户端及匹配的 `tun.ko`。不调整短信业务代码、`start.sh`、路由默认项、DNS、防火墙、APN、SIM、固件或启动文件。只访问网页入口，不提交 PIN、不登录、不读短信内容、不发送短信、不测试电话。网页 HTTP 200 只证明入口可服务；登录后的轮询和短信收发需要后续单独验收。

首次共存采用先暂停短信、建立 VPN，再恢复原短信循环的顺序。它会造成一次有界网页中断。不是短信始终不中断的热接入测试，也不代表长期稳定性或断网重连已通过。

## 2. 已准备的工具

| 文件 | 用途 |
| --- | --- |
| `runtime/build-coexist.sh` | 仅电脑构建新静态 ARM 守护程序及运行 CPU 测试，输出独立 `coexist-build`，保留既有实机产物 |
| `runtime/coexist.c`、`coexist.h` | 控制文件校验、启动 ID/进程身份绑定、原时钟及短信恢复交接、共存期短信身份监测 |
| `runtime/http_probe.c`、`http_probe.h` | 本机 RFC1918/回环 IPv4 的 8787 端口 `GET /`，2 秒期限，不记录正文或响应头 |
| `runtime/guard.c` 的 `coexist` | VPN 建立、释放 swanctl、恢复短信、30 秒数据与网页检查、内存/日志停止线和 VPN 清理 |
| `runtime/prepare_coexist.py` | 电脑准备未绑定包；另行使用新鲜公开身份生成单步请求卡；从不打开 USB 或网络 |
| `runtime/make-coexist-vm.sh`、`verify-coexist-vm.py` | 两台本地 ARM guest 私有网络内的模拟短信与真实 IKEv2 实验，不使用实机证书 |
| 已有 `qdc507-ike-control` | 未来逐卡执行时的 USB 通道、LOGGER_READY/GO 交握和电脑日志落盘；本轮不启动 |

准备包目录 0700、文件 0600；密钥仅在受保护的电脑目录和未来单轮 RAM 目录中存在。Git 仅保存源代码、公开摘要与方案，不保存密钥、真实短信、原始业务日志或二进制。

## 3. 准入与停止条件

| 阶段 | 条件 |
| --- | --- |
| 当轮身份 | 重新采集启动 ID、内核配置哈希、USB 标识、监管/SMS 的 PID + starttime + exe + argv；不得复制历史 PID |
| 物理恢复 | 未来执行前核实现场能切断全部实际供电；若硬锁，按当轮授权重启 |
| 客户端入场 | 短信暂停后、客户端主要文件入 RAM 前连续 3 次 `MemAvailable >= 12288 KiB`，间隔 5 秒 |
| 文件合并 | 低复制移动首分片、追加余片；每片和最终文件哈希均匹配；保留 8 MiB 可用内存及模型余量 |
| 启动前 | `MemAvailable >= 8192 KiB`；UDP 500/4500 空闲，无既有 ipsec0/VIP、测试目标的非默认路由或 `/run/qdc-ike` |
| 运行中 | 可用内存低于 6144 KiB，或比守护启动基线下降超过 3072 KiB，立即停止本轮 VPN |
| 业务身份 | 原监管 PID/starttime/argv 一致；恢复后只有一个新的、属于原监管的 `/data/smsweb/smsweb`，未停止、未重启 |
| 日志/数据 | 内核告警、OOM、内核日志序号缺口/读取失败、日志超过 1 MiB、HTTP 非 200/超时、加密 ping 失败、VIP/路由改变均中止 |
| 时间 | 共存固定 30 秒；守护 80 秒到期 TERM 本轮子进程，再 3 秒强制结束；单张电脑请求卡上限 90 秒 |

上一轮 11.5 MiB 准入只适用于当时已授权的那一次测试。本准备包恢复默认 12 MiB，不能沿用上次例外。若未来达不到准入，停在客户端启动前并恢复短信，不擅自停其他服务或降低阈值。

## 4. 日志采集和内存监测

1. 电脑建立独立的单轮日志目录，先落盘请求、原始 USB 返回和解析结果；权限 0700/0600。每张卡带唯一 run/token。禁止整批重放 cards。
2. 上传和启动前先做 3 轮各 30 秒日志通道 canary。只有连续稳定、没有传输错误或短写，才进入下一步。正常 USB 短读应继续组帧，不能当成数据丢失，也不能把错误忽略。
3. 内核采集从当前 `/dev/kmsg` 尾部开始，stdout 无缓冲。电脑观察到对应 `LOGGER_READY` 并确认接收、落盘后才发送 `GO`。5 秒内没有 GO 就退出，不创建客户端运行目录或网络对象。
4. 守护每次循环最多等待 250 ms 并检查 `MemAvailable` 和内核日志；大约每秒记录 MEM 和完整读到 EOF 的 smaps：guard、charon、CLI，以及恢复后的 smsweb 的 RSS/PSS/匿名页/脏页。不是只读取 `/proc/status` 的单个 RSS 数字。内核和调度可能延迟采样，不能当成硬实时监测。
5. 记录基线、最低可用内存、各进程峰值、短信恢复 PID/starttime、共存开始/结束单调时间、10 次 ping/HTTP 结果。HTTP 探针只输出 `status/transport/bytes`，不记录正文、Cookie、PIN 或短信内容。
6. 电脑记录 USB 读写错误、短写、阶段时间；任何接收故障立即停止进入下一张卡，转恢复流程。不要因模块 stdout 卡住而连续提交新命令。独立恢复守护不依赖 USB 日志流。

## 5. 后续实机顺序（本轮不执行）

1. **只读基线。** 重新确认设备/内核/启动 ID、RAM/no swap、执行型 tmpfs、当前运营商数据连接、短信 HTTP、原监管和短信进程身份，保存 IPv4/IPv6 地址路由、DNS 哈希、防火墙、模块和 taint。核验证书有效期、数值 IKE 入口及服务端测试身份空闲。当前日期和网络状态必须实查，历史结果只作参考。
2. **新 run 和 RAM 目录。** 使用新鲜 run ID，确认没有同名残留。先只上传小恢复守护与探针，校验哈希、自检和日志 canary。所有临时文件只放本轮 `/run/qdc-tun-RUN`；不用 `/data` 安装目录。
3. **独立恢复先就绪。** 用本轮 RAM helper 启动 `resume-clock SUP_PID SUP_START 300`，stdout/stderr 定向本轮 `sms-guard.log`（0600），记录它的 PID/starttime/exe/精确 argv。它先捕获原实时时钟和单调时钟，300 秒自动恢复，或收到 TERM/INT 提前恢复；忽略 HUP。日志必须出现唯一 READY，之后才允许 STOP 原监管、TERM 原短信子进程。不得 killall 或 pkill。
4. **暂停短信并入场。** 再次精确验证原监管与短信身份，暂停监管，正常终止其短信子进程并确认退出。执行 3 次 12 MiB 准入；不足则直接恢复，不加载客户端。按单片请求卡上传、低复制合并并校验其余已审阅文件。只有确有证书时钟需要时才临时修正系统时钟，不写 RTC；记录原值由独立守护恢复。
5. **后绑定。** 在主要文件已就绪、原短信确已退出后采集公开身份快照，`observed_at` 必须在 60 秒内。电脑用 `prepare_coexist.py bind` 在新目录生成 `coexist.control` 和请求卡。原生工具还会核实当前 boot、监管停止状态、恢复守护身份、精确 argv 和年龄不超过 180 秒。未绑定包没有 `40-coexist` 卡，不能直接启动。此时应保留足够时间，不在等待中耗尽 300 秒恢复期限。
6. **TUN 临时加载。** 用带日志交握的 30-load-tun 卡普通 insmod，核验 `10:200` 和运行态；无持久化。测试节点须与基线核对归属，只有本轮创建的字符节点才允许最后清理。
7. **执行一张共存卡。** 新守护先建立证书 IKEv2，确认预期 VIP 和唯一目标 `/32` 路由，完成 5 个加密预检 ping。退出 swanctl 后验证该文件归属并 unlink，释放约 886 KiB RAM 文件空间；不改变已运行的 charon。随后只向精确匹配的恢复守护发送 TERM。
8. **先恢复时钟，再恢复短信。** 恢复守护必须先记 `CLOCK_RESTORED ... before_supervisor_cont=1`，再 CONT 原监管并记 `RESUME_SENT`。共存守护校验顺序、原监管和新短信子进程；最多等待 12 秒网页启动。时钟恢复失败即本轮失败，即使监管已经恢复也不能宣称通过。
9. **30 秒共存。** 网页准备好后开始计时，每 3 秒依次进行一次从预期 VIP 到固定目标的加密 ping 和一次本机网页检查，共 10 对。期间持续采集 charon 与 smsweb 内存、日志及身份。只要一个条件失败就结束 VPN；不再次停止已恢复的短信。首次共存不再重做 CHILD rekey，上一轮独立重协商结果不能替代本轮共存证据。
10. **正常结束。** 普通 TERM 本轮 charon，等待并回收直接子进程，验证 ipsec0/VIP/目标路由消失，删除本轮 `/run/qdc-ike` 的精确已知文件。若仍有子进程或未知条目，保留现场并报告，不能递归删除或强卸载。

## 6. 异常恢复

| 情况 | 恢复顺序 |
| --- | --- |
| 未获得 GO 或 VPN 建立失败，短信还暂停 | 先确认本轮客户端停止；若仍有进程，只对已记录且 PID/starttime/exe 一致的本轮守护发送 TERM，等待其回收子进程。正常卸载 TUN、释放客户端文件，再精确通知原 `resume-clock` 守护提前恢复；核实日志时钟在 CONT 前恢复，原监管和新短信、网页正常。独立 300 秒到期恢复是兜底，不是继续测试的理由 |
| 共存期间 HTTP、短信身份或内存失败 | 守护停止并回收本轮 VPN。原短信监管继续运行；不要再 STOP 或 kill 新短信进程。核验运行态和网页，再清理 VPN/TUN |
| USB/电脑日志采集断开 | 停止提交新卡。客户端 80+3 秒期限独立运行；原短信恢复守护 300 秒独立运行。通道恢复后只读核实准确身份和运行态，再决定精确 TERM/清理；不能按旧 PID 盲目发信号 |
| 时钟恢复失败 | 标记恢复未完成、停止 VPN；保留错误日志。由原始时钟/当轮基线核对后恢复业务时钟，验证原监管及网页；不能拿 HTTP 200 覆盖时钟错误 |
| 客户端未退出/内核异常/硬锁 | 不删仍在使用的文件、不强卸载 TUN。若通道尚在，保留日志和身份；硬锁按当轮现场恢复授权切断全部供电。重启后先只读对照启动和业务基线，不继续同一轮 |

手动提前恢复只发送 TERM 给**本轮独立恢复守护**，由它完成时钟与 CONT 顺序。不要直接 CONT 原监管绕过时钟恢复。每次信号前用 helper `identity PID START EXE` 及精确 argv 核实身份；若不匹配，停止发信号，重新查明真实状态。

## 7. 清理和最终验收

1. 等本轮 charon/CLI/共存守护退出并被回收；确认 `/run/qdc-ike`、ipsec0、VIP 和测试目标路由消失。客户端异常退出不等于已清理。
2. 确认 tun 引用计数为 0，使用 80-unload-tun 的带日志普通 rmmod；不使用 force。只有本轮加载的 tun 才卸载。
3. 85-release-client-files 只删除本轮清单内客户端 RAM 文件，回收内存；若短信尚暂停，在此后精确通知原恢复守护，核验时钟→CONT→短信重启→HTTP。
4. 短信恢复后采集 30 秒 post-restore 内核日志；确认监管仍为原身份、只有一个健康新子进程，无重复循环。首次测试不读取短信正文。
5. 90-clean-owned-files 拒绝任何仍从本轮目录运行的进程，拒绝未知目录/文件。按白名单删除控制文件、分片、状态、密钥、日志和探针，再删 RUN_ID 和空目录。原始日志已先保存电脑；保留其他服务文件。
6. 若 mdev 留下 `/dev/net/tun`，仅在基线不存在、设备号为本轮 `10:200`、tun 已卸载且无使用者时清理它及本轮创建的空 `/dev/net`；本轮以前已有的节点/目录保留。
7. 99-readonly-after 回读地址、路由、DNS、防火墙、boot ID、taint、模块和内存；与本轮基线对比，只允许明确的运营商动态变化，并解释差异。测试身份在服务端无活动 SA，测试进程、模块和目录无残留，短信网页正常。无需改服务端共享规则。
8. 电脑关闭本轮 USB 控制器和所有本地 QEMU/实验辅助进程，保留受保护证据目录。无安装、无自启、无刷机。

通过判据必须同时满足：完整 30 秒、10 对加密 ping 和 HTTP 200、charon/smsweb 同时采样、身份和时钟恢复正确、USB/内核日志完整、内存无越线、VPN/TUN 清理及短信恢复完成。ARM 实验通过只证明工具的这些路径，不证明真实模块具备相同内存余量。

## 8. 电脑准备和实验命令

以下只在电脑构建与生成文件，不包含模块执行命令：

```sh
sh tools/ike-validation/runtime/build-coexist.sh /absolute/ike-validation-local
python3 tools/ike-validation/runtime/prepare_coexist.py prepare /absolute/ike-validation-local \
  --tun-local /absolute/tun-validation-local --guard-build /absolute/ike-validation-local/coexist-build \
  --credentials /absolute/private-credentials --params /absolute/public-connection.json \
  --direct IKE_SERVER_IPV4 --run NEW_RUN_ID --out /absolute/new-unbound-bundle
```

准备器要求新守护哈希对应的 ARM 共存实验回执和 20 项旧回归回执。未来 `bind` 的快照格式见 `runtime/test_prepare_coexist.py` 的合成示例；示例 PID 和 boot ID 只用于电脑测试，不能用于模块。

`prepare_coexist.py accept` 只读复核 `--coexist-logs` 与 `--regression-logs`，核对两份 initrd **内嵌**守护及同轮产物哈希后才生成 schema 2 回执，不会仅凭旁边的二进制文件或一个 passed 字段放行。回执拒绝覆盖；实验使用新的 attempt，保留原证据。

```sh
python3 tools/ike-validation/runtime/prepare_coexist.py bind /absolute/new-unbound-bundle \
  --snapshot /absolute/fresh-public-identities.json --out /absolute/new-bound-bundle
```

`bind` 也只生成本地文件；还须按当轮授权、日志稳定性和恢复条件逐张审阅并执行，不能因为生成了请求卡就自动连接模块。

## 9. 本轮电脑验收结果

见 [公开验收摘要](runtime/coexist-preparation-result-2026-11-14.json)。最终二进制的 20 项 ARM 回归与 7 项共存实验通过；正常共存 30.009 秒，10 对 ping/HTTP、清理后短信身份及 HTTP 检查通过。CPU 44 项通过，另 1 项“活进程阻止 90 清理卡”测试因 amd64 binfmt 容器的 `/proc/exe` 指向转译器而跳过；不把 ARM 其他身份测试当成该卡的单独实测。未知文件拒绝和绑定/哈希/控制文件/HTTP 等测试均已执行。

实验短信是约 60 KiB 的模拟 HTTP worker，不是真实 Go 短信服务；不能用实验可用内存或其 RSS 推算模块共存余量。电脑已生成 `IKE20261114COEX_PREP02` 未绑定包，默认 12 MiB 准入、无共存启动卡。本轮没有上传、运行或读取模块，也没有变更 RouterOS。
