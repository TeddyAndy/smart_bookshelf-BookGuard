#!/bin/sh
# 安全停止 fsm_main — 先优雅退出, 超时再强杀+盲停UHF+关灯
# 用法: ssh root@192.168.0.232 "sh /root/stop_fsm.sh"

STOP_FRAME='\x52\x46\x00\x00\x00\x23\x00\x00\x45'
LED_OFF='/root/led_off'

echo "[stop] 发送 SIGTERM..."
killall fsm_main 2>/dev/null

# 等最多 5 秒让 fsm_main 自己清理
for i in 1 2 3 4 5; do
    sleep 1
    if ! pidof fsm_main >/dev/null 2>&1; then
        echo "[stop] fsm_main 已退出"
        break
    fi
done

# 如果还在, 强杀
if pidof fsm_main >/dev/null 2>&1; then
    echo "[stop] 超时, 强杀 + 盲停 UHF"
    killall -9 fsm_main 2>/dev/null

    # 盲停 UHF 两个模块
    for dev in \
        /dev/serial/by-path/platform-ff780000.usb-usb-0:1.4:1.0-port0 \
        /dev/serial/by-path/platform-ff780000.usb-usb-0:1.1:1.0-port0 \
        /dev/ttyUSB0 /dev/ttyUSB1 /dev/ttyUSB2 /dev/ttyUSB3; do
        if [ -e "$dev" ]; then
            stty -F "$dev" 115200 cs8 -cstopb -parenb raw 2>/dev/null
            printf "$STOP_FRAME" > "$dev"
            echo "[stop] 盲停 $dev"
        fi
    done
fi

# 关灯 (如果有 led_off 工具)
if [ -x "$LED_OFF" ]; then
    $LED_OFF
    echo "[stop] 灯带已关"
fi

echo "[stop] 完成"
