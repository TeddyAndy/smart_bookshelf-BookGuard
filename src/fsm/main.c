/**
 * 智能书架 — 主程序 v8
 *
 * 编译: make && make upload
 * 运行: /root/fsm_main [-v|-q] [--no-radar] [--no-wifi] ...
 */
#include "fsm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#define DB_PATH  FSM_DB_LOCAL

static fsm_t    g_fsm;
static db_ctx_t g_db;
static volatile int g_running = 1;
static void on_signal(int s) { (void)s; g_running = 0; }

static void usage(const char *p) {
    printf("Usage: %s [options]\n", p);
    printf("  运行模式:\n");
    printf("    (default)     事件可见\n");
    printf("    -v            全量可视化 (每2s状态)\n");
    printf("    -q            静默 (仅故障)\n");
    printf("  模块开关 (默认全开):\n");
    printf("    --no-led      禁用灯带\n");
    printf("    --no-radar    禁用雷达\n");
    printf("    --no-uhf      禁用 UHF RFID\n");
    printf("    --no-wifi     禁用 WiFi\n");
    printf("    --no-sensor   禁用传感器\n");
    printf("    --no-mqtt     禁用 MQTT\n");
    printf("    --no-screen   禁用屏幕\n");
    printf("    -h            帮助\n");
}

int main(int argc, char *argv[]) {
    int verbose = 1;

    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    fsm_enable_all(&g_fsm);

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-v"))          verbose = 2;
        else if (!strcmp(argv[i], "-q"))          verbose = 0;
        else if (!strcmp(argv[i], "--no-led"))    g_fsm.mod_led.enabled    = 0;
        else if (!strcmp(argv[i], "--no-radar"))  g_fsm.mod_radar.enabled  = 0;
        else if (!strcmp(argv[i], "--no-uhf"))    g_fsm.mod_uhf.enabled    = 0;
        else if (!strcmp(argv[i], "--no-wifi"))   g_fsm.mod_wifi.enabled   = 0;
        else if (!strcmp(argv[i], "--no-sensor")) g_fsm.mod_sensor.enabled = 0;
        else if (!strcmp(argv[i], "--no-mqtt"))   g_fsm.mod_mqtt.enabled   = 0;
        else if (!strcmp(argv[i], "--no-screen")) g_fsm.mod_screen.enabled = 0;
        else if (!strcmp(argv[i], "-h"))          { usage(argv[0]); return 0; }
        else {
            fprintf(stderr, "Unknown: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    printf("\n📚 智能书架 FSM v8 (模块自检)\n");
    printf("   状态: BOOT→自检→基线→ACTIVE↔SLEEP | FIND=黄灯 | tick=%dms\n", FSM_TICK_MS);
    printf("   模块: 灯带%d 雷达%d UHF%d WiFi%d 传感器%d MQTT%d 屏幕%d | %s\n\n",
           g_fsm.mod_led.enabled, g_fsm.mod_radar.enabled,
           g_fsm.mod_uhf.enabled, g_fsm.mod_wifi.enabled,
           g_fsm.mod_sensor.enabled, g_fsm.mod_mqtt.enabled,
           g_fsm.mod_screen.enabled,
           verbose == 0 ? "静默" : verbose == 2 ? "可视化" : "事件");

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);  /* MQTT 断连不杀进程 */

    if (db_init(&g_db, DB_PATH) < 0) {
        fprintf(stderr, "FAIL: db_init\n");
        return 1;
    }

    if (fsm_init(&g_fsm, &g_db) < 0) {
        fprintf(stderr, "FAIL: fsm_init\n");
        db_close(&g_db);
        return 1;
    }
    g_fsm.verbose = verbose;

    while (g_running) {
        fsm_tick(&g_fsm);
        usleep(FSM_TICK_MS * 1000);
    }

    fsm_stop(&g_fsm);
    db_close(&g_db);
    printf("Done.\n");
    return 0;
}
