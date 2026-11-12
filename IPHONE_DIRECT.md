# iPhone 直连部署

此分支以 `Inceptzws/dji-qdc507-sms-web` 的 `0b5e337` 为基础，补充 SIM PIN 解锁、USB 地址监听、服务守护与可保留数据的回滚。部署目标是 Baiwang QDC507，固件 QDC507GLEFM21，Linux 3.18.44 / ARMv7。未刷写固件。

## 使用

1. 用支持数据传输的 USB-C 线连接模块和 iPhone 15 Pro，等待约 40–90 秒。
2. 在 Safari 打开 **http://192.168.225.1:8787/**。这个页面通过 USB 本地网络访问，SIM 未解锁时也能打开。
3. 首次设置网页访问密码。它与 SIM PIN 是两个不同的密码；访问密码只以哈希形式保存在模块。
4. 登录后，若显示 SIM PIN 锁定，输入正确 PIN，点击「解锁一次」。每次重新上电都可能需要此步骤。
5. 等待网络注册，查看短信列表。可通过 Safari「分享 → 添加到主屏幕」保存入口。

PIN 不保存在模块。失败或结果不明时，保护记录会跨服务重启保留，阻止自动重复提交。必须先核实正确 PIN，再手动授权一次重试。SIM PUK、PIN2 或未知状态均不会提交 PIN。网络中断时先刷新状态，不要连续点击。

验收时，先关闭 iPhone 的 Wi-Fi 与自身蜂窝数据，确认网页能打开，再确认外部 HTTPS 网页可访问。随后分别验证新短信接收与发出短信的接收端实收。页面上的「已提交」仅表示模块接受了发送请求，不能代替送达确认。短信在此网页管理，不会进入 iPhone 系统「信息」；本部署未提供电话音频。

## 已验证及待验证

2026-11-12 在目标模块上验证：

- 静态 ARMv7 程序上传后 SHA-256 与本机构建一致。
- ECM 模式为 1，Mac 从模块取得 `192.168.225.0/24` 地址；模块网页只监听 `192.168.225.1:8787`。
- Mac DJOneHub 服务停止后，模块自己读取已有短信。
- 系统重启后 boot ID 改变，开机钩子自动拉起服务。USB IP 尚未就绪时守护脚本会重新启动服务。
- 重启后的 SIM 为 `SIM PIN`，仍能识别 ICCID，并打开首次设置网页。随后用户完成了网页密码设置和 PIN 解锁，接口读回 `READY`。
- 绑定模块 USB 网卡访问外部 HTTPS 测试页返回 HTTP 200，内容核对通过。
- 模块服务的停止、移除自启钩子、保留短信与重新启用均已实测。
- Go 单元测试、竞态检查、PIN 前端流程测试和 shell 语法检查通过。

**仍需用户接 iPhone 15 Pro 验证**：手机供电与网卡识别、真正断电后再上电、iPhone 上的 PIN 解锁和 4G HTTPS 上网，以及新短信实际收发。系统重启验证不等同于物理断电验证。

## 构建与维护

使用 Go 1.21–1.23 构建；此次使用官方 Go 1.23.12，`CGO_ENABLED=0 GOOS=linux GOARCH=arm GOARM=7`。较新 Go 在此旧内核上的兼容性未经验证。

```sh
go test -race ./...
go vet ./...
node scripts/tests/pin-ui.cjs
sh -n deploy/start.sh deploy/install.sh deploy/rollback-module.sh
./build.sh
```

模块文件都位于 `/data/smsweb`；唯一系统目录改动是 `/etc/rcS.d/S99smsweb -> /data/smsweb/start.sh`。修改后根分区重新挂载为只读。`start.sh` 保持单一守护进程，服务退出后重试；`stop` 会停止守护进程与其子进程。

标准 ADB 可用的电脑可执行 `./deploy/install.sh --autostart`。此次 macOS 的标准 ADB 未枚举出模块，因此用了 [CellBridge](https://github.com/menglilongxia/dji-cellular-bridge) 的 IOKit ADB 工具 `cmd/qdc507-adb`（源码版本 `75a1def8cd9683b43848d9c3d0a15c08dd8d49d3`），保留了本地恢复工具。部署没有安装 CellBridge 的语音组件。

## 回滚

仅停用模块网页，保留短信、密码设置和 ECM 网络：

```sh
adb shell sh /data/smsweb/rollback-module.sh
```

若用 IOKit 工具：

```sh
qdc507-adb -timeout 20s shell 'sh /data/smsweb/rollback-module.sh'
```

脚本核实钩子属于本服务后才移除，停掉守护进程及服务，不删除 `/data/smsweb`。

完整恢复到部署前的 Mac 模式，应使用本次本地备份内的 `恢复Mac模式.command`：先停止模块网页，再恢复 Mac 服务、原 USB 组合和原 USB 网络模式。每台设备必须使用它自己的配置备份，不能套用其他型号的 VID/PID 或刷入其他固件。该完整配置恢复脚本已保存和检查，未执行，以保留当前 iPhone 部署。

备份不进入 Git：配置和短信快照 `before.json`、原 `/data` 与启动目录快照 `module-before.tgz`、Mac 启动配置、IOKit 恢复工具及当前部署快照均保存在用户本机。备份含私有短信与 SIM 标识，勿上传到公开仓库。
