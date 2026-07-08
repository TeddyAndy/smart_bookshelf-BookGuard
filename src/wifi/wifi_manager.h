/**
 * WiFi 管理器 — wpa_supplicant 封装
 *
 * 板端: wpa_supplicant + wpa_cli + wpa.conf
 * 用途: 扫描、连接、保存配置、状态查询
 * LVGL UI 通过此 API 实现 WiFi 连接界面
 */
#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_SSID_MAX     64
#define WIFI_PASS_MAX     64
#define WIFI_SCAN_MAX     32      /* 最多缓存 32 个 AP */

/* ── 扫描结果 ─────────────────────────────────── */

typedef struct {
    char ssid[WIFI_SSID_MAX];
    char bssid[20];
    int  signal;          /* dBm, 负数 */
    int  freq;            /* MHz */
    char flags[128];      /* WPA-PSK-CCMP, WPA2-PSK-CCMP ... */
} wifi_ap_t;

/* ── 当前连接状态 ─────────────────────────────── */

typedef struct {
    int  connected;
    char ssid[WIFI_SSID_MAX];
    char ip[32];
    int  signal;
} wifi_status_t;

/* ── 配置 (对应 wpa.conf) ─────────────────────── */

typedef struct {
    char ssid[WIFI_SSID_MAX];
    char psk[WIFI_PASS_MAX];
} wifi_config_t;

/* ═══════════════════════════════════════════════
 * API
 * ═══════════════════════════════════════════════ */

/**
 * 检查 wpa_supplicant 是否在运行
 * @return 1=运行中, 0=未运行
 */
int  wifi_is_running(void);

/**
 * 检查是否已连接热点
 * @return 1=已连接, 0=未连接
 */
int  wifi_is_connected(void);

/**
 * 获取当前连接状态
 * @param out  输出状态 (可为 NULL)
 * @return 0=成功, -1=失败
 */
int  wifi_get_status(wifi_status_t *out);

/**
 * 扫描附近 AP (阻塞, ~3秒)
 * @param aps  输出数组
 * @param max  数组大小
 * @return 扫描到的 AP 数量, -1=失败
 */
int  wifi_scan(wifi_ap_t *aps, int max);

/**
 * 连接指定热点 (写入 wpa.conf 并重载)
 * @param ssid  热点名称
 * @param psk   密码 (开放网络传 NULL)
 * @param save  1=写入配置文件持久化
 * @return 0=成功, -1=失败
 */
int  wifi_connect(const char *ssid, const char *psk, int save);

/**
 * 断开当前连接
 * @return 0=成功
 */
int  wifi_disconnect(void);

/**
 * 读取已保存的配置 (从 /root/wpa.conf)
 * @param out  输出
 * @return 0=有配置, -1=无配置/失败
 */
int  wifi_config_load(wifi_config_t *out);

/**
 * 保存配置到 /root/wpa.conf
 */
int  wifi_config_save(const wifi_config_t *cfg);

#ifdef __cplusplus
}
#endif
#endif
