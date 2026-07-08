/**
 * UHF 双层扫描器 — DB 驱动, 持续轮询 + last_seen
 *
 * 使用:
 *   scanner_t sc;
 *   scanner_init(&sc, &db, "/dev/ttyS1", "/dev/ttyUSB0", 29, 16);
 *   while (running) {
 *       scanner_poll(&sc);
 *       // 检查 sc.events[] 处理变化
 *       usleep(50000);
 *   }
 *   scanner_stop(&sc);
 */
#ifndef UHF_SCANNER_H
#define UHF_SCANNER_H

#include "uhf_driver.h"
#include "../db/db.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCANNER_MAX_BOOKS  64
#define SCANNER_MAX_EVENTS 16

/* ── 事件类型 ────────────────────────────────────────────── */

typedef enum {
    SCAN_EV_NONE = 0,
    SCAN_EV_FOUND,       /* 书在架 (每轮确认) */
    SCAN_EV_MISSING,     /* 书被取走 (last_seen > 阈值) */
    SCAN_EV_RETURNED,    /* 书放回 */
    SCAN_EV_UNKNOWN,     /* 读到未登记 EPC */
} scan_event_type_t;

typedef struct {
    scan_event_type_t type;
    char    epc[65];
    char    title[128];
    int     layer;        /* 0=上 1=下 */
    int     expected_layer;
    int     rssi;
} scan_event_t;

/* ── 扫描器 ──────────────────────────────────────────────── */

typedef struct {
    /* 硬件 */
    uhf_t    dev[2];          /* [0]=上层, [1]=下层 */
    int      power[2];
    int      ready;           /* 初始化成功标志 */

    /* DB */
    db_ctx_t *db;

    /* 已知 EPC 列表 (从 DB books 表加载) */
    int      book_count;
    char     book_epc[SCANNER_MAX_BOOKS][65];
    char     book_title[SCANNER_MAX_BOOKS][128];
    int      book_layer[SCANNER_MAX_BOOKS];  /* 预期层 */
    int      actual_layer[SCANNER_MAX_BOOKS]; /* 本轮主读层 */
    int      last_rssi[SCANNER_MAX_BOOKS];
    int      last_rssi_side[SCANNER_MAX_BOOKS][2]; /* 上/下最近 RSSI */
    int      layer_score[SCANNER_MAX_BOOKS][2]; /* 上/下近期读数滚动分数 */
    int      last_read_count[SCANNER_MAX_BOOKS][2]; /* 驱动累计 reads 的上次值 */

    /* last_seen 追踪 (索引对应 book_epc) */
    uint32_t last_seen[SCANNER_MAX_BOOKS];
    int      is_present[SCANNER_MAX_BOOKS];
    int      seen_streak[SCANNER_MAX_BOOKS];   /* 连续检测到次数 (放回确认) */
    int      miss_streak[SCANNER_MAX_BOOKS];   /* 连续缺次数 (取出确认) */
    int      layer_change_streak[SCANNER_MAX_BOOKS]; /* 主读层变化确认 */
    int      layer_candidate[SCANNER_MAX_BOOKS]; /* 当前候选主读层 */
    int      layer_candidate_streak[SCANNER_MAX_BOOKS];
    uint32_t missing_reported_at[SCANNER_MAX_BOOKS];

    char     unknown_epc[SCANNER_MAX_EVENTS][65];
    uint32_t unknown_seen[SCANNER_MAX_EVENTS];
    int      unknown_count;
    int      detect_unknown;

    /* 参数 */
    int      present_timeout_sec;   /* 多久未读到判为取走 (默认 5s) */
    int      return_confirm;        /* 连续几次检测到判放回 (默认 2) */
    int      missing_confirm;       /* 连续几次未读到判取出 (默认 3) */
    int      layer_change_confirm;  /* 连续几次主读层变化判错放/归位 */

    /* 事件队列 */
    scan_event_t events[SCANNER_MAX_EVENTS];
    int          event_count;

    /* 统计 */
    int      round_count;
    uint32_t started_at;
} scanner_t;

/* ── API ─────────────────────────────────────────────────── */

/**
 * 初始化扫描器
 * @param up_dev    上层设备路径 (/dev/ttyS1)
 * @param lo_dev    下层设备路径 (/dev/ttyUSB0)
 * @param up_power  上层功率 (dBm)
 * @param lo_power  下层功率 (dBm)
 * @return 0=成功 -1=失败
 */
int scanner_init(scanner_t *sc, db_ctx_t *db,
                 const char *up_dev, const char *lo_dev,
                 int up_power, int lo_power);

/**
 * 主轮询 — 每轮调一次, poll 两个模块, 更新 last_seen,
 * 检测 missing/returned, 填充 events[]
 */
int scanner_poll(scanner_t *sc);

/**
 * 停止扫描器, 更新 DB shelf_state, 释放硬件
 */
void scanner_stop(scanner_t *sc);

/**
 * 获取某本书的 last_seen 秒数
 */
int scanner_last_seen_sec(scanner_t *sc, const char *epc);

/**
 * 打印当前状态 (调试)
 */
void scanner_dump(scanner_t *sc);

#ifdef __cplusplus
}
#endif
#endif
