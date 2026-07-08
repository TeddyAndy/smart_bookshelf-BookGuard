#!/bin/sh
# Smart bookshelf runtime supervisor for the board.

FSM=/root/fsm_main
UI=/root/SmartBookshelf
CH341=/root/ch341.ko
STOP_FSM=/root/stop_fsm.sh
LOG=/tmp/bookshelf_boot.log
SUP_PID=/var/run/bookshelf_supervisor.pid
FSM_LOG=/tmp/fsm_main.log
UI_LOG=/tmp/ui.log
STATE=/tmp/fsm_state

log()
{
    echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG"
}

backlight_on()
{
    for p in /sys/class/backlight/backlight/bl_power /sys/class/backlight/backlight/brightness; do
        [ -e "$p" ] || continue
        case "$p" in
            *bl_power) echo 0 > "$p" 2>/dev/null ;;
            *brightness) echo 255 > "$p" 2>/dev/null ;;
        esac
    done
}

load_ch341()
{
    if grep -q '^ch341 ' /proc/modules 2>/dev/null; then
        log "CH341 already loaded"
        return 0
    fi
    if [ ! -f "$CH341" ]; then
        log "ERROR: missing $CH341"
        return 1
    fi
    insmod "$CH341" >> "$LOG" 2>&1 || true
    sleep 1
    if grep -q '^ch341 ' /proc/modules 2>/dev/null; then
        log "CH341 loaded"
        return 0
    fi
    log "ERROR: CH341 load failed"
    return 1
}

start_fsm()
{
    if pidof fsm_main >/dev/null 2>&1; then
        log "fsm_main already running: $(pidof fsm_main)"
        return 0
    fi
    if [ ! -x "$FSM" ]; then
        log "ERROR: missing executable $FSM"
        return 1
    fi
    rm -f "$STATE"
    log "starting fsm_main"
    nohup "$FSM" > "$FSM_LOG" 2>&1 &
    sleep 1
    if pidof fsm_main >/dev/null 2>&1; then
        log "fsm_main started: $(pidof fsm_main)"
        return 0
    fi
    log "ERROR: fsm_main failed to start"
    tail -40 "$FSM_LOG" >> "$LOG" 2>/dev/null
    return 1
}

start_ui()
{
    if pidof SmartBookshelf >/dev/null 2>&1; then
        log "SmartBookshelf already running: $(pidof SmartBookshelf)"
        backlight_on
        return 0
    fi
    if [ ! -x "$UI" ]; then
        log "ERROR: missing executable $UI"
        return 1
    fi
    log "starting SmartBookshelf"
    nohup "$UI" > "$UI_LOG" 2>&1 &
    sleep 1
    backlight_on
    if pidof SmartBookshelf >/dev/null 2>&1; then
        log "SmartBookshelf started: $(pidof SmartBookshelf)"
        return 0
    fi
    log "ERROR: SmartBookshelf failed to start"
    tail -40 "$UI_LOG" >> "$LOG" 2>/dev/null
    return 1
}

stop_all()
{
    log "stopping bookshelf runtime"
    kill "$(cat "$SUP_PID" 2>/dev/null)" 2>/dev/null || true
    rm -f "$SUP_PID"
    killall SmartBookshelf 2>/dev/null || true
    if [ -x "$STOP_FSM" ]; then
        sh "$STOP_FSM" >> "$LOG" 2>&1
    else
        killall fsm_main 2>/dev/null || true
    fi
    i=0
    while pidof fsm_main >/dev/null 2>&1 && [ "$i" -lt 10 ]; do
        sleep 1
        i=$((i + 1))
    done
    if pidof fsm_main >/dev/null 2>&1; then
        log "WARN: fsm_main still alive after graceful stop; killing"
        killall -9 fsm_main 2>/dev/null || true
    fi
}

monitor_loop()
{
    log "supervisor started"
    load_ch341
    start_fsm
    start_ui

    while true; do
        sleep 5
        if ! pidof fsm_main >/dev/null 2>&1; then
            log "WARN: fsm_main is down; restarting"
            load_ch341
            start_fsm
        fi
        if ! pidof SmartBookshelf >/dev/null 2>&1; then
            log "WARN: SmartBookshelf is down; restarting"
            start_ui
        fi
        backlight_on
    done
}

case "$1" in
    start)
        if [ -f "$SUP_PID" ] && kill -0 "$(cat "$SUP_PID" 2>/dev/null)" 2>/dev/null; then
            log "supervisor already running: $(cat "$SUP_PID")"
            exit 0
        fi
        monitor_loop >/dev/null 2>&1 &
        echo "$!" > "$SUP_PID"
        ;;
    stop)
        stop_all
        ;;
    restart)
        stop_all
        sleep 2
        monitor_loop >/dev/null 2>&1 &
        echo "$!" > "$SUP_PID"
        ;;
    status)
        echo "supervisor=$(cat "$SUP_PID" 2>/dev/null)"
        echo "fsm=$(pidof fsm_main 2>/dev/null)"
        echo "ui=$(pidof SmartBookshelf 2>/dev/null)"
        ;;
    *)
        echo "Usage: $0 {start|stop|restart|status}"
        exit 1
        ;;
esac
