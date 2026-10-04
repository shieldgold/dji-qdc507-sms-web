#!/bin/sh
# ============================================================
# 大疆 4G 模块 · 短信网页服务 —— 开机自启脚本
#
# 由 /etc/init.d/rcS.d/S99smsweb 调用（自启钩子是一个指向本文件的符号链接）。
# 本脚本本身不写任何闪存，二进制和日志都放在 /data/smsweb 下。
# ============================================================

D=/data/smsweb
LOG=$D/boot.log

log() { echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG"; }

log "---- 开机自启开始 ----"

if [ ! -x "$D/smsweb" ]; then
    log "错误：找不到可执行文件 $D/smsweb"
    exit 0
fi

# 等 AT 通道就绪（内核通常在启动早期就建好 /dev/smd9）
i=0
while [ ! -c /dev/smd9 ] && [ $i -lt 30 ]; do
    sleep 1
    i=$((i + 1))
done
if [ ! -c /dev/smd9 ]; then
    log "警告：/dev/smd9 尚未出现，仍然尝试启动"
fi

# 清掉可能残留的旧实例
OLD=$(pidof smsweb)
if [ -n "$OLD" ]; then
    log "清理旧进程 $OLD"
    kill $OLD 2>/dev/null
    sleep 1
fi

start-stop-daemon -S -b -x "$D/smsweb" -- -at /dev/smd9 -port 8787

sleep 2
log "启动完成 pid=$(pidof smsweb)"
exit 0
