# iOS 后台短信与来电通知（0.3.0）

模块直接通过 HTTP/2 + TLS 连接苹果 APNs，通知送达 iPhone。没有 Cloudflare 或其他转发服务，日常使用不依赖 Mac。短信使用普通 alert 推送；真实响铃的来电使用 VoIP 推送，App 立即向 CallKit 报告。后台不靠轮询或播放静音保活。

## 使用

1. 模块持续供电、SIM 已解锁且能上网；iPhone 也需联网。接听和音频还需要 iPhone 与模块保持本地 USB 网络连接。
2. 在“模块电话”连接模块后，打开“通知”，允许系统通知并开启“新短信通知”“后台来电提醒”。每个模块目前登记一台 iPhone。
3. 查看登记状态，点“测试后台推送”，随即锁屏。模块约 8 秒后发送测试，使用真实短信通知通道，不产生短信费用。“仅测试本机横幅”不能代替后台验收。
4. 用另一部手机拨入模块 SIM，确认锁屏出现系统接听界面，可接听、双向通话和挂断。测试推送只使用普通通知，不伪造 VoIP 来电。

短信通知不含号码、正文或验证码，只提示新增数量。点击进入原短信页。首次登记不提醒历史短信；模块保存去重标识和待发队列，重启不会重新生成已记录的通知。通知是尽力送达，断网、专注模式及系统设置可能影响显示，短信原文以模块收件箱为准。

## 绑定与失败处理

- 登记需当前 SIM 的登录状态，实时核对 ICCID；发送前再次核对。观察到换卡会删除旧订阅和队列，换回原卡不会恢复旧登记。
- App 启动时向 Apple 登记，令牌变化后更新模块。模块在权限 0600 的 `push-state.json` 保存令牌与去重标识。通知负载只含类别、数量、事件标识及必要的通话标识；不含 ICCID、密码、PIN 或短信内容。
- 模块保存专用于本 App 开发环境的 APNs 私钥，固定只连接苹果端点。私钥、配置不进入 App、日志或 Git；配置文件和私钥必须为受限权限。此前试建的 Cloudflare 服务已移除。
- ES256 发送凭据缓存 50 分钟。TLS 不跳过证书校验，禁用 HTTP 重定向；APNs 响应必须使用 HTTP/2。失效设备令牌停止当前事件重试，临时网络错误退避重试，最多发送 5 次。
- 短信最长排队 5 分钟，来电最长 20 秒；已结束的来电从队列移除，APNs 不保存离线 VoIP 呼叫。重试沿用事件 UUID 和 collapse ID，不能据此保证所有网络故障下“恰好送达一次”。
- 来电唤醒时先立即报告 CallKit，再用已有登录 Cookie 查询对应电话。已结束、模块不可达或登录失效时关闭来电界面；不会在锁屏时读取保存的 PIN 或密码。iPhone 重启后应先解锁一次，并连接模块。
- 模块系统时间不可靠时使用 App 校准，保存时间估计供重启使用。通过 TLS 验证的 Apple 响应 Date 头继续校准；不修改系统时钟。长期断电导致证书时间无法验证时需重新打开 App。
- 本固件 DNS 监听 `192.168.225.1:53`，排除 loopback，`resolv.conf` 可能为空。推送客户端使用此 DNS，不改系统 DNS、USB 模式或其他路由。
- App 仅在模块已配置且本机短信订阅已开启时停用本地短信横幅，避免重复提醒；未配置时保留前台提醒。

## 部署

新增接口均要求登录：

- `GET /api/push/status`：配置、登记、队列长度、苹果已接收次数与错误。苹果接收不等于手机已显示。
- `POST /api/push/register`：本机标识、普通与 VoIP 令牌、两个开关、`expected_iccid`、`environment=development`。
- `POST /api/push/test`：延迟 8 秒的普通测试推送。

模块 `/data/smsweb/push-apns.json`：

```json
{"key_id":"APPLEKEYID","team_id":"APPLETEAM1","topic":"com.shield.modulephone","environment":"development"}
```

同目录 `push-apns.p8` 为 Apple P-256 PKCS#8 私钥，两文件权限 0600；`push-ca.pem` 只含公共 CA 证书。服务启动时读取配置，更改后在无通话时重启服务。旧 `push-relay.json` 不再读取。

当前 App 签名和专用密钥都是开发环境，唯一推送主机为 `api.sandbox.push.apple.com`。使用 `www.apple.com/library/test/success.html` 的 TLS 响应校准时间。正式分发时必须同步调整签名、密钥权限、登记环境检查和固定苹果端点，不可只改配置。

## 验证记录与回退（2026-11-13）

- Go 竞态测试与 vet 通过；覆盖消息去重、重启、开关、换卡、令牌更新、过期来电、损坏文件保留、HTTP/2、TLS、拒绝重定向、真实 ES256 签名、凭据缓存、时钟回退和 APNs 错误分类。
- 模块通过蜂窝网直连 Apple，未带凭据的探针获得 HTTP/2 `403 MissingProviderToken`；验证了协议与网络，不代表通知送达。探针峰值 RSS 5216 KB，退出后可用内存约 9 MB。
- 0.3.0 App 已签名安装至 iPhone 15 Pro。iOS 单元、Core 和通知界面测试通过；真实麦克风测试在模拟器跳过。
- 用户已明确允许将专用推送私钥写入模块私有目录，两个文件均为 0600，传输校验通过。原 Cloudflare 推送及探针服务已删除。
- 使用同一生产发送器和模块内专用密钥的直连检查通过：苹果只拒绝虚拟设备令牌；未向真实手机发送此探针。签名探针峰值 RSS 5872 KB，退出后模块服务 RSS 约 5 MB、可用内存约 8.5 MB。
- 用户确认 iPhone 首页连接正常、通知登记成功，并在锁屏后收到“模块电话 · 后台推送测试”。模块 → 苹果 APNs → 真实 iPhone 的后台普通推送已验收。
- 用户随后确认：真实新短信在锁屏时显示通知；真实来电显示系统界面，接听后双方有声。普通短信与 PushKit → CallKit 的后台触发、接听及双向声音均已完成真实手机验收。
- 当前已部署模块程序 SHA-256：`6aa699b3baf364945de08e8a1835b7eb3e8258feee7e8920143ddaf7be28bdb8`，已从模块读回比对。App 为 0.3.0（3）。

原模块程序保留于 `/data/smsweb/smsweb.before-push-20261113`。回退时先关闭 App 通知开关，在无通话时停止服务，恢复旧程序并重启；无需改动 SIM、短信、网络或语音运行时。通知私有文件可保留，旧程序不读取它们。

参考：[Apple 推送请求](https://developer.apple.com/documentation/usernotifications/sending-notification-requests-to-apns)、[发送凭据与连接](https://developer.apple.com/documentation/usernotifications/establishing-a-token-based-connection-to-apns)、[PushKit 来电处理](https://developer.apple.com/documentation/pushkit/responding-to-voip-notifications-from-pushkit)。
