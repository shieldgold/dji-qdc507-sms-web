# 完整短信服务与 charon 的私有链路组合实验

这里是独立于已封存单机夹具的电脑实验。两台通用 ARM/Linux 3.18.44 QEMU 仅通过主机 `127.0.0.1` 的 socket 成对互联，没有用户网络、WAN、USB 或主机设备挂载。构建容器继续禁网，只写私有输出和独占 `/work/sms-combo-root.*` 临时目录。

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s tools/sms-validation/combined -p 'test_*.py'
PYTHONDONTWRITEBYTECODE=1 python3 tools/sms-validation/combined/run.py \
  /absolute/ike-validation-local --binary /absolute/smsweb-armv7 \
  --label before --attempt 1 --pki /private/new-empty-combo-pki
```

`--pki` 首次必须是不存在的新目录；工具只在该目录生成有效期 7 天、RSA 2048 的合成 CA、`client.lab`、`server.lab` 实验密钥与证书。配对 before/after 使用同一目录。已有目录必须有本工具生成的标记且证书仍有效；不会覆盖失效或来源不明的目录。所有密钥、initramfs 和原始日志都留在私有目录，不进入 Git。

服务端使用原 IKE 算法及 16 threads 配置。客户端直接复用既有 `coexist-build/qdc-ike-guard` 二进制，仍是 8 MiB 启动、6 MiB 绝对下限、相对基线下降不超过 3 MiB，以及原 PID/start/exe、独立恢复守护、时钟先恢复和 30 秒/10 组加密 ping + HTTP 机制。仅建立指定 /32 路由，不改默认路由或 DNS。既有 `ike-validation` / `tun-validation` 源码和产物不被修改。

与旧模拟短信服务的区别：`/data/smsweb/smsweb` 是完整生产 ARM 二进制；一个持久 raw PTY 合成基带贯穿真实监管进程暂停/短信进程退出/恢复。首次运行注册合成账号；在 `COEXIST_READY` 后重新登录、读取合成长短信、发送一条合成短信并重复相同请求 ID 校验去重，再读取 16 个完整静态资源响应。最终要求至少 10 次真实后台短信轮询、恰好一次合成提交。辅助 HTTP 客户端验证实际正文与 Content-Length 一致。

组合窗口内保留完整短信进程和 charon 的 smaps。原 guard 继续按自己的采样记录进程内存、系统可用内存和停止原因；成功必须同时有完整 30 秒、10 组流量、短信功能、恢复顺序、VPN 清理和地址/路由恢复证据。`STOP memory` 属于停止条件触发，不能算组合成功，也不能通过修改阈值或算法重标为成功。

输出在 `sms-opt-build/combined-logs/<label>-<attempt>/`。通用 VM 的结果不等于真实模块部署或验收。
