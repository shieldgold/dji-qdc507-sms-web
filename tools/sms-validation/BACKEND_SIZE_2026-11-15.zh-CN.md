# 前端实现与主程序进一步瘦身评估

当前前端为手写原生 HTML、CSS、JavaScript，没有 Vue、React、Angular 或 jQuery。四个资源直接由 Go embed 嵌入；DOM、事件、短信页面状态和 fetch 请求在手机/电脑浏览器执行。模块上运行 Go HTTP/API、AT、推送及通话控制服务，没有 Node.js 或前端框架运行时。Node/jsdom/esbuild 仅用于电脑测试或实验。

本轮只读取源码、在电脑编译和运行隔离 ARM 实验；生产源码、已验收候选和模块文件均未改。全部虚拟机已退出。

## 5550232 B 主程序的构成

固定 Go 1.23.12、ARMv7、CGO0、memory 配置。对相同源码额外保留 ELF 符号，按函数 text 符号归属统计，并直接读取已验收二进制 section：

| 内容 | 字节数 | 约 MiB |
|---|---:|---:|
| `.text` 机器代码 | 3094916 | 2.95 |
| `.gopclntab` 运行时函数/调用栈相关元数据 | 1468992 | 1.40 |
| `.rodata` 只读数据 | 692650 | 0.66 |
| `.noptrdata` | 248920 | 0.24 |
| `.data` | 27968 | 0.027 |

另有类型/接口表、build info、对齐等。BSS 虚拟大小不计为同量文件字节。应用自身 `main.*` 函数 text 约 181488 B，约 177 KiB；该数不含其数据及共同依赖，不能把程序理解为只有这部分才有用。

主要依赖包括 Go runtime、网络、TLS/X.509、椭圆曲线、JSON/反射、通用正则等。函数 text 分类不能直接相加当作可以删除的包大小，依赖还共享常量、元数据和接口方法。`-s -w`、4 KiB 布局及关闭内联的 memory 配置已经采用；`.gopclntab` 属于运行时数据，不是能用普通 strip 继续删除的 DWARF。

## 优先候选：用精确规则替换通用正则依赖

`go list -deps` 确认 `regexp` 在当前生产依赖图中仅由 smsweb 自身直接引入。使用位置是验证码提取、请求 ID、拨号号码及 APNs ID/UUID/token 校验，总计 8 个固定正则。

只在电脑私有副本中，以明确标为 NONFUNCTIONAL 的占位校验测量依赖上限：5550232→5337240 B，差 **212992 B，即 208 KiB**。它故意不保留校验功能，**不是优化候选，不执行、不部署**；只是用于定量判断移除通用依赖的潜力。

真正实现应使用字符范围检查及按 UTF-8 rune 计数的验证码扫描，继续保留全部规则语义。尤其验证码的关键词优先、最长 8 个非数字字符、4–8 位贪婪捕获、弱匹配边界、无效 UTF-8 等，须与旧正则做差分回归。等价实现还会加入代码，最终收益需重新编译和 ARM 验证，不能把 208 KiB 宣称为已取得的无损节省。

## UPX：文件显著缩小，但当前共存条件不通过

对已验收 `805aa0ef…` 的电脑副本使用本机 UPX 5.1.1 / LZMA / level 6：

- 文件 5550232→**1453508 B（约 1.39 MiB）**，压缩校验通过。
- 密封 ARM/Linux 3.18.44 单机夹具启动、短信合成流程及压力阶段通过。
- 相同单机 HTTP 阶段，与一个原 memory 样本比较：RSS 6300→6676 KiB，Anonymous **1492→4092 KiB**，增加 **2600 KiB**。另一个未打包原样本 RSS 为 6236、Anonymous 同为 1492 KiB；这是一轮 UPX 探索，不是重复统计或真实 NAND 内存结论。
- 使用最终 eb566 guard、原 16 个 charon 线程和原 8/6/3 MiB 条件做完整组合，**失败**：在短信 HTTP 启动阶段，baseline 26992、available 23664 KiB，下降 **3328 KiB，超过 3072 KiB**。随后恢复健康检查也触发内存条件；日志保留为失败，未改阈值重标通过。

因此 UPX 没有成为交付候选。文件更小不能代替运行时内存及恢复验收。主程序仍使用原已验收的 5550232 B 内存版。

## HTTP/2 不能直接裁掉

同一工具链的本地 `nethttpomithttp2` 诊断编译得到 5234840 B，比现候选少 315392 B，约 **308 KiB**。该产物未运行或部署。

当前模块直接发送苹果 APNs，代码明确启用 HTTP/2。Apple 要求 APNs 使用 HTTP/2 和 TLS 1.2 或更高版本，因此该删减会破坏通知功能，不属于保持现有功能的优化。[Apple APNs 连接要求](https://developer.apple.com/documentation/usernotifications/establishing-a-connection-to-apns)。

## 较大规模的后续方向

若目标明显低于现有约 5.3 MiB，应先做后端依赖替换原型，例如由小型原生 C 组件提供完整 APNs/HTTP2/TLS，或逐步重写后端。必须保留签名、证书与主机名验证、可信时间、错误分类、重试和来电时限，且计算 **Go 程序加原生组件的总大小**。当前没有经过验证的目标体积，不承诺可缩到 1–2 MiB。

将功能拆成多个 Go 可执行程序会重复携带运行库，不能据单个文件变小认定整体变小；若依赖服务器转发，还会改变模块独立运行的条件。仅重写前端不会消除这些后端依赖。

建议顺序：先实现并验证固定正则的等价解析，再根据实际节省量决定是否值得替换 TLS/推送组件。前端精简及历史版本归档见[存储复核](STORAGE_FRONTEND_2026-11-15.zh-CN.md)。

电脑私有证据在 `ike-validation-local/storage-opt-build/backend-size/`；UPX 单机日志在 `sms-opt-build/logs/upx-storage-experiment-ready-1/`，失败共存日志在 `sms-opt-build/newguard-integration-base/sms-opt-build/combined-logs/upx-storage-experiment-1/`。没有真实短信、拨号、设备操作或固件/自启修改。
