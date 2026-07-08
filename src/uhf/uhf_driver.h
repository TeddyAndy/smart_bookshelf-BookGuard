/**
 * CPH305F UHF RFID 驱动 — 协议帧层
 *
 * 帧格式: | 52 46 | type | addr(2) | code | param_len(2) | [TLV...] | csum |
 *   type: 0=命令  1=响应  2=通知
 *   code: 0x21=开始盘存  0x23=停止  0x40=设备信息  0x48/0x49=单参数读写
 *         0x80=标签通知(通知帧, 0x50=单标签TLV)
 *
 * 使用模式 (唯一验证可靠的):
 *   init → set_power → inventory_start → poll(...) → inventory_stop → deinit
 *   不反复 init/deinit，不反复 start/stop
 */
#ifndef UHF_DRIVER_H
#define UHF_DRIVER_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UHF_MAX_TAGS    128
#define UHF_EPC_MAX     32
#define UHF_EPC_STR     65
#define UHF_DEVICE_MAX  64

/* ── 标签 ─────────────────────────────────────────────────── */

typedef struct {
    uint8_t  epc[UHF_EPC_MAX];
    uint8_t  epc_len;
    char     epc_str[UHF_EPC_STR];
    uint8_t  rssi;
    uint8_t  antenna;
    int      read_count;
    time_t   first_seen;
    time_t   last_seen;
} uhf_tag_t;

/* ── 回调 ─────────────────────────────────────────────────── */

typedef void (*uhf_on_tag_fn)(const uhf_tag_t *tag, void *user);

/* ── 上下文 ───────────────────────────────────────────────── */

typedef struct {
    int      fd;
    char     device[UHF_DEVICE_MAX];
    uint8_t  power;
    uint8_t  fw_ver[3];
    int      scanning;            /* 是否已发 0x21 启动盘存 */
    int      verbose;

    uhf_tag_t  tags[UHF_MAX_TAGS];
    int        tag_count;

    uhf_on_tag_fn  on_tag;
    void          *on_tag_user;
} uhf_t;

/* ── 生命周期 ─────────────────────────────────────────────── */

/** 打开串口, 查设备信息, 返回 0 成功 */
int  uhf_init(uhf_t *u, const char *device);
/** 停止盘存(如果在), 关闭串口 */
void uhf_deinit(uhf_t *u);

/* ── 参数 ─────────────────────────────────────────────────── */

/** 设置功率 (dBm, 1 字节直接值) */
int  uhf_set_power(uhf_t *u, uint8_t dbm);
/** 查询功率 */
int  uhf_get_power(uhf_t *u, uint8_t *dbm);

/* ── 盘存控制 ─────────────────────────────────────────────── */

/** 开始持续盘存 (0x21) */
int  uhf_inventory_start(uhf_t *u);
/** 停止盘存 (0x23) — 跳过通知帧, 等待匹配的响应帧, 最长 3s */
int  uhf_inventory_stop(uhf_t *u);
/** 单次盘存 (0x22, 阻塞, 等 timeout_ms 毫秒) */
int  uhf_inventory_once(uhf_t *u, int timeout_ms);

/**
 * 非阻塞轮询 — 主循环高频调用
 * @param timeout_ms  单次 select 等待时间 (ms), 建议 1~50
 * @return 本轮新发现的标签数, -1 出错
 */
int  uhf_poll(uhf_t *u, int timeout_ms);

/* ── 标签缓存 ─────────────────────────────────────────────── */

void uhf_tags_clear(uhf_t *u);
int  uhf_tag_index(uhf_t *u, const char *epc_str);
int  uhf_tag_count(uhf_t *u);

/* ── 回调 ─────────────────────────────────────────────────── */

/** 设标签回调: 每次发现新标签时触发 */
void uhf_on_tag(uhf_t *u, uhf_on_tag_fn fn, void *user);

/* ── 调试 ─────────────────────────────────────────────────── */

void uhf_verbose(uhf_t *u, int on);

#ifdef __cplusplus
} }
#endif
#endif
