# 独立 ARM/Linux 3.18 PTY 回归

此目录不修改单机或组合密封夹具。它在无网、无 USB 的通用 ARM/Linux 3.18.44 虚拟机中运行两个真实 PTY Go 测试：`TestOpenModemCloseReleasesBlockedPTY`、`TestModemSetupFailureReleasesPTYDescriptor`。

应在私有目录分别复制旧 v3 与新 v4 冻结源码，向旧副本加入相同的新 `at_linux_test.go`，用 Go 1.23.12、`CGO_ENABLED=0 GOOS=linux GOARCH=arm GOARM=7`、`go test -c -buildvcs=false -trimpath -ldflags='-s -w -R 4096'` 编译。原冻结源码不被写入。两个测试均只操作访客 `/dev/ptmx` 和 `/dev/pts/*`，检查真实内核 FD 的 `Fstat/EBADF` 和 reader goroutine 退出。

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tools/sms-validation/pty-arm -p 'test_*.py'
PYTHONDONTWRITEBYTECODE=1 python3 tools/sms-validation/pty-arm/run_pty.py \
  /absolute/ike-validation-local --binary /private/old-tests-armv7 --variant old --attempt 1
PYTHONDONTWRITEBYTECODE=1 python3 tools/sms-validation/pty-arm/run_pty.py \
  /absolute/ike-validation-local --binary /private/new-tests-armv7 --variant new --attempt 1
```

旧版必须同时复现两个指定阻塞关闭错误，不能把内核崩溃、测试超时或夹具失败当作 RED。新版必须两个测试都 PASS。私有 `sms-opt-build/pty-arm-logs/` 保留完整测试输出、退出状态及产物/内核/夹具 SHA。容器禁网，只使用独立 `/work/sms-pty-root.*` 临时目录；访客由 `qdc.sms.pty=1` 限定启动。
