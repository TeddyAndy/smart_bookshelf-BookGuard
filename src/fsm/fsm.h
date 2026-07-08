/**
 * 智能书架 FSM — 状态机 v8
 *
 * 状态: BOOT → ACTIVE ↔ SLEEP, BOOT/ACTIVE/SLEEP → FIND, 任意 → FAULT
 * 模块可开关, 黄灯自检, 品红初始化, FIND=黄灯
 */
#ifndef FSM_H
#define FSM_H

#include "../uhf/uhf_scanner.h"
#include "../db/db.h"
#include "../ld2410c/ld2410c.h"
#include "../mqtt/mqtt_client.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 硬件配置 ──────────────────────────────────────────────── */

#define FSM_UP_DEV      "/dev/serial/by-path/platform-ff780000.usb-usb-0:1.2:1.0-port0"
#define FSM_LO_DEV      "/dev/serial/by-path/platform-ff780000.usb-usb-0:1.3:1.0-port0"
#define FSM_RADAR_DEV   "/dev/ttyS1"
#define FSM_RADAR_BAUD  115200
#define FSM_RADAR_GATE_MOVING  2    /* 运动门数 (×0.75m) */
#define FSM_RADAR_GATE_STILL   2    /* 静止门数 (2=约1.5m, 避免近前静止误判无人) */
#define FSM_RADAR_UNMANNED_SEC 5    /* 无人确认秒 */
#define FSM_RADAR_SENS_MOVING  70
#define FSM_RADAR_SENS_STILL   75
#define FSM_SENSOR_I2C  "/dev/i2c-2"
#define FSM_IR_I2C      "/dev/i2c-2"
#define FSM_IR_SER_GPIO   32
#define FSM_IR_RCLK_GPIO  33
#define FSM_IR_SRCLK_GPIO 34
#define FSM_IR_BOARD_MAX  4
#define FSM_UP_PWR      17
#define FSM_LO_PWR      17
#define FSM_LED_SPI     "/dev/spidev1.0"
#define FSM_LED_COUNT   78
#define FSM_BRIGHT      50

#define FSM_DB_LOCAL    "/root/bookshelf.db"
#define FSM_DB_ARCHIVE  "/mnt/sdcard/bookshelf/history.db"
#define FSM_ARCHIVE_INTERVAL_SEC  600   /* 每10分钟归档清理一次 */

#ifndef FSM_MQTT_BROKER
#define FSM_MQTT_BROKER "tcp://82.157.67.148:1883"
#endif
#ifndef FSM_MQTT_USER
#define FSM_MQTT_USER   "board"
#endif
#ifndef FSM_MQTT_PASS
#define FSM_MQTT_PASS   "BoardBookGuard666"
#endif
#define FSM_MQTT_STATUS_ACTIVE_SEC  30    /* ACTIVE 全量快照间隔 */
#define FSM_MQTT_STATUS_SLEEP_SEC  300    /* SLEEP 全量快照间隔 */
#define FSM_MQTT_SENSOR_SEC          5    /* 传感器数据间隔 */
#define FSM_MQTT_POLL_MS             1    /* 每 tick poll 超时 */

#define FSM_BASELINE_TO   20
#define FSM_SLEEP_TIMEOUT 10
#define FSM_RADAR_FRAME_STALE_SEC 5
#define FSM_FIND_TIMEOUT  15   /* FIND 超时秒数 */
#define FSM_TICK_MS       40
#define FSM_DUMP_IVAL     2
#define FSM_OP_SETTLE_SEC 1    /* 红外稳定后等待 UHF 防抖的操作提交窗口 */
#define FSM_LAYER_CHANGE_SETTLE_SEC 3 /* UHF 跨层候选等待红外追平后再提交 */
#define FSM_IR_SLEEP_SCAN_SEC 1 /* SLEEP 低功耗红外扫描间隔 */

/* ── 状态 ──────────────────────────────────────────────────── */

typedef enum {
    FSM_BOOT   = 0,  /* 启动 + 基线 */
    FSM_ACTIVE = 1,  /* 有人操作, 全速盘存 */
    FSM_SLEEP  = 2,  /* 无人, 停盘存省电 */
    FSM_FIND   = 3,  /* 查找模式 (黄灯引导) */
    FSM_FAULT  = 4,  /* 故障 (红灯) */
} fsm_state_t;

/* ── 模块开关 ──────────────────────────────────────────────── */

typedef struct {
    int enabled;    /* 默认 1=开启检测 */
    int ok;         /* 初始化结果: 1=通过, 0=失败, -1=未检 */
} fsm_mod_t;

typedef struct {
    /* ── 状态 ── */
    fsm_state_t    state;
    fsm_state_t    prev_state;

    /* ── 模块 ── */
    fsm_mod_t      mod_led;
    fsm_mod_t      mod_radar;
    fsm_mod_t      mod_uhf;
    fsm_mod_t      mod_wifi;
    fsm_mod_t      mod_sensor;
    fsm_mod_t      mod_mqtt;
    fsm_mod_t      mod_screen;

    /* ── 硬件 ── */
    scanner_t      sc;
    db_ctx_t      *db;              /* NAND 本地库 */
    db_ctx_t       db_archive;      /* SD 归档库 (可空) */
    int            archive_ok;      /* 归档库可用 */
    ld2410c_ctx_t  radar;
    mqtt_ctx_t     mqtt;
    int            led_ok;

    /* ── 传感器 ── */
    int            i2c_fd;           /* /dev/i2c-2 fd */
    int            sensor_ok;        /* BH1750+SHT30 可用 */
    int            bh1750_ok;        /* 光照可用 */
    int            sht30_ok;         /* 温湿度可用 (当前可缺省) */
    double         last_temp;        /* 用于突变检测 */
    double         last_hum;
    double         last_lux;

    /* ── 红外定位 ── */
    int            ir_fd;
    int            ir_gpio_ok;
    int            ir_board_ok[FSM_IR_BOARD_MAX];
    uint16_t       ir_bitmap[FSM_IR_BOARD_MAX];
    uint16_t       ir_prev_bitmap[FSM_IR_BOARD_MAX];
    uint16_t       ir_raw_bitmap[FSM_IR_BOARD_MAX];
    int            ir_raw_streak[FSM_IR_BOARD_MAX];
    int            ir_changed;
    int            ir_change_layer;
    double         ir_change_start_cm;
    double         ir_change_end_cm;
    int            ir_removed_layer;
    double         ir_removed_start_cm;
    double         ir_removed_end_cm;
    uint32_t       t_last_ir_removed;
    int            ir_added_layer;
    double         ir_added_start_cm;
    double         ir_added_end_cm;
    uint32_t       t_last_ir_added;
    uint32_t       t_last_ir_scan;
    uint32_t       t_last_ir_reconcile;
    int            wifi_hw_present;
    int            wifi_connected;
    int            wifi_signal;
    char           wifi_ssid[64];
    char           wifi_ip[32];
    char           wifi_last_error[128];
    uint32_t       t_last_wifi_status;

    /* ── 时间 ── */
    uint32_t       t_start;
    uint32_t       t_last_dump;
    uint32_t       t_last_present;
    uint32_t       t_last_radar_frame;
    uint32_t       t_last_radar_person;
    uint32_t       t_last_archive;    /* 上次归档时间 */
    uint32_t       t_last_mqtt_status;/* 上次全量快照推送 */
    uint32_t       t_last_mqtt_sensor;/* 上次传感器推送 */
    uint32_t       t_last_state_save; /* 上次持久化运行快照 */
    unsigned long  radar_last_valid_frames;
    int            radar_seen_person;
    int            radar_sleep_reliable;

    /* ── 故障 ── */
    int            fault_dev;
    char           fault_text[256];
    char           fault_action[256];

    /* ── 基线 ── */
    int            baseline_locked[2];
    int            baseline_step[2];
    uint32_t       baseline_step_ts[2];

    /* ── 运行时 ── */
    int            verbose;
    char           find_query[128];  /* 搜索关键字 */
    char           find_target_epc[64];
    char           find_target_title[128];
    int            find_status;      /* 0=idle 1=searching 2=taken 3=timeout 4=cancelled */
    int            find_matches[64]; /* 匹配书籍的 book 索引 */
    int            find_count;       /* 匹配数量 */
    uint32_t       t_find_start;     /* FIND 模式开始时间 */
    char           pending_epc[64];
    int            pending_layer;
    int            add_mode;
    int            register_pending;
    int            led_brightness;
    uint32_t       t_last_brightness_poll;
    int            uhf_power[2];
    uint32_t       missing_since[64];
    int            layer_pending_valid[64];
    int            layer_pending_value[64];
    int            layer_pending_rssi[64];
    uint32_t       t_layer_pending[64];
    int            shelf_issue;       /* 1=有错放/不确定, 用状态灯提示 */
    char           shelf_issue_text[256];

    /* ── 红外/UHF 操作窗口 ── */
    int            op_active;
    uint32_t       op_started_at;
    uint32_t       op_last_ir_change;
    uint16_t       op_before_ir[FSM_IR_BOARD_MAX];
    uint16_t       op_after_ir[FSM_IR_BOARD_MAX];
    uint16_t       op_seen_removed_ir[FSM_IR_BOARD_MAX];
    uint16_t       op_seen_added_ir[FSM_IR_BOARD_MAX];
    uint16_t       recent_removed_ir[FSM_IR_BOARD_MAX];
    double         recent_removed_start_cm[FSM_IR_BOARD_MAX];
    double         recent_removed_end_cm[FSM_IR_BOARD_MAX];
    uint32_t       t_recent_removed_ir[FSM_IR_BOARD_MAX];
    int            op_before_present[64];
    int            op_before_layer[64];
    double         op_before_start[64];
    double         op_before_end[64];
} fsm_t;

/* ── API ───────────────────────────────────────────────────── */

int  fsm_init(fsm_t *f, db_ctx_t *db);
int  fsm_tick(fsm_t *f);
void fsm_stop(fsm_t *f);

/* 模块开关 (init 前设置) */
void fsm_enable_all(fsm_t *f);

/* UI / 调试桥接 */
int  fsm_request_find(fsm_t *f, const char *query);
void fsm_finish_find(fsm_t *f, int cancelled);
int  fsm_force_state(fsm_t *f, fsm_state_t state);
int  fsm_register_book(fsm_t *f, const char *epc, const char *title,
                       const char *author, int layer,
                       double start_cm, double end_cm);
int  fsm_register_book_pages(fsm_t *f, const char *epc, const char *title,
                             const char *author, int layer,
                             double start_cm, double end_cm, int pages);

#ifdef __cplusplus
}
#endif
#endif
