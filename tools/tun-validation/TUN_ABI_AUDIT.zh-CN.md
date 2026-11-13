# QDC507 TUN 关键 ABI 离线交叉核验

2026-11-13。**已覆盖的关键字段、回调槽位和调用路径未发现明确 ABI 冲突；证据支持继续准备受控临时加载验证，但尚不能把候选驱动标记为设备可用。** 本轮只操作电脑上的既有镜像、源码、工具链和分析文件，没有访问模块、上传或加载驱动，也没有修改设备配置或固件。

## 输入与检查范围

- 原厂 ARM Image SHA-256：`62daa067512597e59b952f45be8cd5b6d34fc75265d0f60c5a3b8b8f09ecea2a`。
- 候选 GCC 4.9.3 TUN SHA-256：`2b44764dc5bee73faab5671bc5575cb08cb08c8cb68854fb8ef1a85b5ca8637d`，本轮未重建或改变交付驱动。
- 候选源码仍为 Quectel 3.18.44 提交 `82ed00908b3e8efc3ff0de27d2b5a7c0524ecd7f`，目标配置、编译器来源和差异沿用 [构建报告](README.zh-CN.md)。未取得完整 QDC507 原厂源码。
- 使用同一套 GCC 4.9.3、实际 TUN 编译命令及头文件，额外编译调试对象和 **21 个参考编译单元**。只为提取布局加入 `-g` 等调试选项；校验调试对象与原 `tun.o` 的所有分配节字节完全相同，避免把另一个探针的布局误当成实际驱动布局。
- 原厂无 KALLSYMS、无 MODVERSIONS、无原厂 DWARF。入口来自此前恢复的 6,144 项导出表；另外恢复了从 `0xc05a0ef0` 开始的 17,720 项 ARM exidx 连续表，辅助限制函数检查范围。exidx 会合并相邻同类展开信息，不能把到下一项的跨度直接当作精确函数长度。

本轮检查 **82 个函数**：68 个是候选 TUN 的导入函数，14 个是辅助核验函数。其中 5 个未导出辅助函数由已定位调用者的直接分支、候选对象符号和对应机器码关系归属，包括 `sk_prot_alloc`、`__dev_queue_xmit`、`__dev_open`、`__netdev_update_features`、`dev_hard_start_xmit`。这是二进制归属分析，不声称找到了原厂原始源码。

其余 33 个 TUN 导入符号本轮没有做同等深度的函数体对照，包含 `init_net`、`jiffies`、内存分配、用户缓冲区复制、RCU、权限和日志设施等。它们已完成此前的导出存在性检查，不能据此称所有 101 项 ABI 均已证明。

## 函数体与调用方式

### 链接地址归一化

按候选 ELF 的实际重定位记录，只屏蔽 ARM B/BL 的链接目标位和 ABS32 地址字；保留操作码、寄存器、分支条件、结构访问偏移和非地址常量。**29 个函数的整个候选长度范围因此逐字节一致**，包括：

`free_netdev`、`netif_tx_wake_queue`、`kfree_skb`、`skb_queue_tail`、`skb_queue_purge`、`sk_free`、`sk_detach_filter`、`ether_setup`、`sock_from_file`、`init_timer_key`、`__init_waitqueue_head`、`__rtnl_link_register` 等。

这些相同函数中的 **28 处已知导出地址引用与原厂导出表一致**，没有发现目标冲突；另有 15 处本地辅助目标或数据指针未在这一检查中独立证明身份，因此不能把“地址归一化后相同”描述成无条件的完整语义等价。

其余 **53 个函数存在机器码差异**。已抽查的差异包括寄存器分配、栈保存和指令安排，例如 `skb_put`、`sock_init_data`、`alloc_netdev_mqs`。没有把这些差异自动归类为 ABI 冲突，也没有逐一证明全部差异都只是编译器造成。

### 参数与回调实例

| 接口 | 原厂指令与候选对照 |
| --- | --- |
| `alloc_netdev_mqs` | 前四参数走 r0–r3；保存 32 字节寄存器后，从 `sp+32`、`sp+36` 读取两个队列参数，与候选六参数调用形式相符 |
| `sk_alloc` → `sk_prot_alloc` | 原厂将第四参数 `proto *` 从 r3 转到内部调用的 r0；内部读取 slab、obj_size、owner 的偏移与候选一致 |
| `sock_init_data` | socket 和 sock 两个参数分别建立 `socket.sk` 与 `sock.sk_socket` 关联，偏移分别为 20 和 376 |
| `ndo_start_xmit` | 原厂 `dev_hard_start_xmit` 从设备偏移 272 读取操作表，再从表偏移 16 读取回调，调用时 r0 为 skb、r1 为设备，与 TUN 回调形式相符 |
| `proto_ops` | 原厂 socket 分派代码读取 owner/release/sendmsg/recvmsg 的槽位分别为 4/8/56/60 |
| `file_operations` | `do_sync_read/write` 从文件偏移 20 读取 f_op，再从表偏移 16/20 读取 aio_read/aio_write；QRAWDATA 的 ioctl/open/release 旁证见 [上一报告](QRAWDATA_ABI_ANALYSIS.zh-CN.md) |

以上是具体机器访问与调用寄存器的旁证。原厂没有类型 CRC，不能由这些实例恢复全部函数的 C 类型、限定符和所有间接回调契约。

## 共享对象字段对照

采用有界参数来源跟踪：跟踪参数寄存器、可保留的别名、简单地址运算、条件路径及符合固定形式的有界跳转表；调用按 AAPCS 清除调用者保存寄存器，遇到未知来源即停止推断。比较得到 **484 组共同访问模式**。其中按候选 DWARF 和对应函数角色，可归属为 **277 组“类型、字节偏移、访问指令”证据，涉及 188 个不同的类型/偏移位置**。

这些计数不是“277 个独立字段完全证明”：同一位置可能分别被读写，联合体和位域也可能共用位置。机器可读结果保留了这些候选字段名称歧义和原厂指令地址。

| 对象 | 获得原厂旁证的关键偏移，单位为字节 |
| --- | --- |
| `net_device` | features 112、hw_features 120、vlan_features 136、ifindex 160、stats 168、netdev_ops 272、flags 288、priv_flags 292、mtu 304、type 308、hard_header_len 310、addr_len 349、dev_addr 436、收发队列相关 440–504、tx_queue_len 512、pcpu_refcnt 576、reg_state 的字节位置 604、网络命名空间 612、rtnl_link_ops 1012 |
| `sk_buff` | sk 16、dev 20、cb 24、len 84、data_len 88、checksum 字节 101、csum_start 108、csum_offset 110、hash 120、protocol 144、头部位置 146/148/150、tail 152、end 156、head 160、data 164、truesize 168、users 172 |
| `sock` | 网络命名空间 36、引用计数 84、接收队列 100、filter 152、wq 156、flags 160、发送内存计数 172、sndbuf 180、shutdown 字节 196、错误队列 252、socket 376、data_ready 420、write_space 424、destruct 436 |
| `socket` | type 4、flags 8、wq 12、file 16、sk 20、ops 24 |
| `file` | f_op 20、private_data 120 |
| `proto` | clear_sk 80、slab 128、obj_size 132、owner 156 |
| `net_device_ops` | init 0、uninit 4、open 8、start_xmit 16、set_mac_address 32、validate_addr 36、change_mtu 48、fix_features 128、set_features 132 |
| `rtnl_link_ops` | kind 8、setup 16、dellink 40 |
| `skb_shared_info` | nr_frags 0、tx_flags 1、gso_size 2、gso_segs 4、gso_type 6、frag_list 8、ip6_frag_id 28、destructor_arg 36、frags 40 |
| 定时器、misc、等待队列 | 定时器 expires/base/function 为 8/12/16；misc 的 minor/name/list/parent/this_device 为 0/4/12/20/24；等待队列链表为 0/4 |

### 分配边界、命名空间和位域

另外对 9 项关键常量/访问做固定指令核对：

- **`sizeof(net_device)=1232`**：原厂 `alloc_netdev_mqs` 的无私有区分配常量为 `0x4d0`，与候选 DWARF 一致。
- **私有区对齐位置为 1248**：原厂带私有区的分配加量为 `0x4e0`，与候选 `ALIGN(1232,32)` 一致。这直接关系到 TUN 的 `netdev_priv()` 指针位置。
- **stats 在偏移 168**：原厂 `dev_get_stats` 的无专用统计回调路径使用设备地址加 `0xa8`。
- **文件 private_data 在 120**：原厂 `sock_from_file` 在确认 f_op 后从 `0x78` 取私有指针。
- **current → 网络命名空间链**：原厂 `sock_create` 依次读取 `thread_info.task=12`、`task_struct.nsproxy=736`、`nsproxy.net_ns=20`，与 TUN 使用的候选头文件一致。
- **checksum 位域**：候选 `ip_summed` 在字节 101 的 bit 1–2；原厂 `skb_partial_csum_set` 从字节 `0x65` 取值并 OR `0x06`，与将其设为 `CHECKSUM_PARTIAL` 的编码一致。

这里没有把“最后一个已观察成员的末端”当作原厂结构总大小。候选 `sk_buff=176`、`sock=440`、`socket=28`、`skb_shared_info=176` 等仍只是候选类型大小；已核对它们的一批关键访问位置，**尚未分别取得原厂完整大小的独立证明**。

## 仍未闭合的范围

1. **完整布局与未经过的回调。** `net_device` 总大小已获得分配指令旁证，但其他类型并非全部成员、尾部及位掩码都已证明。例如 `ndo_stop`、`ndo_select_queue`、部分 rtnetlink/ethtool 槽位没有在本轮取得同等强度的逐项证据。
2. **参数跟踪不是完整程序证明。** 它不模拟任意算术、寄存器经栈保存和恢复后的来源、所有循环状态或间接被调用函数。即使本轮已接纳范围内没有遗留计算跳转，也不代表已解析整个网络栈调用图。
3. **4 组访问未被自动配对。** 两组在 `zerocopy_sg_from_iovec` 的 iovec 访问，两组在 `skb_copy_ubufs` 的分片半字访问。人工对照中，原厂分别采用先移动指针再以负偏移访问、以及栈保存/不同寄存器取地址；尚未把该观察提升为分析器的完整等价证明。当前标为分析覆盖缺口，不标为字段冲突。
4. **代码与工具链差异。** 53 个函数的全部语义差异未逐一证明；原厂 GCC 具体补丁集、厂商完整源码以及模块类型 CRC 均缺失。
5. **运行行为。** 驱动加载、模块初始化、TUN 创建、收发、内存压力和长时间稳定性均未验证。离线结果不能替代这些检查。

## 阶段结论与下一阶段边界

此次比导出名称和 vermagic 检查更深入：确认了 `netdev_priv()` 这一分配边界，多个实际网络对象访问位置、checksum 位域、socket/网络回调槽位和 current 命名空间取址链。**没有发现需要立即放弃当前候选驱动的明确 ABI 冲突。**

在这些证据基础上，可以继续准备设备端受控临时加载方案。设备端执行仍属于后续单独授权阶段：需要具备日志采集与恢复手段，再验证加载、创建单队列 TUN、基本收发和卸载清理；内核模块即使从内存加载也可能导致崩溃或重启，不能把它视为完全无风险操作。当前不执行这些步骤，也不设置持久安装或开机加载。

## 复现、验证与留档

分析工具沿用上一轮的 Capstone 5.0.9、pyelftools 0.33，依赖仅安装到任务目录。REA 的现有 raw-binary 路由不支持已安装的 Hopper，Ghidra 未配置；沿用已记录的限制，本轮使用 ELF/DWARF 和 Capstone 分析，没有修改 REA/客户端配置，也不将结果称为成功的 REA 反编译。

```sh
python3 -m pip install --target /Users/shield/Documents/ChatGPT/DJI-4G/tun-validation-local/qrawdata-analysis/python-deps -r tools/tun-validation/qrawdata-requirements.txt

tools/tun-validation/tun_abi_build.sh /Users/shield/Documents/ChatGPT/DJI-4G/tun-validation-local

python3 tools/tun-validation/tun_abi_inspect.py /Users/shield/Documents/ChatGPT/DJI-4G/tun-validation-local --deps /Users/shield/Documents/ChatGPT/DJI-4G/tun-validation-local/qrawdata-analysis/python-deps

PYTHONPATH=/Users/shield/Documents/ChatGPT/DJI-4G/tun-validation-local/qrawdata-analysis/python-deps python3 -m unittest discover -s tools/tun-validation -p 'test_*.py'
```

**38 项单元测试通过。** 新增测试覆盖地址归一化时保留字段差异、未知重定位拒绝、高位 ARM 分支地址、调用者寄存器失效、条件合并、位域存储字节归属和有界跳转表等。原 TUN 交付文件的 SHA-256 未变。

机器可读结果见 [abi-result.json](abi-result.json)，目标清单见 [abi-targets.json](abi-targets.json)。电脑的 `abi-analysis/` 另存参考对象、完整候选布局、逐函数配对位置和探索性反汇编；原厂镜像、设备数据和工具依赖不提交到 Git。
