/**
 * 智能书架 FSM v8 — 实现
 *
 * BOOT → 自检(品红) → 基线(橙) → ACTIVE(绿) ↔ SLEEP(暗蓝)
 *   → FIND(黄, 找书) → ACTIVE
 *   → FAULT(红, 任意模块致命故障)
 *
 * 模块可通过 --no-xxx 关闭, 默认全开
 */
#include "fsm.h"
#include "../common/sb_ipc.h"
#include "../ws2812b/shelf_led.h"
#include "../uhf/uhf_driver.h"
#include "../wifi/wifi_manager.h"
#include "../mqtt/json_builder.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <termios.h>
#include <math.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

/* ── 前向声明 ─────────────────────────────────────────── */

static void enter_find(fsm_t *f);
static void leave_find(fsm_t *f, int cancelled);
static void enter_active(fsm_t *f);
static int  sensor_read(fsm_t *f, double *temp, double *hum, double *lux);
static int  ir_scan(fsm_t *f);
static int  ir_prime_baseline(fsm_t *f);
static void uhf_safe_startup_prepare(void);
static void uhf_stop_both(fsm_t *f);
static void enter_fault(fsm_t *f, int fault_dev);
static void enter_fault_reason(fsm_t *f, int fault_dev, const char *text, const char *action);
static int  run_selfcheck(fsm_t *f);
static void led_status(fsm_t *f, uint8_t r, uint8_t g, uint8_t b);
static int  fsm_set_led_brightness(fsm_t *f, int brightness, int persist);
static int  fsm_set_uhf_power(fsm_t *f, int upper_dbm, int lower_dbm, int persist);
static int  book_expected_pos(fsm_t *f, const char *epc, double *start, double *end);
static void fsm_begin_find(fsm_t *f, const char *query);

typedef struct {
    int enabled;
    int addr;
    int layer;
    double start_cm;
    double end_cm;
    int reverse;
    const char *name;
} ir_board_cfg_t;

static const ir_board_cfg_t g_ir_boards[FSM_IR_BOARD_MAX] = {
    /* 单块 16 路覆盖半层书架，四块板拼成上下两层 38cm 覆盖 */
    {1, 0x21, 0,  0.0, 19.0, 0, "upper_left"},
    {1, 0x20, 0, 19.0, 38.0, 0, "upper_right"},
    {1, 0x24, 1,  0.0, 19.0, 0, "lower_left"},
    {1, 0x22, 1, 19.0, 38.0, 0, "lower_right"},
};

#define IR_STABLE_SCANS      4
#define IR_ONLY_MOVE_MIN_BITS 6
#define IR_REORDER_MIN_SPAN_CM 4.0
#define SHELF_LEN_CM 38.0

/* ── 本地 IPC 文件由 src/common/sb_ipc.h 统一定义 ───────────── */

static const char *fsm_state_text(fsm_state_t state)
{
    switch (state) {
    case FSM_BOOT:   return "BOOT";
    case FSM_ACTIVE: return "ACTIVE";
    case FSM_SLEEP:  return "SLEEP";
    case FSM_FIND:   return "FIND";
    case FSM_FAULT:  return "FAULT";
    default:         return "UNKNOWN";
    }
}

static int fsm_parse_state_text(const char *text, fsm_state_t *state)
{
    if (!text || !state) return -1;
    if (!strcmp(text, "BOOT")) {
        *state = FSM_BOOT;
        return 0;
    }
    if (!strcmp(text, "ACTIVE")) {
        *state = FSM_ACTIVE;
        return 0;
    }
    if (!strcmp(text, "SLEEP")) {
        *state = FSM_SLEEP;
        return 0;
    }
    if (!strcmp(text, "FIND")) {
        *state = FSM_FIND;
        return 0;
    }
    if (!strcmp(text, "FAULT")) {
        *state = FSM_FAULT;
        return 0;
    }
    return -1;
}

static int radar_frame_is_fresh(const fsm_t *f, uint32_t now)
{
    if (!f || !f->t_last_radar_frame) return 0;
    return now - f->t_last_radar_frame <= FSM_RADAR_FRAME_STALE_SEC;
}

static void fsm_update_radar_runtime(fsm_t *f, int radar_has_person, uint32_t now)
{
    if (!f || !f->mod_radar.ok) {
        if (f) f->radar_sleep_reliable = 0;
        return;
    }

    const ld2410c_stats_t *st = ld2410c_get_stats(&f->radar);
    if (st && st->valid_frames != f->radar_last_valid_frames) {
        f->radar_last_valid_frames = st->valid_frames;
        f->t_last_radar_frame = now;
    }

    if (radar_has_person) {
        f->radar_seen_person = 1;
        f->t_last_radar_person = now;
    }

    /*
     * 只有雷达本次启动后确实识别过人体、且仍在稳定出帧，才允许它触发自动休眠。
     * 否则 LD2410C 卡在“无人”时会把有人操作的书架误关。
     */
    f->radar_sleep_reliable = f->radar_seen_person && radar_frame_is_fresh(f, now);
}

static void fsm_update_wifi_status(fsm_t *f, uint32_t now)
{
    wifi_status_t st;
    int fd;

    if (!f) return;
    if (f->t_last_wifi_status && now - f->t_last_wifi_status < 3) return;
    f->t_last_wifi_status = now;

    fd = open("/sys/class/net/wlan0/operstate", O_RDONLY);
    f->wifi_hw_present = (fd >= 0);
    if (fd >= 0) close(fd);
    if (f->mod_wifi.enabled) f->mod_wifi.ok = f->wifi_hw_present;

    f->wifi_connected = 0;
    f->wifi_signal = 0;
    f->wifi_ssid[0] = '\0';
    f->wifi_ip[0] = '\0';

    if (!f->wifi_hw_present) {
        snprintf(f->wifi_last_error, sizeof(f->wifi_last_error), "未检测到 wlan0");
        return;
    }

    if (wifi_get_status(&st) == 0) {
        static uint32_t last_dhcp_try = 0;
        f->wifi_connected = st.connected;
        f->wifi_signal = st.signal;
        snprintf(f->wifi_ssid, sizeof(f->wifi_ssid), "%s", st.ssid);
        snprintf(f->wifi_ip, sizeof(f->wifi_ip), "%s", st.ip);
        if (st.connected && !st.ip[0] && now - last_dhcp_try >= 15) {
            last_dhcp_try = now;
            system("udhcpc -i wlan0 -q -n >/tmp/udhcpc.log 2>&1 &");
            snprintf(f->wifi_last_error, sizeof(f->wifi_last_error), "已连接，正在获取 IP");
        } else if (st.connected)
            f->wifi_last_error[0] = '\0';
        else if (!f->wifi_last_error[0])
            snprintf(f->wifi_last_error, sizeof(f->wifi_last_error), "未连接");
    } else {
        snprintf(f->wifi_last_error, sizeof(f->wifi_last_error), "读取 WiFi 状态失败");
    }
}

static void fsm_write_state_file(fsm_t *f)
{
    char buf[2048];
    int len;

    if (!f) return;
    fsm_update_wifi_status(f, (uint32_t)time(NULL));
    len = snprintf(buf, sizeof(buf),
            "state=%s\nfault_dev=%d\nfault_text=%s\nfault_action=%s\nfind_count=%d\n"
            "find_status=%d\nfind_target_epc=%s\nfind_target_title=%s\n"
            "add_mode=%d\nregister_pending=%d\npending_epc=%s\npending_layer=%d\n"
            "radar_seen=%d\nradar_sleep_reliable=%d\nradar_last_person=%u\n"
            "wifi_hw=%d\nwifi_connected=%d\nwifi_ssid=%s\nwifi_ip=%s\n"
            "wifi_signal=%d\nwifi_error=%s\nmqtt_connected=%d\n"
            "led_brightness=%d\n"
            "uhf_power_upper=%d\nuhf_power_lower=%d\n"
            "mod_led_ok=%d\nmod_radar_ok=%d\nmod_uhf_ok=%d\n"
            "mod_wifi_ok=%d\nmod_sensor_ok=%d\nmod_mqtt_ok=%d\nmod_screen_ok=%d\n"
            "bh1750_ok=%d\nsht30_ok=%d\n"
            "shelf_issue=%d\nshelf_issue_text=%s\n"
            "ir0_ok=%d\nir0_bitmap=%04x\n"
            "ir1_ok=%d\nir1_bitmap=%04x\n"
            "ir2_ok=%d\nir2_bitmap=%04x\n"
            "ir3_ok=%d\nir3_bitmap=%04x\n",
            fsm_state_text(f->state), f->fault_dev,
            f->fault_text, f->fault_action, f->find_count,
            f->find_status, f->find_target_epc, f->find_target_title,
            f->add_mode, f->register_pending, f->pending_epc, f->pending_layer,
            f->radar_seen_person, f->radar_sleep_reliable, f->t_last_radar_person,
            f->wifi_hw_present, f->wifi_connected, f->wifi_ssid, f->wifi_ip,
            f->wifi_signal, f->wifi_last_error,
            f->mod_mqtt.ok && f->mqtt.connected,
            f->led_brightness,
            f->uhf_power[0], f->uhf_power[1],
            f->mod_led.ok, f->mod_radar.ok, f->mod_uhf.ok,
            f->mod_wifi.ok, f->mod_sensor.ok, f->mod_mqtt.ok, f->mod_screen.ok,
            f->bh1750_ok, f->sht30_ok,
            f->shelf_issue, f->shelf_issue_text,
            f->ir_board_ok[0], f->ir_bitmap[0],
            f->ir_board_ok[1], f->ir_bitmap[1],
            f->ir_board_ok[2], f->ir_bitmap[2],
            f->ir_board_ok[3], f->ir_bitmap[3]);
    if (len < 0 || len >= (int)sizeof(buf)) return;
    sb_write_text_atomic(SB_FSM_STATE_FILE, buf);
}

static void fsm_set_issue(fsm_t *f, const char *text)
{
    if (!f) return;
    f->shelf_issue = 1;
    snprintf(f->shelf_issue_text, sizeof(f->shelf_issue_text), "%s",
             text && text[0] ? text : "书架状态需要人工确认");
    db_meta_set(f->db, "shelf_issue", "1");
    db_meta_set(f->db, "shelf_issue_text", f->shelf_issue_text);
}

static void fsm_clear_issue(fsm_t *f)
{
    if (!f) return;
    f->shelf_issue = 0;
    f->shelf_issue_text[0] = '\0';
    db_meta_set(f->db, "shelf_issue", "0");
    db_meta_set(f->db, "shelf_issue_text", "");
}

static int parse_int_0_255(const char *s, int *out)
{
    char *end = NULL;
    long v;

    if (!s || !out) return -1;
    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || end == s) return -1;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') end++;
    if (*end || v < 0 || v > 255) return -1;
    *out = (int)v;
    return 0;
}

static int parse_power_dbm(const char *s, int *out)
{
    int v = 0;
    if (parse_int_0_255(s, &v) < 0) return -1;
    if (v < 10 || v > 33) return -1;
    *out = v;
    return 0;
}

static int fsm_set_led_brightness(fsm_t *f, int brightness, int persist)
{
    char value[16];

    if (!f || brightness < 0 || brightness > 255) return -1;
    if (f->led_brightness == brightness) return 0;

    f->led_brightness = brightness;
    if (f->led_ok) {
        shelf_led_set_brightness((uint8_t)brightness);
        shelf_led_show();
    }
    if (persist && f->db) {
        snprintf(value, sizeof(value), "%d", brightness);
        db_meta_set(f->db, "led_brightness", value);
        sb_write_text_atomic(SB_LED_BRIGHTNESS_FILE, value);
    }
    fsm_write_state_file(f);
    printf("[FSM] LED亮度: %d/255\n", brightness);
    return 0;
}

static int fsm_set_uhf_power(fsm_t *f, int upper_dbm, int lower_dbm, int persist)
{
    char value[16];
    int changed = 0;

    if (!f) return -1;
    if (upper_dbm < 10 || upper_dbm > 33 || lower_dbm < 10 || lower_dbm > 33)
        return -1;

    if (f->uhf_power[0] != upper_dbm) {
        f->uhf_power[0] = upper_dbm;
        changed = 1;
    }
    if (f->uhf_power[1] != lower_dbm) {
        f->uhf_power[1] = lower_dbm;
        changed = 1;
    }
    f->sc.power[0] = f->uhf_power[0];
    f->sc.power[1] = f->uhf_power[1];

    for (int d = 0; d < 2; d++) {
        if (f->sc.dev[d].fd >= 0) {
            if (uhf_set_power(&f->sc.dev[d], (uint8_t)f->sc.power[d]) < 0) {
                enter_fault_reason(f, d,
                                   d == 0 ? "上层 UHF 设置功率失败" : "下层 UHF 设置功率失败",
                                   "检查对应 UHF 连接，设备恢复后点击重新检测");
                return -1;
            }
        }
    }

    if (persist && f->db) {
        snprintf(value, sizeof(value), "%d", f->uhf_power[0]);
        db_meta_set(f->db, "uhf_power_upper", value);
        snprintf(value, sizeof(value), "%d", f->uhf_power[1]);
        db_meta_set(f->db, "uhf_power_lower", value);
    }
    if (changed || persist) {
        printf("[FSM] UHF功率: 上%d dBm / 下%d dBm\n", f->uhf_power[0], f->uhf_power[1]);
        fsm_write_state_file(f);
    }
    return 0;
}

static void fsm_poll_led_brightness_file(fsm_t *f, uint32_t now)
{
    char buf[32];
    FILE *fp;
    int brightness;

    if (!f) return;
    if (now == f->t_last_brightness_poll) return;
    f->t_last_brightness_poll = now;

    fp = fopen(SB_LED_BRIGHTNESS_FILE, "r");
    if (!fp) return;
    if (!fgets(buf, sizeof(buf), fp)) {
        fclose(fp);
        return;
    }
    fclose(fp);

    if (parse_int_0_255(buf, &brightness) == 0)
        fsm_set_led_brightness(f, brightness, 1);
}

static void fsm_join_book_titles(fsm_t *f, const int *idx, int n, char *out, size_t out_sz)
{
    int used = 0;
    if (!out || out_sz == 0) return;
    out[0] = '\0';
    if (!f || !idx || n <= 0) return;
    for (int i = 0; i < n; i++) {
        int b = idx[i];
        const char *title = (b >= 0 && b < f->sc.book_count) ? f->sc.book_title[b] : "?";
        int left = (int)out_sz - used;
        int wrote;
        if (left <= 1) break;
        wrote = snprintf(out + used, (size_t)left, "%s%s", used ? "/" : "", title);
        if (wrote < 0) break;
        if (wrote >= left) {
            used = (int)out_sz - 1;
            break;
        }
        used += wrote;
        if (used > (int)out_sz - 12 && i + 1 < n) {
            snprintf(out + used, out_sz - (size_t)used, "/等%d本", n);
            break;
        }
    }
}

static void fsm_save_runtime_snapshot(fsm_t *f, int clean_shutdown)
{
    char buf[256];
    int present = 0;
    int misplaced = 0;

    if (!f || !f->db) return;
    for (int i = 0; i < f->sc.book_count; i++) {
        shelf_item_t s;
        int p = f->sc.is_present[i];
        int layer = f->sc.actual_layer[i];
        if (db_shelf_find(f->db, f->sc.book_epc[i], &s) == 1) {
            p = s.is_present;
            layer = s.layer;
        }
        if (p) {
            present++;
            if (layer != f->sc.book_layer[i]) misplaced++;
        }
    }

    snprintf(buf, sizeof(buf), "books=%d present=%d misplaced=%d issue=%d",
             f->sc.book_count, present, misplaced, f->shelf_issue);
    db_meta_set(f->db, "last_snapshot", buf);
    db_meta_set(f->db, "clean_shutdown", clean_shutdown ? "1" : "0");
    db_meta_set(f->db, "fsm_state", fsm_state_text(f->state));
}

static void fsm_clear_register_pending(fsm_t *f)
{
    if (!f) return;
    f->register_pending = 0;
    f->pending_epc[0] = '\0';
    f->pending_layer = -1;
    f->sc.detect_unknown = 0;
}

static int parse_register_cmd(char *cmd, int *layer, double *start, double *end,
                              char **title)
{
    char *p = cmd + 9; /* REGISTER */
    char *tok;
    while (*p == ' ') p++;
    tok = strsep(&p, " ");
    if (!tok || !p) return -1;
    *layer = atoi(tok);
    tok = strsep(&p, " ");
    if (!tok || !p) return -1;
    *start = atof(tok);
    tok = strsep(&p, " ");
    if (!tok || !p) return -1;
    *end = atof(tok);
    while (*p == ' ') p++;
    if (!*p) return -1;
    *title = p;
    return 0;
}

static int parse_register_pages_cmd(char *cmd, int *layer, int *pages, char **title)
{
    char *p = cmd + 14; /* REGISTER_PAGES */
    char *tok;
    while (*p == ' ') p++;
    tok = strsep(&p, " ");
    if (!tok || !p) return -1;
    *layer = atoi(tok);
    tok = strsep(&p, " ");
    if (!tok || !p) return -1;
    *pages = atoi(tok);
    while (*p == ' ') p++;
    if (!*p) return -1;
    *title = p;
    return 0;
}

static double book_width_from_pages(int pages)
{
    double w;
    if (pages <= 0) pages = 200;
    w = pages * 0.0055;  /* 约 0.55cm / 100页，和运行时宽度估算保持一致。 */
    if (w < 0.8) w = 0.8;
    if (w > 8.0) w = 8.0;
    return w;
}

static void choose_register_position(fsm_t *f, int layer, int pages,
                                     double *start, double *end)
{
    double width = book_width_from_pages(pages);
    double s = 0.0;
    uint32_t now = (uint32_t)time(NULL);

    if (f && f->ir_added_layer == layer && f->ir_added_end_cm > f->ir_added_start_cm &&
        f->t_last_ir_added && now - f->t_last_ir_added <= 30) {
        double center = (f->ir_added_start_cm + f->ir_added_end_cm) / 2.0;
        s = center - width / 2.0;
    } else if (f && f->db) {
        shelf_item_t items[96];
        int n = db_shelf_list_layer(f->db, layer, items, 96);
        double max_end = 0.0;
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                if (!items[i].is_present) continue;
                if (items[i].end_cm > max_end) max_end = items[i].end_cm;
            }
        }
        s = max_end;
    }

    if (s < 0.0) s = 0.0;
    if (s + width > 38.0) s = 38.0 - width;
    if (s < 0.0) s = 0.0;
    if (start) *start = s;
    if (end) *end = s + width;
}

static void fsm_begin_find(fsm_t *f, const char *query)
{
    if (!f || !query || !query[0]) return;
    snprintf(f->find_query, sizeof(f->find_query), "%.127s", query);
    f->find_count = 0;
    f->find_status = 1;
    f->find_target_epc[0] = '\0';
    f->find_target_title[0] = '\0';
    if (f->state == FSM_ACTIVE || f->state == FSM_SLEEP || f->state == FSM_FIND)
        enter_find(f);
}

static int json_string_field(const char *json, const char *field, char *out, size_t out_sz)
{
    char needle[64];
    const char *p;
    const char *end;
    size_t len;

    if (!json || !field || !out || out_sz == 0) return 0;
    snprintf(needle, sizeof(needle), "\"%s\"", field);
    p = strstr(json, needle);
    if (!p) return 0;
    p = strchr(p + strlen(needle), ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    end = p;
    while (*end && *end != '"') end++;
    len = (size_t)(end - p);
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return out[0] != '\0';
}

static void wifi_prepare_runtime(void)
{
    system("ip link set wlan0 up 2>/dev/null");
    if (!wifi_is_running()) {
        system("mkdir -p /var/run/wpa_supplicant; "
               "wpa_supplicant -B -i wlan0 -c /root/wpa.conf >/tmp/wpa_supplicant.log 2>&1");
    }
}

static void fsm_wifi_scan_to_file(fsm_t *f)
{
    FILE *fp;
    wifi_ap_t aps[WIFI_SCAN_MAX];
    int n;
    uint32_t started = (uint32_t)time(NULL);

    if (!f) return;
    wifi_prepare_runtime();
    sb_write_text_atomic(SB_WIFI_SCAN_FILE, "status=scanning\nmessage=正在扫描 WiFi\n");
    n = wifi_scan(aps, WIFI_SCAN_MAX);
    fp = fopen(SB_WIFI_SCAN_FILE, "w");
    if (!fp) return;
    fprintf(fp, "updated_at=%u\n", (unsigned)time(NULL));
    if (n < 0) {
        fprintf(fp, "status=error\n");
        fprintf(fp, "error=扫描失败\n");
        snprintf(f->wifi_last_error, sizeof(f->wifi_last_error), "扫描失败");
    } else {
        fprintf(fp, "status=done\n");
        fprintf(fp, "count=%d\n", n);
        fprintf(fp, "duration=%u\n", (unsigned)((uint32_t)time(NULL) - started));
        for (int i = 0; i < n; ++i)
            fprintf(fp, "ap=%s|%d|%s\n", aps[i].ssid, aps[i].signal, aps[i].flags);
        snprintf(f->wifi_last_error, sizeof(f->wifi_last_error), "扫描到 %d 个网络", n);
    }
    fclose(fp);
    f->t_last_wifi_status = 0;
}

static void fsm_retry_fault(fsm_t *f)
{
    if (!f) return;
    printf("[FSM] UI 请求重新检测故障模块\n");
    if (f->led_ok) led_status(f, 255, 0, 255);
    if (f->mod_uhf.ok) scanner_stop(&f->sc);
    if (f->mod_radar.ok) ld2410c_close(&f->radar);
    if (f->mod_mqtt.ok && f->mqtt.connected) mqtt_disconnect(&f->mqtt);
    f->mod_led.ok = 0;
    f->mod_radar.ok = 0;
    f->mod_uhf.ok = 0;
    f->mod_wifi.ok = 0;
    f->mod_sensor.ok = 0;
    f->mod_mqtt.ok = 0;
    f->mod_screen.ok = 0;
    f->bh1750_ok = 0;
    f->sht30_ok = 0;

    if (run_selfcheck(f) == 0) {
        f->fault_dev = -1;
        f->fault_text[0] = '\0';
        f->fault_action[0] = '\0';
        f->state = FSM_BOOT;
        f->prev_state = FSM_BOOT;
        memset(f->baseline_locked, 0, sizeof(f->baseline_locked));
        memset(f->baseline_step, 0, sizeof(f->baseline_step));
        memset(f->baseline_step_ts, 0, sizeof(f->baseline_step_ts));
        f->t_start = (uint32_t)time(NULL);
        f->t_last_present = f->t_start;
        if (f->ir_fd >= 0) ir_prime_baseline(f);
        led_status(f, 255, 128, 0);
        printf("[FSM] 故障重检通过, 返回 BOOT 基线\n");
    } else {
        printf("[FSM] 故障重检仍未通过\n");
        enter_fault(f, f->fault_dev);
    }
    fsm_write_state_file(f);
}

static void fsm_wifi_connect_cmd(fsm_t *f, char *payload)
{
    char *sep;
    char *ssid = payload;
    char *psk = "";

    if (!f || !payload || !payload[0]) return;
    sep = strchr(payload, '|');
    if (sep) {
        *sep = '\0';
        psk = sep + 1;
    }

    wifi_prepare_runtime();
    if (wifi_connect(ssid, psk, 1) == 0) {
        system("udhcpc -i wlan0 -q -n >/tmp/udhcpc.log 2>&1 &");
        snprintf(f->wifi_last_error, sizeof(f->wifi_last_error), "正在连接 %.96s", ssid);
    } else {
        snprintf(f->wifi_last_error, sizeof(f->wifi_last_error), "连接配置失败");
    }
    f->t_last_wifi_status = 0;
}

static void fsm_poll_local_cmd(fsm_t *f)
{
    char cmd[256];
    char processing[256];
    fsm_state_t forced_state;
    FILE *fp;

    if (!f) return;
    snprintf(processing, sizeof(processing), "%s.processing.%ld",
             SB_CMD_FILE, (long)getpid());
    if (rename(SB_CMD_FILE, processing) != 0) return;

    fp = fopen(processing, "r");
    if (!fp) {
        unlink(processing);
        return;
    }

    if (!fgets(cmd, sizeof(cmd), fp)) {
        fclose(fp);
        unlink(processing);
        return;
    }
    fclose(fp);
    unlink(processing);

    cmd[strcspn(cmd, "\r\n")] = '\0';

    if (!cmd[0]) return;

    if (!strncmp(cmd, "FIND ", 5)) {
        fsm_begin_find(f, cmd + 5);
        return;
    }

    if (!strcmp(cmd, "FIND_CANCEL")) {
        leave_find(f, 1);
        return;
    }

    if (!strcmp(cmd, "CLEAR_ISSUE")) {
        fsm_clear_issue(f);
        fsm_write_state_file(f);
        printf("[FSM] 人工确认: 清除书架提示\n");
        return;
    }

    if (!strncmp(cmd, "LED_BRIGHTNESS ", 15)) {
        int brightness = 0;
        if (parse_int_0_255(cmd + 15, &brightness) == 0)
            fsm_set_led_brightness(f, brightness, 1);
        else
            printf("[FSM] LED亮度命令无效: %s\n", cmd + 15);
        return;
    }

    if (!strncmp(cmd, "UHF_POWER ", 10)) {
        int up = 0, lo = 0;
        char *p = cmd + 10;
        char *sep = strchr(p, ' ');
        if (sep) {
            *sep++ = '\0';
            while (*sep == ' ') sep++;
        }
        if (sep && parse_power_dbm(p, &up) == 0 && parse_power_dbm(sep, &lo) == 0)
            fsm_set_uhf_power(f, up, lo, 1);
        else
            printf("[FSM] UHF功率命令无效: %s\n", cmd + 10);
        return;
    }

    if (!strncmp(cmd, "UHF_POWER_UPPER ", 16)) {
        int up = 0;
        if (parse_power_dbm(cmd + 16, &up) == 0)
            fsm_set_uhf_power(f, up, f->uhf_power[1], 1);
        else
            printf("[FSM] 上层UHF功率命令无效: %s\n", cmd + 16);
        return;
    }

    if (!strncmp(cmd, "UHF_POWER_LOWER ", 16)) {
        int lo = 0;
        if (parse_power_dbm(cmd + 16, &lo) == 0)
            fsm_set_uhf_power(f, f->uhf_power[0], lo, 1);
        else
            printf("[FSM] 下层UHF功率命令无效: %s\n", cmd + 16);
        return;
    }

    if (!strncmp(cmd, "FORCE_STATE ", 12)) {
        if (fsm_parse_state_text(cmd + 12, &forced_state) == 0) {
            if (fsm_force_state(f, forced_state) == 0)
                printf("[FSM] 强制状态切换: %s\n", fsm_state_text(forced_state));
            else
                printf("[FSM] 强制状态切换失败: %s\n", cmd + 12);
        } else {
            printf("[FSM] 未知强制状态: %s\n", cmd + 12);
        }
        return;
    }

    if (!strcmp(cmd, "FORCE_WAKE")) {
        if (fsm_force_state(f, FSM_ACTIVE) == 0)
            printf("[FSM] 强制唤醒: ACTIVE\n");
        else
            printf("[FSM] 强制唤醒失败\n");
        return;
    }

    if (!strcmp(cmd, "ADD_START")) {
        if (f->state == FSM_SLEEP) enter_active(f);
        fsm_clear_register_pending(f);
        f->add_mode = 1;
        f->sc.detect_unknown = 1;
        printf("[FSM] 加书模式: 一次只锁定一枚未知 EPC\n");
        return;
    }

    if (!strcmp(cmd, "ADD_CANCEL")) {
        f->add_mode = 0;
        fsm_clear_register_pending(f);
        printf("[FSM] 加书模式取消\n");
        return;
    }

    if (!strncmp(cmd, "REGISTER ", 9)) {
        int layer = 0;
        double start = 0.0, end = 0.0;
        char *title = NULL;
        if (!f->register_pending || !f->pending_epc[0]) return;
        if (parse_register_cmd(cmd, &layer, &start, &end, &title) == 0) {
            if (fsm_register_book(f, f->pending_epc, title, "", layer, start, end) == 0) {
                printf("[FSM] 新书已登记: %s EPC=%s L%d %.1f-%.1f\n",
                       title, f->pending_epc, layer, start, end);
                f->add_mode = 0;
                fsm_clear_register_pending(f);
                /* 重新加载扫描器书目表，避免重启 FSM 才认识新书。 */
                scanner_stop(&f->sc);
                scanner_init(&f->sc, f->db, FSM_UP_DEV, FSM_LO_DEV,
                             f->uhf_power[0], f->uhf_power[1]);
            }
        }
        return;
    }

    if (!strncmp(cmd, "REGISTER_PAGES ", 15)) {
        int layer = 0;
        int pages = 0;
        double start = 0.0, end = 0.0;
        char *title = NULL;
        if (!f->register_pending || !f->pending_epc[0]) return;
        if (parse_register_pages_cmd(cmd, &layer, &pages, &title) == 0) {
            choose_register_position(f, layer, pages, &start, &end);
            if (fsm_register_book_pages(f, f->pending_epc, title, "", layer, start, end, pages) == 0) {
                printf("[FSM] 新书已登记: %s EPC=%s L%d pages=%d %.1f-%.1f\n",
                       title, f->pending_epc, layer, pages, start, end);
                f->add_mode = 0;
                fsm_clear_register_pending(f);
                scanner_stop(&f->sc);
                scanner_init(&f->sc, f->db, FSM_UP_DEV, FSM_LO_DEV,
                             f->uhf_power[0], f->uhf_power[1]);
            }
        }
        return;
    }

    if (!strncmp(cmd, "DELETE_BOOK ", 12)) {
        const char *epc = cmd + 12;
        book_info_t book;
        shelf_item_t shelf;
        int has_book = 0;
        int has_shelf = 0;
        if (!epc[0]) return;

        has_book = db_book_find(f->db, epc, &book) == 1;
        has_shelf = db_shelf_find(f->db, epc, &shelf) == 1;

        scanner_stop(&f->sc);
        db_shelf_delete(f->db, epc);
        if (db_book_delete(f->db, epc) == 0) {
            const char *title = has_book && book.title[0] ? book.title :
                                has_shelf && shelf.title[0] ? shelf.title : epc;
            db_log_add(f->db, epc, title, "deleted",
                       has_shelf ? shelf.layer : -1,
                       has_shelf ? shelf.start_cm : 0.0,
                       has_shelf ? shelf.end_cm : 0.0,
                       has_shelf ? shelf.rssi : 0,
                       "screen delete");
            printf("[FSM] 书籍已删除: %s EPC=%s\n", title, epc);
        } else {
            printf("[FSM] 删除书籍失败: EPC=%s\n", epc);
        }
        if (!strcmp(f->pending_epc, epc)) fsm_clear_register_pending(f);
        scanner_init(&f->sc, f->db, FSM_UP_DEV, FSM_LO_DEV,
                     f->uhf_power[0], f->uhf_power[1]);
        fsm_write_state_file(f);
        return;
    }

    if (!strcmp(cmd, "NET_SCAN")) {
        fsm_wifi_scan_to_file(f);
        return;
    }

    if (!strncmp(cmd, "NET_CONNECT ", 12)) {
        fsm_wifi_connect_cmd(f, cmd + 12);
        return;
    }

    if (!strcmp(cmd, "NET_DISCONNECT")) {
        wifi_disconnect();
        snprintf(f->wifi_last_error, sizeof(f->wifi_last_error), "已断开");
        f->t_last_wifi_status = 0;
        return;
    }

    if (!strcmp(cmd, "FAULT_RETRY")) {
        fsm_retry_fault(f);
        return;
    }

    if (!strcmp(cmd, "SYSTEM_REBOOT") || !strcmp(cmd, "SYSTEM_POWEROFF")) {
        int reboot_req = !strcmp(cmd, "SYSTEM_REBOOT");
        printf("[FSM] 收到系统%s命令, 准备安全停止\n", reboot_req ? "重启" : "关机");
        fsm_save_runtime_snapshot(f, 1);
        fsm_write_state_file(f);
        if (f->mod_mqtt.ok && f->mqtt.connected) mqtt_disconnect(&f->mqtt);
        if (f->mod_uhf.ok) scanner_stop(&f->sc);
        if (f->led_ok) led_status(f, 0, 0, 0);
        sync();
        system(reboot_req ? "reboot" : "poweroff");
        return;
    }
}

static void uhf_safe_startup_prepare(void) {
    static const char *ports[] = {
        FSM_UP_DEV,
        FSM_LO_DEV,
        "/dev/ttyUSB0",
        "/dev/ttyUSB1",
        "/dev/ttyUSB2",
        "/dev/ttyUSB3",
        NULL
    };
    static const uint8_t stop_frame[] = {
        0x52, 0x46, 0x00, 0x00, 0x00, 0x23, 0x00, 0x00, 0x45
    };

    for (const char **p = ports; *p; p++) {
        int fd = open(*p, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) continue;

        struct termios t;
        if (tcgetattr(fd, &t) == 0) {
            cfsetispeed(&t, B115200);
            cfsetospeed(&t, B115200);
            t.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
            t.c_cflag |= CS8 | CREAD | CLOCAL;
            t.c_iflag &= ~(IXON | IXOFF | IXANY);
            t.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
            t.c_oflag &= ~OPOST;
            t.c_cc[VMIN] = 0;
            t.c_cc[VTIME] = 1;
            tcsetattr(fd, TCSANOW, &t);
        }

        for (int i = 0; i < 3; i++) {
            write(fd, stop_frame, sizeof(stop_frame));
            tcdrain(fd);
            usleep(80000);
        }
        tcflush(fd, TCIOFLUSH);
        close(fd);
    }

    usleep(250000);
}

static void enter_fault_reason(fsm_t *f, int fault_dev, const char *text, const char *action) {
    if (!f) return;

    if (f->state != FSM_FAULT) {
        f->prev_state = f->state;
        f->fault_dev = fault_dev;
        uhf_stop_both(f);
    }

    snprintf(f->fault_text, sizeof(f->fault_text), "%s",
             text && text[0] ? text : "系统模块异常");
    snprintf(f->fault_action, sizeof(f->fault_action), "%s",
             action && action[0] ? action : "请检查硬件连接后点击重新检测");
    f->state = FSM_FAULT;
    fsm_write_state_file(f);
}

static void enter_fault(fsm_t *f, int fault_dev) {
    enter_fault_reason(f, fault_dev, "系统模块异常",
                       "请检查硬件连接后点击重新检测");
}

/* ── MQTT 发布辅助 ─────────────────────────────────────── */

static int mqtt_can_publish_shelf_status(fsm_t *f) {
    return f->mod_uhf.enabled && f->mod_uhf.ok && f->sc.book_count > 0;
}

static void mqtt_publish_status(fsm_t *f) {
    if (!f->mod_mqtt.ok || !f->mqtt.connected) return;

    const char *st = (f->state == FSM_ACTIVE) ? "ACTIVE" :
                     (f->state == FSM_SLEEP)  ? "SLEEP"  :
                     (f->state == FSM_FIND)   ? "FIND"   :
                     (f->state == FSM_BOOT)   ? "BOOT"   : "FAULT";

    /*
     * --no-uhf 调试时 scanner 为空。不要向 bookshelf/status 发布空 shelf，
     * 否则云端会把手动/历史书架快照覆盖为 0 本。
     */
    if (!mqtt_can_publish_shelf_status(f)) {
        char buf[256];
        int len = snprintf(buf, sizeof(buf),
            "{\"type\":\"device_status\",\"status\":\"online\",\"state\":\"%s\","
            "\"timestamp\":%u,\"uptime_sec\":%u,\"uhf_enabled\":%s}",
            st, (unsigned)time(NULL),
            (unsigned)((uint32_t)time(NULL) - f->t_start),
            f->mod_uhf.enabled ? "true" : "false");
        if (len >= (int)sizeof(buf)) len = sizeof(buf) - 1;
        mqtt_publish(&f->mqtt, "bookshelf/device/status", buf, len, 0, 0);
        f->t_last_mqtt_status = (uint32_t)time(NULL);
        return;
    }

    int present = 0;
    for (int i = 0; i < f->sc.book_count; i++)
        if (f->sc.is_present[i]) present++;

    char shelf_json[8192];
    json_builder_t jb;
    uint32_t ts = (uint32_t)time(NULL);
    json_init(&jb, shelf_json, sizeof(shelf_json));
    json_obj_open(&jb, NULL);
    json_str(&jb, "type", "snapshot");
    json_str(&jb, "status", "online");
    json_str(&jb, "state", st);
    json_int(&jb, "timestamp", ts);
    json_int(&jb, "uptime_sec", ts - f->t_start);

    json_obj_open(&jb, "shelf");
    json_int(&jb, "total", f->sc.book_count);
    json_int(&jb, "present", present);
    json_int(&jb, "missing", f->sc.book_count - present);
    json_arr_open(&jb, "books");
    for (int i = 0; i < f->sc.book_count; i++) {
        book_info_t I;
        int found = db_book_find(f->db, f->sc.book_epc[i], &I);
        json_obj_open(&jb, NULL);
        json_str(&jb, "epc", f->sc.book_epc[i]);
        json_str(&jb, "title", found ? I.title : f->sc.book_title[i]);
        json_int(&jb, "layer", f->sc.book_layer[i]);
        json_float(&jb, "pos_cm", found ? I.expected_start : 0.0);
        json_bool(&jb, "present", f->sc.is_present[i]);
        json_int(&jb, "rssi", f->sc.is_present[i] ? 180 : 0);
        json_obj_close(&jb);
    }
    json_arr_close(&jb);
    json_obj_close(&jb);

    /* 雷达 */
    const ld2410c_data_t *rd = ld2410c_get_data(&f->radar);
    json_obj_open(&jb, "radar");
    json_bool(&jb, "has_person", rd && rd->target_state >= 1 && rd->target_state <= 3);
    json_int(&jb, "move_dist_cm", rd ? rd->moving_distance_cm : 0);
    json_int(&jb, "still_dist_cm", rd ? rd->still_distance_cm : 0);
    json_obj_close(&jb);

    /* 传感器 (实际读数) */
    json_obj_open(&jb, "sensors");
    {
        double t, h, lx;
        if (sensor_read(f, &t, &h, &lx) == 0) {
            int has_th = (t > -100.0 && h >= 0.0);
            int has_lx = (lx >= 0.0);
            json_float(&jb, "temp_c", has_th ? t : 0.0);
            json_float(&jb, "hum_pct", has_th ? h : 0.0);
            json_float(&jb, "lux", has_lx ? lx : 0.0);
        } else {
            json_int(&jb, "temp_c", 0);
            json_int(&jb, "hum_pct", 0);
            json_int(&jb, "lux", 0);
        }
    }
    json_obj_close(&jb);

    /* 统计 */
    json_obj_open(&jb, "stats");
    json_int(&jb, "events_today", 0);
    json_obj_close(&jb);
    json_obj_close(&jb);

    mqtt_publish(&f->mqtt, "bookshelf/status", shelf_json, json_len(&jb), 1, 1);
    f->t_last_mqtt_status = (uint32_t)time(NULL);
}

static void mqtt_publish_event(fsm_t *f, const char *type,
                                const char *epc, const char *title,
                                int layer, double pos_cm, int rssi) {
    if (!f->mod_mqtt.ok || !f->mqtt.connected) return;

    char buf[768];
    json_builder_t jb;
    json_init(&jb, buf, sizeof(buf));
    json_obj_open(&jb, NULL);
    json_str(&jb, "type", "event");
    json_str(&jb, "event", type);
    json_str(&jb, "epc", epc);
    json_str(&jb, "title", title);
    json_int(&jb, "layer", layer);
    json_float(&jb, "pos_cm", pos_cm);
    json_int(&jb, "rssi", rssi);
    json_int(&jb, "timestamp", (uint32_t)time(NULL));
    json_obj_close(&jb);
    mqtt_publish(&f->mqtt, "bookshelf/event", buf, json_len(&jb), 1, 0);
}

/* ── 传感器读写 (BH1750 + SHT30, I2C2) ─────────────────── */

static int sensor_init(fsm_t *f) {
    f->i2c_fd = open(FSM_SENSOR_I2C, O_RDWR);
    if (f->i2c_fd < 0) return -1;

    if (f->bh1750_ok) {
        /* BH1750: 上电 + 连续高分辨率模式 */
        ioctl(f->i2c_fd, I2C_SLAVE, 0x23);
        uint8_t cmd = 0x01;  /* Power On */
        if (write(f->i2c_fd, &cmd, 1) < 0) f->bh1750_ok = 0;
        usleep(10000);
        cmd = 0x10;  /* Continuous High Res */
        if (write(f->i2c_fd, &cmd, 1) < 0) f->bh1750_ok = 0;
        usleep(180000);  /* 等首次测量完成 */
    }

    f->sensor_ok = f->bh1750_ok || f->sht30_ok;
    f->last_temp = f->last_hum = f->last_lux = -999.0;
    return f->sensor_ok ? 0 : -1;
}

static int sensor_read(fsm_t *f, double *temp, double *hum, double *lux) {
    if (!f->sensor_ok || f->i2c_fd < 0) return -1;
    int ok = 0;

    *temp = -999.0;
    *hum = -1.0;
    *lux = -1.0;

    /* BH1750: 读光照 */
    if (f->bh1750_ok) {
        ioctl(f->i2c_fd, I2C_SLAVE, 0x23);
        uint8_t lbuf[2] = {0};
        if (read(f->i2c_fd, lbuf, 2) == 2) {
            uint16_t raw = (lbuf[0] << 8) | lbuf[1];
            *lux = (double)raw / 1.2;
            ok = 1;
        } else {
            fprintf(stderr, "[FSM] ⚠️ BH1750 读取失败, 暂时禁用光照\n");
            f->bh1750_ok = 0;
        }
    }

    if (!f->sht30_ok) return ok ? 0 : -1;

    /* SHT30: 单次高精度测量, 禁用 clock stretching (0x2400) */
    ioctl(f->i2c_fd, I2C_SLAVE, 0x44);
    uint8_t cmd[2] = {0x24, 0x00};
    if (write(f->i2c_fd, cmd, 2) != 2) {
        fprintf(stderr, "[FSM] ⚠️ SHT30 写失败, 暂时禁用温湿度\n");
        f->sht30_ok = 0;
        return ok ? 0 : -1;
    }
    usleep(25000);
    uint8_t buf[6] = {0};
    if (read(f->i2c_fd, buf, 6) != 6) {
        fprintf(stderr, "[FSM] ⚠️ SHT30 读失败, 暂时禁用温湿度\n");
        f->sht30_ok = 0;
        return ok ? 0 : -1;
    }

    /* CRC-8 校验 (SHT30 datasheet §4.13) */
    for (int b = 0; b < 2; b++) {
        uint8_t crc = 0xFF;
        for (int j = 0; j < 2; j++) {
            crc ^= buf[b*3+j];
            for (int k = 0; k < 8; k++)
                crc = (crc & 0x80) ? ((crc << 1) ^ 0x31) : (crc << 1);
        }
        if (crc != buf[b*3+2]) {
            fprintf(stderr, "[FSM] SHT30 CRC error (byte %d)\n", b);
            f->sht30_ok = 0;
            return ok ? 0 : -1;
        }
    }

    uint16_t rt = (buf[0] << 8) | buf[1];
    uint16_t rh = (buf[3] << 8) | buf[4];
    *temp = -45.0 + 175.0 * ((double)rt / 65535.0);
    *hum  = 100.0 * ((double)rh / 65535.0);
    if (*hum > 100.0) *hum = 100.0;
    if (*hum < 0.0)   *hum = 0.0;

    return 0;
}

static void sensor_close(fsm_t *f) {
    if (f->i2c_fd >= 0) {
        /* BH1750 断电 */
        ioctl(f->i2c_fd, I2C_SLAVE, 0x23);
        uint8_t cmd = 0x00;  /* Power Down */
        write(f->i2c_fd, &cmd, 1);
        close(f->i2c_fd);
        f->i2c_fd = -1;
    }
    f->sensor_ok = 0;
}

static void mqtt_publish_sensors(fsm_t *f) {
    if (!f->mod_mqtt.ok || !f->mqtt.connected) return;

    double temp, hum, lux;
    if (sensor_read(f, &temp, &hum, &lux) == 0) {
        int has_temp_hum = (temp > -100.0 && hum >= 0.0);
        int has_lux = (lux >= 0.0);
        /* 突变检测: 超过阈值立刻推送 (QoS 0, 不占带宽) */
        int sudden = 0;
        if (has_temp_hum && f->last_temp > -100 && fabs(temp - f->last_temp) > 2.0) sudden = 1;
        if (has_temp_hum && f->last_hum  > -100 && fabs(hum  - f->last_hum)  > 10.0) sudden = 1;
        if (has_lux && f->last_lux  > -100 && fabs(lux  - f->last_lux)  > 100.0) sudden = 1;

        if (has_temp_hum) {
            f->last_temp = temp;
            f->last_hum  = hum;
        }
        if (has_lux) f->last_lux = lux;

        /* 写 sensor_log */
        int rs = 0;
        const ld2410c_data_t *rd = ld2410c_get_data(&f->radar);
        if (rd) rs = rd->target_state;
        db_sensor_add(f->db,
                      has_temp_hum ? temp : 0.0,
                      has_temp_hum ? hum : 0.0,
                      has_lux ? lux : 0.0,
                      rs);

        /* MQTT 推送 */
        char buf[256];
        int len = snprintf(buf, sizeof(buf),
            "{\"type\":\"sensors\",\"temp_c\":%.1f,\"hum_pct\":%.1f,"
            "\"lux\":%.0f,\"timestamp\":%u}",
            has_temp_hum ? temp : 0.0,
            has_temp_hum ? hum : 0.0,
            has_lux ? lux : 0.0,
            (unsigned)time(NULL));
        if (len >= (int)sizeof(buf)) len = sizeof(buf) - 1;
        mqtt_publish(&f->mqtt, "bookshelf/sensors", buf, len, 0, 0);

        /* 突变时额外推一次全量快照 */
        if (sudden) mqtt_publish_status(f);
    }

    f->t_last_mqtt_sensor = (uint32_t)time(NULL);
}


/* ── MQTT 下行命令回调 ─────────────────────────────────── */

static void on_mqtt_message(const char *topic, const char *payload,
                             int payload_len, void *user) {
    fsm_t *f = (fsm_t *)user;
    if (!f || !topic) return;

    if (strstr(topic, "cmd/find")) {
        char raw[256];
        char query[128];
        int plen = payload_len > 0 ? payload_len : 0;
        if (plen >= (int)sizeof(raw)) plen = sizeof(raw) - 1;
        if (plen > 0 && payload) memcpy(raw, payload, plen);
        raw[plen] = '\0';

        if (!json_string_field(raw, "query", query, sizeof(query)))
            snprintf(query, sizeof(query), "%.127s", raw);
        printf("[FSM] 📲 收到查找命令: %s\n", query);
        fsm_begin_find(f, query);
    }
    else if (strstr(topic, "cmd/refresh")) {
        printf("[FSM] 📲 收到刷新请求\n");
        mqtt_publish_status(f);
    }
}

/* ── LED 渲染 (亮度渐变, cm精度, 多书叠加) ───────────────── */

static int led_idx(int layer, int cm) {
    if (layer == 0) return 1 + cm;
    else            return 76 - cm;
}

static void led_render(fsm_t *f) {
    for (int i = 0; i < 2; i++)
        shelf_led_clear_position(i);

    uint8_t buf[2][38][3];
    memset(buf, 0, sizeof(buf));

    for (int b = 0; b < f->sc.book_count; b++) {
        book_info_t I;
        shelf_item_t S;
        int have_shelf;
        if (db_book_find(f->db, f->sc.book_epc[b], &I) != 1) continue;
        have_shelf = (db_shelf_find(f->db, f->sc.book_epc[b], &S) == 1);
        int L = have_shelf ? S.layer : f->sc.actual_layer[b];
        int present = have_shelf ? S.is_present : f->sc.is_present[b];
        double start = have_shelf ? S.start_cm : I.expected_start;
        double end   = have_shelf ? S.end_cm : I.expected_end;
        int misplaced = present && I.expected_layer >= 0 && L != I.expected_layer;
        if (start >= end || L < 0 || L > 1) continue;

        uint8_t cr = 0, cg = 0, cb = 0;
        if (f->state == FSM_FIND) {
            /* FIND 模式: 只亮匹配的书 = 黄灯 */
            int matched = 0;
            for (int m = 0; m < f->find_count; m++)
                if (f->find_matches[m] == b) { matched = 1; break; }
            if (matched && present) {
                cr = 255; cg = 255; cb = 0;           /* 匹配在架 = 黄 */
            } else if (matched && !present) {
                cr = 255; cg = 100; cb = 0;           /* 匹配但不在架 = 暗橙 */
            } else {
                continue;                              /* 不匹配 = 不亮 */
            }
        } else if (!present) {
            if (f->missing_since[b] &&
                (uint32_t)time(NULL) - f->missing_since[b] < 8) {
                cr = 0; cg = 0; cb = 255;       /* 刚取走/疑似取走 = 蓝 */
            } else {
                continue;
            }
        } else if (misplaced) {
            cr = 255; cg = 0; cb = 180;         /* 错放 = 粉 */
        } else if (present) {
            cr = 0; cg = 255; cb = 0;           /* 在架 = 绿 */
        } else {
            continue;
        }

        int first = (int)floor(start);
        int last  = (int)ceil(end) - 1;
        if (first < 0) first = 0;
        if (last > 37) last = 37;

        for (int l = first; l <= last; l++) {
            double lo = (l > start) ? (double)l : start;
            double hi = (l+1 < end) ? (double)(l+1) : end;
            double overlap = hi - lo;
            if (overlap <= 0) continue;

            double center = (start + end) / 2.0;
            double dist = fabs((l + 0.5) - center);
            double half = (end - start) / 2.0;
            /* 中央10 → 边缘2, 磨砂胶带漫反射后层次更丰富 */
            double bright = 10.0 - 8.0 * (dist / half);
            if (bright < 2.0) bright = 2.0;
            if (bright > 10.0) bright = 10.0;

            uint8_t v = (uint8_t)(bright * 25.5);  /* 10→255, 2→51 */
            if (cr && buf[L][l][0] < v) buf[L][l][0] = v;
            if (cg && buf[L][l][1] < v) buf[L][l][1] = v;
            if (cb && buf[L][l][2] < v) buf[L][l][2] = v;
        }
    }

    for (int L = 0; L < 2; L++)
        for (int l = 0; l < 38; l++)
            if (buf[L][l][0] || buf[L][l][1] || buf[L][l][2])
                shelf_led_set(led_idx(L, l), buf[L][l][0], buf[L][l][1], buf[L][l][2]);

    shelf_led_show();
}

/* ── UHF 停/启 ────────────────────────────────────────────── */

static const uint8_t stop_frame[] = {0x52,0x46,0x00,0x00,0x00,0x23,0x00,0x00,0x45};

static void uhf_stop_both(fsm_t *f) {
    for (int d = 0; d < 2; d++) {
        if (f->sc.dev[d].fd >= 0) {
            write(f->sc.dev[d].fd, stop_frame, sizeof(stop_frame));
            f->sc.dev[d].scanning = 0;
        }
    }
    usleep(200000);
    for (int d = 0; d < 2; d++)
        if (f->sc.dev[d].fd >= 0) tcflush(f->sc.dev[d].fd, TCIOFLUSH);
}

static int uhf_start_both(fsm_t *f) {
    for (int d = 0; d < 2; d++) {
        if (f->sc.dev[d].fd < 0) continue;
        if (uhf_set_power(&f->sc.dev[d], (uint8_t)f->sc.power[d]) < 0) return -1;
        uhf_tags_clear(&f->sc.dev[d]);
        if (uhf_inventory_start(&f->sc.dev[d]) < 0) return -1;
    }
    return 0;
}

/* ── LED 状态灯 ───────────────────────────────────────────── */

static void led_status(fsm_t *f, uint8_t r, uint8_t g, uint8_t b) {
    if (!f->led_ok) return;
    shelf_led_clear();
    shelf_led_status(0, r, g, b);
    shelf_led_status(1, r, g, b);
    shelf_led_show();
}

/* ══════════════════════════════════════════════════════════════
 * 模块自检
 * ══════════════════════════════════════════════════════════════ */

static int check_led(fsm_t *f) {
    f->led_ok = (shelf_led_init(FSM_LED_SPI, FSM_LED_COUNT) == 0);
    f->mod_led.ok = f->led_ok;
    if (f->led_ok) {
        shelf_led_set_brightness(0);
        shelf_led_all_off();
        shelf_led_set_brightness((uint8_t)f->led_brightness);
    }
    return f->mod_led.ok;
}

static int check_radar(fsm_t *f) {
    f->mod_radar.ok = (ld2410c_init(&f->radar, FSM_RADAR_DEV, FSM_RADAR_BAUD) == 0);
    if (f->mod_radar.ok) {
        ld2410c_set_max_gate(&f->radar,
                             FSM_RADAR_GATE_MOVING,
                             FSM_RADAR_GATE_STILL,
                             FSM_RADAR_UNMANNED_SEC);
        ld2410c_set_sensitivity(&f->radar,
                                FSM_RADAR_SENS_MOVING,
                                FSM_RADAR_SENS_STILL);
    }
    return f->mod_radar.ok;
}

static int check_uhf(fsm_t *f) {
    uhf_safe_startup_prepare();
    f->mod_uhf.ok = (scanner_init(&f->sc, f->db,
                                   FSM_UP_DEV, FSM_LO_DEV,
                                   f->uhf_power[0], f->uhf_power[1]) == 0);
    return f->mod_uhf.ok;
}

static int check_sensor(fsm_t *f) {
    /* 检测 I2C2 上 BH1750(0x23) 和 SHT30(0x44)。SHT30 当前允许缺失。 */
    int fd = open(FSM_SENSOR_I2C, O_RDWR);
    if (fd < 0) { f->mod_sensor.ok = 0; return 0; }

    f->mod_sensor.ok = 0;
    f->bh1750_ok = 0;
    f->sht30_ok = 0;

    /* 探 BH1750 @ 0x23: 发读命令, 不要求实际数据 */
    if (ioctl(fd, I2C_SLAVE, 0x23) < 0) {
        perror("[FSM] I2C BH1750");
    } else {
        uint8_t cmd = 0x01;  /* power on (无害) */
        if (write(fd, &cmd, 1) < 0) {
            fprintf(stderr, "[FSM] ⚠️ BH1750 (0x23) 未响应\n");
        } else {
            f->bh1750_ok = 1;
            f->mod_sensor.ok = 1;
        }
    }

    /* 探 SHT30 @ 0x44: 无 clock-stretching 单次命令。缺失仅告警。 */
    if (ioctl(fd, I2C_SLAVE, 0x44) < 0) {
        perror("[FSM] I2C SHT30");
    } else {
        uint8_t cmd[2] = {0x24, 0x00};
        if (write(fd, cmd, 2) < 0) {
            fprintf(stderr, "[FSM] ⚠️ SHT30 (0x44) 未响应, 温湿度暂不可用\n");
        } else {
            f->sht30_ok = 1;
            f->mod_sensor.ok = 1;
        }
    }

    close(fd);
    return f->mod_sensor.ok;
}

static int check_wifi(fsm_t *f) {
    /* 致命: 检测 wlan0 接口是否存在 (USB 网卡硬件) */
    int fd = open("/sys/class/net/wlan0/operstate", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "[FSM] ❌ WiFi 硬件未找到 (wlan0 不存在)\n");
        f->mod_wifi.ok = 0;
        return 0;  /* 致命 */
    }
    char buf[16] = {0};
    read(fd, buf, sizeof(buf)-1);
    close(fd);
    f->mod_wifi.ok = 1;

    /* 非致命: 是否已关联热点 */
    if (strncmp(buf, "up", 2) != 0) {
        fprintf(stderr, "[FSM] ⚠️ WiFi 未连接热点, 继续本地模式\n");
        return 1;
    }

    /* 标记: 需要时间同步 (主循环延迟执行) */
    printf("[FSM] WiFi 已连接, 时间同步稍后执行\n");

    return 1;
}

static int check_mqtt(fsm_t *f) {
    /* 非致命: 尝试连 Broker, 成功则保持连接 */
    mqtt_init(&f->mqtt, FSM_MQTT_BROKER, FSM_MQTT_USER, FSM_MQTT_PASS,
              on_mqtt_message, f);
    mqtt_set_will(&f->mqtt,
                  f->mod_uhf.enabled ? "bookshelf/status" : "bookshelf/device/status",
                  "{\"status\":\"offline\"}", 1, 1);

    if (mqtt_connect(&f->mqtt) == 0) {
        f->mod_mqtt.ok = 1;
        /* 订阅下行命令主题 */
        mqtt_subscribe(&f->mqtt, "bookshelf/cmd/find", 1);
        mqtt_subscribe(&f->mqtt, "bookshelf/cmd/refresh", 0);
        printf("[MQTT] ✅ Broker 已连接 (%s)\n", FSM_MQTT_BROKER);
        return 1;
    }
    fprintf(stderr, "[FSM] ⚠️ MQTT Broker 不可达, 数据将存本地\n");
    f->mod_mqtt.ok = 0;
    return 1;  /* 非致命 */
}

static int check_screen(fsm_t *f) {
    /* 检测 framebuffer 或 DRM */
    const char *paths[] = {"/dev/fb0", "/dev/dri/card0", NULL};
    for (const char **p = paths; *p; p++) {
        int fd = open(*p, O_RDWR);
        if (fd >= 0) { close(fd); f->mod_screen.ok = 1; return 1; }
    }
    f->mod_screen.ok = 0;
    return 0;
}

/* ── 红外阵列 (TCA9555 + 74HC595) ───────────────────────── */

static int write_text_file(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t n = write(fd, text, strlen(text));
    close(fd);
    return n == (ssize_t)strlen(text) ? 0 : -1;
}

static int gpio_path(int gpio, const char *name, char *buf, size_t len)
{
    int n = snprintf(buf, len, "/sys/class/gpio/gpio%d/%s", gpio, name);
    return n > 0 && (size_t)n < len ? 0 : -1;
}

static int gpio_export_output(int gpio)
{
    char path[128];
    char num[24];
    if (gpio_path(gpio, "value", path, sizeof(path)) < 0) return -1;
    if (access(path, F_OK) != 0) {
        snprintf(num, sizeof(num), "%d", gpio);
        if (write_text_file("/sys/class/gpio/export", num) < 0 && errno != EBUSY)
            return -1;
        for (int i = 0; i < 20 && access(path, F_OK) != 0; i++)
            usleep(10000);
    }
    if (gpio_path(gpio, "direction", path, sizeof(path)) < 0) return -1;
    return write_text_file(path, "out");
}

static int gpio_write_value(int gpio, int value)
{
    char path[128];
    if (gpio_path(gpio, "value", path, sizeof(path)) < 0) return -1;
    return write_text_file(path, value ? "1" : "0");
}

static void ir_pulse(int gpio)
{
    gpio_write_value(gpio, 1);
    usleep(50);
    gpio_write_value(gpio, 0);
    usleep(50);
}

static void ir_shift_byte(uint8_t value)
{
    for (int bit = 7; bit >= 0; bit--) {
        gpio_write_value(FSM_IR_SER_GPIO, (value >> bit) & 1);
        ir_pulse(FSM_IR_SRCLK_GPIO);
    }
    ir_pulse(FSM_IR_RCLK_GPIO);
}

static int ir_gpio_init(fsm_t *f)
{
    if (f->ir_gpio_ok) return 0;
    if (gpio_export_output(FSM_IR_SER_GPIO) < 0 ||
        gpio_export_output(FSM_IR_RCLK_GPIO) < 0 ||
        gpio_export_output(FSM_IR_SRCLK_GPIO) < 0) {
        fprintf(stderr, "[FSM] ⚠️ 红外 GPIO 初始化失败\n");
        return -1;
    }
    gpio_write_value(FSM_IR_SER_GPIO, 0);
    gpio_write_value(FSM_IR_RCLK_GPIO, 0);
    gpio_write_value(FSM_IR_SRCLK_GPIO, 0);
    ir_shift_byte(0x00);
    f->ir_gpio_ok = 1;
    return 0;
}

static int i2c_write_reg8(int fd, uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return write(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf) ? 0 : -1;
}

static int i2c_read_reg8(int fd, uint8_t reg, uint8_t *value)
{
    if (write(fd, &reg, 1) != 1) return -1;
    return read(fd, value, 1) == 1 ? 0 : -1;
}

static int ir_probe_board(fsm_t *f, int board)
{
    const ir_board_cfg_t *cfg = &g_ir_boards[board];
    uint8_t v = 0xff;

    if (!cfg->enabled) {
        f->ir_board_ok[board] = 0;
        return 0;
    }
    if (f->ir_fd < 0) return -1;
    if (ioctl(f->ir_fd, I2C_SLAVE, cfg->addr) < 0) return -1;

    /* TCA9555: 两个端口配置为输入，再读 INPUT0 验证。 */
    i2c_write_reg8(f->ir_fd, 0x06, 0xff);
    i2c_write_reg8(f->ir_fd, 0x07, 0xff);
    if (i2c_read_reg8(f->ir_fd, 0x00, &v) < 0) {
        f->ir_board_ok[board] = 0;
        return -1;
    }
    f->ir_board_ok[board] = 1;
    printf("[FSM] 红外板 %s @0x%02x 在线\n", cfg->name, cfg->addr);
    return 0;
}

static int ir_init(fsm_t *f)
{
    if (ir_gpio_init(f) < 0) return -1;
    f->ir_fd = open(FSM_IR_I2C, O_RDWR);
    if (f->ir_fd < 0) return -1;

    int ok = 0;
    for (int i = 0; i < FSM_IR_BOARD_MAX; i++)
        if (ir_probe_board(f, i) == 0 && f->ir_board_ok[i]) ok++;

    return ok > 0 ? 0 : -1;
}

static int ir_board_scan(fsm_t *f, int board, uint16_t *bitmap)
{
    const ir_board_cfg_t *cfg = &g_ir_boards[board];
    uint16_t detected = 0;

    if (!cfg->enabled || !f->ir_board_ok[board] || f->ir_fd < 0) return -1;
    if (ioctl(f->ir_fd, I2C_SLAVE, cfg->addr) < 0) return -1;

    for (int group = 0; group < 8; group++) {
        uint8_t in0 = 0xff, in1 = 0xff;
        ir_shift_byte((uint8_t)(1u << group));
        usleep(3000);
        if (i2c_read_reg8(f->ir_fd, 0x00, &in0) < 0 ||
            i2c_read_reg8(f->ir_fd, 0x01, &in1) < 0) {
            ir_shift_byte(0x00);
            return -1;
        }
        if (((in0 >> group) & 1) == 0) detected |= (uint16_t)(1u << group);
        if (((in1 >> group) & 1) == 0) detected |= (uint16_t)(1u << (group + 8));
        ir_shift_byte(0x00);
        usleep(500);
    }

    *bitmap = detected;
    return 0;
}

static void ir_channel_range_cm(const ir_board_cfg_t *cfg, int ch,
                                double *start, double *end)
{
    int idx = cfg->reverse ? (15 - ch) : ch;
    double step = (cfg->end_cm - cfg->start_cm) / 16.0;
    if (start) *start = cfg->start_cm + step * idx;
    if (end) *end = cfg->start_cm + step * (idx + 1);
}

static int ir_scan(fsm_t *f)
{
    if (!f || f->ir_fd < 0) return -1;

    static uint32_t last_reprobe[FSM_IR_BOARD_MAX];
    uint32_t now = (uint32_t)time(NULL);
    int any_ok = 0;
    int changed = 0;
    int change_layer = -1;
    double change_start = 999.0;
    double change_end = -1.0;
    int removed_layer = -1;
    double removed_start = 999.0;
    double removed_end = -1.0;
    int added_layer = -1;
    double added_start = 999.0;
    double added_end = -1.0;

    for (int b = 0; b < FSM_IR_BOARD_MAX; b++) {
        uint16_t bitmap = 0;
        const ir_board_cfg_t *cfg = &g_ir_boards[b];
        if (!cfg->enabled) continue;
        if (!f->ir_board_ok[b]) {
            if (now - last_reprobe[b] >= 2) {
                last_reprobe[b] = now;
                if (ir_probe_board(f, b) == 0 && f->ir_board_ok[b]) {
                    f->ir_raw_streak[b] = 0;
                    printf("[FSM] 红外板 %s 已恢复\n", cfg->name);
                }
            }
            if (!f->ir_board_ok[b]) continue;
        }
        if (ir_board_scan(f, b, &bitmap) < 0) {
            fprintf(stderr, "[FSM] ⚠️ 红外板 %s 读取失败\n", cfg->name);
            f->ir_board_ok[b] = 0;
            last_reprobe[b] = now;
            continue;
        }

        any_ok = 1;
        if (bitmap == f->ir_raw_bitmap[b]) {
            if (f->ir_raw_streak[b] < 1000) f->ir_raw_streak[b]++;
        } else {
            f->ir_raw_bitmap[b] = bitmap;
            f->ir_raw_streak[b] = 1;
        }
        if (f->ir_raw_streak[b] < IR_STABLE_SCANS)
            continue;

        uint16_t old_bitmap = f->ir_bitmap[b];
        uint16_t diff = bitmap ^ old_bitmap;
        uint16_t removed = old_bitmap & (uint16_t)~bitmap;
        uint16_t added = bitmap & (uint16_t)~old_bitmap;
        f->ir_prev_bitmap[b] = old_bitmap;
        f->ir_bitmap[b] = bitmap;

        if (diff) {
            changed = 1;
            change_layer = cfg->layer;
            for (int ch = 0; ch < 16; ch++) {
                if (!(diff & (1u << ch))) continue;
                double s, e;
                ir_channel_range_cm(cfg, ch, &s, &e);
                if (s < change_start) change_start = s;
                if (e > change_end) change_end = e;
            }
            if (removed) {
                removed_layer = cfg->layer;
                for (int ch = 0; ch < 16; ch++) {
                    if (!(removed & (1u << ch))) continue;
                    double s, e;
                    ir_channel_range_cm(cfg, ch, &s, &e);
                    if (s < removed_start) removed_start = s;
                    if (e > removed_end) removed_end = e;
                }
            }
            if (added) {
                added_layer = cfg->layer;
                for (int ch = 0; ch < 16; ch++) {
                    if (!(added & (1u << ch))) continue;
                    double s, e;
                    ir_channel_range_cm(cfg, ch, &s, &e);
                    if (s < added_start) added_start = s;
                    if (e > added_end) added_end = e;
                }
            }
            printf("[FSM] 红外变化 %s bitmap=%04x diff=%04x L%d %.1f-%.1fcm\n",
                   cfg->name, bitmap, diff, change_layer, change_start, change_end);
            if (removed_layer >= 0)
                printf("[FSM]   红外空出: L%d %.1f-%.1fcm\n",
                       removed_layer, removed_start, removed_end);
            if (added_layer >= 0)
                printf("[FSM]   红外新增: L%d %.1f-%.1fcm\n",
                       added_layer, added_start, added_end);
        }
    }

    f->ir_changed = changed;
    if (changed) {
        f->ir_change_layer = change_layer;
        f->ir_change_start_cm = change_start;
        f->ir_change_end_cm = change_end;
    }
    if (removed_layer >= 0 && removed_end > removed_start) {
        f->ir_removed_layer = removed_layer;
        f->ir_removed_start_cm = removed_start;
        f->ir_removed_end_cm = removed_end;
        f->t_last_ir_removed = (uint32_t)time(NULL);
    }
    if (added_layer >= 0 && added_end > added_start) {
        f->ir_added_layer = added_layer;
        f->ir_added_start_cm = added_start;
        f->ir_added_end_cm = added_end;
        f->t_last_ir_added = (uint32_t)time(NULL);
    }
    f->t_last_ir_scan = (uint32_t)time(NULL);
    return any_ok ? 0 : -1;
}

static int ir_prime_baseline(fsm_t *f)
{
    if (!f || f->ir_fd < 0) return -1;
    int ok = 0;
    for (int b = 0; b < FSM_IR_BOARD_MAX; b++) {
        uint16_t bitmap = 0;
        const ir_board_cfg_t *cfg = &g_ir_boards[b];
        if (!cfg->enabled || !f->ir_board_ok[b]) continue;
        if (ir_board_scan(f, b, &bitmap) == 0) {
            f->ir_bitmap[b] = bitmap;
            f->ir_prev_bitmap[b] = bitmap;
            f->ir_raw_bitmap[b] = bitmap;
            f->ir_raw_streak[b] = IR_STABLE_SCANS;
            printf("[FSM] 红外基线 %s bitmap=%04x\n", cfg->name, bitmap);
            ok++;
        }
    }
    f->ir_changed = 0;
    f->ir_removed_layer = -1;
    f->ir_added_layer = -1;
    return ok > 0 ? 0 : -1;
}

/* ── 自检主流程 ─────────────────────────────────────────── */

static int run_selfcheck(fsm_t *f) {
    printf("[FSM] ═══ 自检开始 ═══\n");

    typedef struct { fsm_mod_t *m; const char *name; int (*fn)(fsm_t*); } check_t;
    check_t checks[] = {
        {&f->mod_led,    "LED灯带",  check_led},
        {&f->mod_radar,  "雷达",     check_radar},
        {&f->mod_wifi,   "WiFi",     check_wifi},
        {&f->mod_sensor, "传感器",   check_sensor},
        {&f->mod_mqtt,   "MQTT",     check_mqtt},
        {&f->mod_screen, "屏幕",     check_screen},
        {&f->mod_uhf,    "UHF RFID", check_uhf},
        {NULL, NULL, NULL}
    };

    for (check_t *c = checks; c->fn; c++) {
        if (c->m->ok == 1) {
            printf("[FSM]   %s  ✅\n", c->name);
            continue;
        }
        if (c->m->enabled) {
            int fatal = (c->m != &f->mod_mqtt &&
                         c->m != &f->mod_wifi &&
                         c->m != &f->mod_sensor);
            int r = c->fn(f);
            if (!r && fatal) {
                fprintf(stderr, "[FSM] ❌ %s 自检失败\n", c->name);
                if (c->m == &f->mod_uhf) {
                    char text[256];
                    snprintf(text, sizeof(text),
                             "UHF 初始化失败：上层%s，下层%s",
                             access(FSM_UP_DEV, F_OK) == 0 ? "设备存在但打开失败" : "设备未出现",
                             access(FSM_LO_DEV, F_OK) == 0 ? "设备存在但打开失败" : "设备未出现");
                    enter_fault_reason(f, 0, text,
                                       "检查 USB/CH341 和两个 UHF 模块，确认 1.2/1.3 设备节点出现后点击重新检测");
                } else if (c->m == &f->mod_radar) {
                    enter_fault_reason(f, -1, "雷达模块初始化失败",
                                       "检查 /dev/ttyS1 雷达连接后点击重新检测");
                } else if (c->m == &f->mod_led) {
                    enter_fault_reason(f, -1, "LED 灯带初始化失败",
                                       "检查 /dev/spidev1.0 和灯带供电后点击重新检测");
                } else if (c->m == &f->mod_screen) {
                    enter_fault_reason(f, -1, "屏幕模块初始化失败",
                                       "检查显示程序和屏幕设备后点击重新检测");
                } else {
                    enter_fault_reason(f, -1, "模块自检失败",
                                       "检查硬件连接后点击重新检测");
                }
                led_status(f, 255, 0, 0);
                return -1;
            }
        }
        printf("[FSM]   %s  %s\n", c->name,
               c->m->enabled ? (c->m->ok ? "✅" : "⚠️离线") : "⏭️跳过");
    }

    printf("[FSM] ═══ 自检通过 ═══\n");
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 * FSM: init / baseline / tick / stop
 * ══════════════════════════════════════════════════════════════ */

void fsm_enable_all(fsm_t *f) {
    f->mod_led.enabled    = 1;
    f->mod_radar.enabled  = 1;
    f->mod_uhf.enabled    = 1;
    f->mod_wifi.enabled   = 1;
    f->mod_sensor.enabled = 1;
    f->mod_mqtt.enabled   = 1;
    f->mod_screen.enabled = 1;
}

int fsm_init(fsm_t *f, db_ctx_t *db) {
    if (!f || !db) return -1;

    /* 保存模块开关 (main 已通过 fsm_enable_all + --no-xxx 设好) */
    fsm_mod_t saved_mods[7];
    memcpy(&saved_mods[0], &f->mod_led, sizeof(fsm_mod_t) * 7);

    memset(f, 0, sizeof(*f));
    memcpy(&f->mod_led, saved_mods, sizeof(fsm_mod_t) * 7);
    f->db = db;
    f->i2c_fd = -1;
    f->ir_fd = -1;
    f->sc.dev[0].fd = -1;
    f->sc.dev[1].fd = -1;
    f->state      = FSM_BOOT;
    f->prev_state = FSM_BOOT;
    f->fault_dev  = -1;
    f->pending_layer = -1;
    f->led_brightness = FSM_BRIGHT;
    f->uhf_power[0] = FSM_UP_PWR;
    f->uhf_power[1] = FSM_LO_PWR;
    f->verbose    = 1;
    f->t_start    = (uint32_t)time(NULL);
    {
        char meta[128];
        if (db_meta_get(db, "shelf_issue", meta, sizeof(meta)) == 1 && atoi(meta) != 0) {
            f->shelf_issue = 1;
            if (db_meta_get(db, "shelf_issue_text", meta, sizeof(meta)) == 1)
                snprintf(f->shelf_issue_text, sizeof(f->shelf_issue_text), "%s", meta);
            if (!f->shelf_issue_text[0])
                snprintf(f->shelf_issue_text, sizeof(f->shelf_issue_text), "书架状态需要人工确认");
        }
        if (db_meta_get(db, "clean_shutdown", meta, sizeof(meta)) == 1 && strcmp(meta, "1") != 0) {
            printf("[FSM] ⚠️ 上次可能不是正常关机, 将用持久化书架状态做开机基线对比\n");
        }
        if (db_meta_get(db, "led_brightness", meta, sizeof(meta)) == 1) {
            int brightness = 0;
            if (parse_int_0_255(meta, &brightness) == 0)
                f->led_brightness = brightness;
        }
        if (db_meta_get(db, "uhf_power_upper", meta, sizeof(meta)) == 1) {
            int power = 0;
            if (parse_power_dbm(meta, &power) == 0)
                f->uhf_power[0] = power;
        }
        if (db_meta_get(db, "uhf_power_lower", meta, sizeof(meta)) == 1) {
            int power = 0;
            if (parse_power_dbm(meta, &power) == 0)
                f->uhf_power[1] = power;
        }
        db_meta_set(db, "clean_shutdown", "0");
        db_meta_set(db, "boot_started_at", "running");
    }

    /* ── LED 先初始化 ── */
    check_led(f);
    fsm_write_state_file(f);

    /* ── 品红 — 自检 ── */
    led_status(f, 255, 0, 255);
    if (run_selfcheck(f) < 0) return -1;

    /* ── 归档库 (SD卡, 非致命) ── */
    f->archive_ok = (db_archive_open(&f->db_archive, FSM_DB_ARCHIVE) == 0);
    if (f->archive_ok)
        printf("[FSM] 归档库已连接 (%s)\n", FSM_DB_ARCHIVE);
    else
        fprintf(stderr, "[FSM] ⚠️ SD 归档库不可用, 数据仅存本地\n");
    f->t_last_archive = (uint32_t)time(NULL);

    /* ── 检查 DB 文件大小 ── */
    db_check_size(FSM_DB_LOCAL,
                  f->archive_ok ? FSM_DB_ARCHIVE : NULL);

    /* ── 传感器 (非致命) ── */
    if (f->mod_sensor.enabled && f->mod_sensor.ok) {
        if (sensor_init(f) == 0)
            printf("[FSM] 传感器已就绪 (%s) BH1750=%d SHT30=%d\n",
                   FSM_SENSOR_I2C, f->bh1750_ok, f->sht30_ok);
        else
            fprintf(stderr, "[FSM] ⚠️ 传感器读取初始化失败\n");
    }

    /* ── 红外定位 (非致命, 当前只启用 upper_right @0x20) ── */
    if (ir_init(f) == 0) {
        ir_prime_baseline(f);
        printf("[FSM] 红外定位已就绪 (%s)\n", FSM_IR_I2C);
    } else {
        fprintf(stderr, "[FSM] ⚠️ 红外定位不可用, 继续 UHF 模式\n");
    }

    /* ── 橙 — 基线 ── */
    printf("[FSM] BOOT %d books loaded, baseline...\n", f->sc.book_count);
    led_status(f, 255, 128, 0);
    return 0;
}

/* ── 基线: 启动时只建立初始可见集合, 不因瞬时漏读改低功率或判缺书 ── */

static int layer_all_seen(fsm_t *f, int L) {
    for (int i = 0; i < f->sc.book_count; i++) {
        if (f->sc.book_layer[i] != L) continue;
        if (f->sc.last_seen[i] == 0) return 0;
    }
    return 1;
}

static int baseline(fsm_t *f) {
    uint32_t now = (uint32_t)time(NULL);
    int seen = 0;

    if (f->baseline_step_ts[0] == 0) {
        for (int L = 0; L < 2; L++) {
            f->baseline_step[L]    = 0;
            f->baseline_step_ts[L] = f->t_start;
        }
    }

    for (int i = 0; i < f->sc.book_count; i++)
        if (f->sc.last_seen[i] > 0) { f->sc.is_present[i] = 1; seen++; }

    for (int L = 0; L < 2; L++) {
        if (f->baseline_locked[L]) continue;
        if (layer_all_seen(f, L)) {
            f->baseline_locked[L] = 1;
            printf("[FSM] %s层锁定 %ddBm ✅\n",
                   (L == 0) ? "上" : "下", f->sc.power[L]);
        }
    }

    if (f->baseline_locked[0] && f->baseline_locked[1]) {
        printf("[FSM] ✅基线 %d/%d (%.1fs) 上%ddBm/下%ddBm\n",
               seen, f->sc.book_count, (double)(now - f->t_start),
               f->sc.power[0], f->sc.power[1]);
        for (int i = 0; i < f->sc.book_count; i++) {
            shelf_item_t prev;
            double start = 0.0, end = 0.0;
            book_expected_pos(f, f->sc.book_epc[i], &start, &end);
            if (db_shelf_find(f->db, f->sc.book_epc[i], &prev) == 1) {
                if (!prev.is_present) {
                    char detail[160];
                    snprintf(detail, sizeof(detail), "上次关机记录为离架, 本次开机 UHF 读到");
                    db_log_add(f->db, f->sc.book_epc[i], f->sc.book_title[i],
                               "baseline_mismatch", prev.layer, prev.start_cm,
                               prev.end_cm, f->sc.last_rssi[i], detail);
                }
                f->sc.is_present[i] = prev.is_present;
                f->sc.actual_layer[i] = prev.layer;
            } else {
                f->sc.is_present[i] = 1;
                db_shelf_upsert(f->db, f->sc.book_epc[i], f->sc.book_title[i], "",
                                f->sc.book_layer[i], start, end, 0, 1);
            }
        }
        f->t_last_present = now;
        fsm_save_runtime_snapshot(f, 0);
        return 1;
    }

    if (now - f->t_start > FSM_BASELINE_TO) {
        printf("[FSM] ═══ 基线超时 %d秒 ═══\n", FSM_BASELINE_TO);
        for (int i = 0; i < f->sc.book_count; i++) {
            double start = 0.0, end = 0.0;
            book_expected_pos(f, f->sc.book_epc[i], &start, &end);
            if (f->sc.last_seen[i] == 0) {
                shelf_item_t prev;
                printf("  ❌缺: %s L%d\n", f->sc.book_title[i], f->sc.book_layer[i]);
                if (db_shelf_find(f->db, f->sc.book_epc[i], &prev) == 1) {
                    if (prev.is_present) {
                        db_log_add(f->db, f->sc.book_epc[i], f->sc.book_title[i],
                                   "baseline_mismatch", prev.layer, prev.start_cm,
                                   prev.end_cm, 0, "上次关机记录为在架, 本次开机 UHF 未读到");
                    }
                    f->sc.is_present[i] = prev.is_present;
                    f->sc.actual_layer[i] = prev.layer;
                    printf("     保持上次状态: L%d %.1f-%.1f present=%d\n",
                           prev.layer, prev.start_cm, prev.end_cm, prev.is_present);
                } else {
                    f->sc.is_present[i] = 0;
                    db_shelf_upsert(f->db, f->sc.book_epc[i], f->sc.book_title[i], "",
                                    f->sc.book_layer[i], start, end, 0, 0);
                }
            } else {
                shelf_item_t prev;
                if (db_shelf_find(f->db, f->sc.book_epc[i], &prev) == 1) {
                    if (!prev.is_present) {
                        db_log_add(f->db, f->sc.book_epc[i], f->sc.book_title[i],
                                   "baseline_mismatch", prev.layer, prev.start_cm,
                                   prev.end_cm, f->sc.last_rssi[i],
                                   "上次关机记录为离架, 本次开机 UHF 读到");
                    }
                    f->sc.is_present[i] = prev.is_present;
                    f->sc.actual_layer[i] = prev.layer;
                } else {
                    f->sc.is_present[i] = 1;
                    db_shelf_upsert(f->db, f->sc.book_epc[i], f->sc.book_title[i], "",
                                    f->sc.book_layer[i], start, end, 0, 1);
                }
            }
        }
        printf("[FSM] ⚠️ 基线部分完成, 继续 ACTIVE\n");
        f->t_last_present = now;
        fsm_save_runtime_snapshot(f, 0);
        return 1;
    }

    if (f->verbose >= 2)
        printf("[FSM] 基线中... %d/%d (%.0fs) 上%s/%ddBm 下%s/%ddBm\n",
               seen, f->sc.book_count, (double)(now - f->t_start),
               f->baseline_locked[0] ? "锁" : "扫", f->sc.power[0],
               f->baseline_locked[1] ? "锁" : "扫", f->sc.power[1]);
    return 0;
}

/* ══════════════════════════════════════════════════════════════
 * ACTIVE ↔ SLEEP ↔ FIND
 * ══════════════════════════════════════════════════════════════ */

static void enter_active(fsm_t *f) {
    printf("[FSM] ▶ ACTIVE (有人)\n");
    f->prev_state = f->state;
    f->state = FSM_ACTIVE;
    if (f->prev_state == FSM_SLEEP) {
        if (uhf_start_both(f) < 0) { enter_fault(f, -1); return; }
        printf("[FSM] UHF 已重启\n");
    }
    f->t_last_present = (uint32_t)time(NULL);
    mqtt_publish_status(f);  /* 状态切换立刻推送 */
}

static void enter_sleep(fsm_t *f) {
    printf("[FSM] 💤 SLEEP (无人 %ds)\n", FSM_SLEEP_TIMEOUT);
    f->prev_state = f->state;
    f->state = FSM_SLEEP;
    uhf_stop_both(f);
    led_status(f, 0, 0, 50);  /* 暗蓝 */
    mqtt_publish_status(f);   /* 状态切换立刻推送 */
}

static void enter_find(fsm_t *f) {
    printf("[FSM] 🔍 FIND (找书)\n");

    int already_in_find = (f->state == FSM_FIND);
    if (!already_in_find) {
        f->prev_state = f->state;
        f->state = FSM_FIND;
        f->t_find_start = (uint32_t)time(NULL);
        f->find_status = 1;
        /* 从 SLEEP 进来需唤醒 UHF */
        if (f->prev_state == FSM_SLEEP) {
            if (uhf_start_both(f) < 0) { enter_fault(f, -1); return; }
            printf("[FSM] UHF 已唤醒 (FIND)\n");
        }
    }

    /* 搜索匹配书籍 (按书名或 EPC), 支持追加 */
    if (f->find_query[0]) {
        int new_matches = 0;
        for (int i = 0; i < f->sc.book_count; i++) {
            if (strstr(f->sc.book_title[i], f->find_query) ||
                strstr(f->sc.book_epc[i], f->find_query)) {
                /* 查重 */
                int dup = 0;
                for (int j = 0; j < f->find_count; j++)
                    if (f->find_matches[j] == i) { dup = 1; break; }
                if (!dup && f->find_count < 64) {
                    f->find_matches[f->find_count++] = i;
                    snprintf(f->find_target_epc, sizeof(f->find_target_epc), "%s",
                             f->sc.book_epc[i]);
                    snprintf(f->find_target_title, sizeof(f->find_target_title), "%s",
                             f->sc.book_title[i]);
                    new_matches++;
                    printf("[FSM]   ✅ 找到: %s L%d\n",
                           f->sc.book_title[i], f->sc.book_layer[i]);
                }
            }
        }
        if (new_matches == 0 && !already_in_find)
            printf("[FSM]   ❌ 未找到匹配\n");
    }
}

static void leave_find(fsm_t *f, int cancelled) {
    if (!f) return;

    printf("[FSM] 🔍 %s FIND\n", cancelled ? "取消" : "结束");
    f->find_status = cancelled ? 4 : 0;
    f->find_count = 0;
    f->find_query[0] = '\0';
    f->find_target_epc[0] = '\0';
    f->find_target_title[0] = '\0';

    if (f->state != FSM_FIND) return;

    if (f->prev_state == FSM_SLEEP)
        enter_sleep(f);
    else
        enter_active(f);
}

static int book_expected_pos(fsm_t *f, const char *epc, double *start, double *end)
{
    book_info_t info;
    if (!f || !epc || db_book_find(f->db, epc, &info) != 1) {
        if (start) *start = 0.0;
        if (end) *end = 0.0;
        return 0;
    }
    if (start) *start = info.expected_start;
    if (end) *end = info.expected_end;
    return 1;
}

typedef struct {
    double start;
    double end;
} cm_range_t;

static int book_expected_range(fsm_t *f, int book_idx, double *start, double *end)
{
    if (!f || book_idx < 0 || book_idx >= f->sc.book_count) return 0;
    return book_expected_pos(f, f->sc.book_epc[book_idx], start, end);
}

static int range_overlap(double a0, double a1, double b0, double b1)
{
    double s = a0 > b0 ? a0 : b0;
    double e = a1 < b1 ? a1 : b1;
    return e > s + 0.05;
}

static double range_overlap_cm(double a0, double a1, double b0, double b1)
{
    double s = a0 > b0 ? a0 : b0;
    double e = a1 < b1 ? a1 : b1;
    return e > s ? e - s : 0.0;
}

static int uhf_missing_is_recent(fsm_t *f, int idx, uint32_t now)
{
    if (!f || idx < 0 || idx >= 64) return 1;
    if (!f->missing_since[idx]) return 1;
    return now - f->missing_since[idx] < 6;
}

static int recent_removed_ir_matches_book(fsm_t *f, int idx, uint32_t now,
                                          double *out_s, double *out_e)
{
    shelf_item_t s;
    double bs, be, width;
    int layer;

    if (!f || idx < 0 || idx >= f->sc.book_count) return 0;
    bs = f->op_before_start[idx];
    be = f->op_before_end[idx];
    layer = f->op_before_layer[idx];
    if (db_shelf_find(f->db, f->sc.book_epc[idx], &s) == 1) {
        bs = s.start_cm;
        be = s.end_cm;
        layer = s.layer;
    }
    width = be - bs;
    if (width <= 0.1) width = 1.2;

    for (int b = 0; b < FSM_IR_BOARD_MAX; b++) {
        const ir_board_cfg_t *cfg = &g_ir_boards[b];
        double rs, re, overlap;
        if (!cfg->enabled || cfg->layer != layer) continue;
        if (!f->recent_removed_ir[b] || !f->t_recent_removed_ir[b]) continue;
        if (now - f->t_recent_removed_ir[b] > 8) continue;
        rs = f->recent_removed_start_cm[b];
        re = f->recent_removed_end_cm[b];
        if (!(re > rs)) continue;
        overlap = range_overlap_cm(bs, be, rs, re);
        if (overlap >= 0.30 || overlap >= width * 0.25) {
            if (out_s) *out_s = rs;
            if (out_e) *out_e = re;
            return 1;
        }
    }
    return 0;
}

static int recent_removed_ir_same_layer(fsm_t *f, int layer, uint32_t now,
                                        double *out_s, double *out_e)
{
    if (!f || layer < 0) return 0;
    for (int b = 0; b < FSM_IR_BOARD_MAX; b++) {
        const ir_board_cfg_t *cfg = &g_ir_boards[b];
        if (!cfg->enabled || cfg->layer != layer) continue;
        if (!f->recent_removed_ir[b] || !f->t_recent_removed_ir[b]) continue;
        if (now - f->t_recent_removed_ir[b] > 8) continue;
        if (!(f->recent_removed_end_cm[b] > f->recent_removed_start_cm[b])) continue;
        if (out_s) *out_s = f->recent_removed_start_cm[b];
        if (out_e) *out_e = f->recent_removed_end_cm[b];
        return 1;
    }
    return 0;
}

static void confirm_book_removed(fsm_t *f, int idx, uint32_t now, const char *detail)
{
    shelf_item_t s;
    int layer;
    double start;
    double end;

    if (!f || idx < 0 || idx >= f->sc.book_count) return;
    layer = f->op_before_layer[idx];
    start = f->op_before_start[idx];
    end = f->op_before_end[idx];
    if (db_shelf_find(f->db, f->sc.book_epc[idx], &s) == 1) {
        layer = s.layer;
        start = s.start_cm;
        end = s.end_cm;
    }
    db_shelf_upsert(f->db, f->sc.book_epc[idx], f->sc.book_title[idx], "",
                    layer, start, end, 0, 0);
    db_log_add(f->db, f->sc.book_epc[idx], f->sc.book_title[idx],
               "borrowed", layer, start, end, 0,
               detail ? detail : "UHF missing confirmed");
    f->missing_since[idx] = now;
    f->sc.is_present[idx] = 0;
    mqtt_publish_event(f, "borrowed", f->sc.book_epc[idx], f->sc.book_title[idx],
                       layer, start, 0);
}

static int removed_ir_matches_book(fsm_t *f, int idx, uint16_t removed_bits,
                                   double removed_s, double removed_e)
{
    double overlap;
    double width;
    if (!f || idx < 0 || !removed_bits || !(removed_e > removed_s)) return 0;
    overlap = range_overlap_cm(f->op_before_start[idx], f->op_before_end[idx],
                               removed_s, removed_e);
    width = f->op_before_end[idx] - f->op_before_start[idx];
    if (width <= 0.1) width = 1.2;
    return overlap >= 0.35 || overlap >= width * 0.35;
}

static void restore_transient_missing(fsm_t *f, int idx, const char *detail)
{
    if (!f || idx < 0 || idx >= f->sc.book_count) return;
    f->sc.is_present[idx] = 1;
    f->sc.actual_layer[idx] = f->op_before_layer[idx];
    db_shelf_upsert(f->db, f->sc.book_epc[idx], f->sc.book_title[idx], "",
                    f->op_before_layer[idx], f->op_before_start[idx],
                    f->op_before_end[idx], f->sc.last_rssi[idx], 1);
    db_log_add(f->db, f->sc.book_epc[idx], f->sc.book_title[idx],
               "ignored", f->op_before_layer[idx], f->op_before_start[idx],
               f->op_before_end[idx], f->sc.last_rssi[idx],
               detail ? detail : "ignored transient UHF missing");
}

static double range_width(cm_range_t r)
{
    return r.end > r.start ? r.end - r.start : 0.0;
}

static uint16_t ir_fill_single_gaps(uint16_t bitmap)
{
    uint16_t out = bitmap;
    for (int ch = 1; ch < 15; ch++) {
        uint16_t bit = (uint16_t)(1u << ch);
        if ((bitmap & bit) == 0 &&
            (bitmap & (uint16_t)(1u << (ch - 1))) &&
            (bitmap & (uint16_t)(1u << (ch + 1)))) {
            out |= bit;
        }
    }
    return out;
}

static int ir_occupied_ranges_for_board(const ir_board_cfg_t *cfg,
                                        uint16_t bitmap,
                                        cm_range_t *ranges,
                                        int max_ranges)
{
    cm_range_t intervals[16];
    int n = 0;

    for (int ch = 0; ch < 16; ch++) {
        if (!(bitmap & (1u << ch))) continue;
        ir_channel_range_cm(cfg, ch, &intervals[n].start, &intervals[n].end);
        n++;
    }
    if (n == 0) return 0;

    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (intervals[j].start < intervals[i].start) {
                cm_range_t tmp = intervals[i];
                intervals[i] = intervals[j];
                intervals[j] = tmp;
            }
        }
    }

    int out = 0;
    for (int i = 0; i < n; i++) {
        if (out == 0 || intervals[i].start > ranges[out - 1].end + 0.08) {
            if (out >= max_ranges) break;
            ranges[out++] = intervals[i];
        } else if (intervals[i].end > ranges[out - 1].end) {
            ranges[out - 1].end = intervals[i].end;
        }
    }
    return out;
}

/* ── 操作窗口: UHF=身份, 红外=位置变化, DB=历史约束 ───────── */

static void op_capture_before(fsm_t *f)
{
    if (!f) return;
    for (int b = 0; b < FSM_IR_BOARD_MAX; b++) {
        f->op_before_ir[b] = f->ir_prev_bitmap[b];
        f->op_after_ir[b] = f->ir_bitmap[b];
        f->op_seen_removed_ir[b] = 0;
        f->op_seen_added_ir[b] = 0;
    }

    for (int i = 0; i < f->sc.book_count && i < 64; i++) {
        shelf_item_t s;
        book_info_t info;
        if (db_shelf_find(f->db, f->sc.book_epc[i], &s) == 1) {
            f->op_before_present[i] = s.is_present;
            f->op_before_layer[i] = s.layer;
            f->op_before_start[i] = s.start_cm;
            f->op_before_end[i] = s.end_cm;
        } else if (db_book_find(f->db, f->sc.book_epc[i], &info) == 1) {
            f->op_before_present[i] = f->sc.is_present[i];
            f->op_before_layer[i] = info.expected_layer;
            f->op_before_start[i] = info.expected_start;
            f->op_before_end[i] = info.expected_end;
        } else {
            f->op_before_present[i] = f->sc.is_present[i];
            f->op_before_layer[i] = f->sc.book_layer[i];
            f->op_before_start[i] = 0.0;
            f->op_before_end[i] = 0.0;
        }
    }
}

static void op_range_from_bits(const ir_board_cfg_t *cfg, uint16_t bits,
                               double *start, double *end)
{
    double s = 999.0, e = -1.0;
    for (int ch = 0; ch < 16; ch++) {
        if (!(bits & (1u << ch))) continue;
        double cs, ce;
        ir_channel_range_cm(cfg, ch, &cs, &ce);
        if (cs < s) s = cs;
        if (ce > e) e = ce;
    }
    if (start) *start = s;
    if (end) *end = e;
}

static int op_changed_board(fsm_t *f)
{
    int best = -1;
    int best_bits = 0;
    for (int b = 0; b < FSM_IR_BOARD_MAX; b++) {
        const ir_board_cfg_t *cfg = &g_ir_boards[b];
        if (!cfg->enabled || !f->ir_board_ok[b]) continue;
        uint16_t diff = (uint16_t)(f->op_before_ir[b] ^ f->op_after_ir[b]);
        diff |= f->op_seen_removed_ir[b];
        diff |= f->op_seen_added_ir[b];
        int bits = __builtin_popcount((unsigned)diff);
        if (bits > best_bits) {
            best_bits = bits;
            best = b;
        }
    }
    return best;
}

static int op_book_width(fsm_t *f, int idx, double *width)
{
    double w = 0.0;
    if (!f || idx < 0 || idx >= f->sc.book_count || !width) return 0;
    {
        book_info_t info;
        if (db_book_find(f->db, f->sc.book_epc[idx], &info) == 1) {
            if (info.expected_end > info.expected_start + 0.1)
                w = info.expected_end - info.expected_start;
            else if (info.pages > 0)
                w = info.pages * 0.0055;
        }
    }
    if (w <= 0.1 && f->op_before_end[idx] > f->op_before_start[idx])
        w = f->op_before_end[idx] - f->op_before_start[idx];
    if (w <= 0.1) w = 1.2;
    if (w < 0.6) w = 0.6;
    if (w > 8.0) w = 8.0;
    *width = w;
    return 1;
}

static void op_clamp_fixed_range(const ir_board_cfg_t *cfg, double width, double *s, double *e)
{
    if (!s || !e) return;
    if (width <= 0.1) width = 1.2;
    *e = *s + width;
    if (cfg) {
        if (*e > cfg->end_cm) {
            *e = cfg->end_cm;
            *s = *e - width;
        }
        if (*s < cfg->start_cm) {
            *s = cfg->start_cm;
            *e = *s + width;
        }
    }
}

static void op_reflow_board_from_ir(fsm_t *f, int board, const char *reason)
{
    const ir_board_cfg_t *cfg = &g_ir_boards[board];
    cm_range_t runs[32];
    int books[SCANNER_MAX_BOOKS];
    double widths[SCANNER_MAX_BOOKS];
    double before_s[SCANNER_MAX_BOOKS];
    double before_e[SCANNER_MAX_BOOKS];
    double prefix[SCANNER_MAX_BOOKS + 1];
    double dp[33][SCANNER_MAX_BOOKS + 1];
    int prev[33][SCANNER_MAX_BOOKS + 1];
    int assign_start[32];
    int assign_end[32];
    int n = 0;
    int run_count = 0;
    double total_width = 0.0;
    double ir_width = 0.0;
    const double INF = 1e12;

    if (!f || board < 0 || board >= FSM_IR_BOARD_MAX) return;
    if (!cfg->enabled || !f->ir_board_ok[board]) return;

    for (int b = 0; b < FSM_IR_BOARD_MAX; b++) {
        const ir_board_cfg_t *bcfg = &g_ir_boards[b];
        cm_range_t bruns[16];
        uint16_t bitmap;
        int rn;
        if (!bcfg->enabled || !f->ir_board_ok[b] || bcfg->layer != cfg->layer)
            continue;
        bitmap = ir_fill_single_gaps(f->op_after_ir[b]);
        rn = ir_occupied_ranges_for_board(bcfg, bitmap, bruns, 16);
        for (int r = 0; r < rn && run_count < 32; r++)
            runs[run_count++] = bruns[r];
    }
    if (run_count <= 0) return;

    for (int i = 0; i < run_count; i++) {
        for (int j = i + 1; j < run_count; j++) {
            if (runs[j].start < runs[i].start) {
                cm_range_t tmp = runs[i];
                runs[i] = runs[j];
                runs[j] = tmp;
            }
        }
    }
    {
        int out = 0;
        for (int i = 0; i < run_count; i++) {
            if (runs[i].start < 0.0) runs[i].start = 0.0;
            if (runs[i].end > 38.0) runs[i].end = 38.0;
            if (!(runs[i].end > runs[i].start)) continue;
            if (out == 0 || runs[i].start > runs[out - 1].end + 0.08) {
                runs[out++] = runs[i];
            } else if (runs[i].end > runs[out - 1].end) {
                runs[out - 1].end = runs[i].end;
            }
        }
        run_count = out;
    }
    if (run_count <= 0) return;

    for (int r = 0; r < run_count; r++)
        ir_width += range_width(runs[r]);

    for (int i = 0; i < f->sc.book_count && i < SCANNER_MAX_BOOKS; i++) {
        shelf_item_t s;
        int present = f->sc.is_present[i];
        int layer = f->sc.actual_layer[i];
        double bs = f->op_before_start[i], be = f->op_before_end[i];

        if (db_shelf_find(f->db, f->sc.book_epc[i], &s) == 1) {
            present = s.is_present;
            layer = s.layer;
            bs = s.start_cm;
            be = s.end_cm;
        }
        if (!present || layer != cfg->layer) continue;
        if (bs < 0.0) bs = 0.0;
        if (be > 38.0) be = 38.0;
        if (!(be > bs)) {
            book_expected_range(f, i, &bs, &be);
            if (bs < 0.0) bs = 0.0;
            if (be > 38.0) be = 38.0;
        }
        before_s[n] = bs;
        before_e[n] = be;
        books[n++] = i;
    }

    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            if (before_s[j] < before_s[i]) {
                int tmp = books[i];
                double ds;
                books[i] = books[j];
                books[j] = tmp;
                ds = before_s[i];
                before_s[i] = before_s[j];
                before_s[j] = ds;
                ds = before_e[i];
                before_e[i] = before_e[j];
                before_e[j] = ds;
            }
        }
    }

    for (int k = 0; k < n; k++) {
        double width = 1.2;
        op_book_width(f, books[k], &width);
        widths[k] = width;
        total_width += widths[k];
    }
    if (n <= 0 || run_count <= 0) return;
    if (run_count > n) {
        db_log_add(f->db, "SYSTEM", "红外分组过多", "ignored",
                   cfg->layer, 0.0, 38.0, 0,
                   "layer infrared groups exceed candidate book count");
        return;
    }
    if (ir_width < total_width * 0.45) {
        db_log_add(f->db, "SYSTEM", "红外覆盖不足", "ignored",
                   cfg->layer, 0.0, 38.0, 0,
                   "layer infrared coverage too small; kept database position");
        return;
    }

    prefix[0] = 0.0;
    for (int i = 0; i < n; i++)
        prefix[i + 1] = prefix[i] + widths[i];
    for (int r = 0; r <= run_count; r++) {
        for (int j = 0; j <= n; j++) {
            dp[r][j] = INF;
            prev[r][j] = -1;
        }
    }
    dp[0][0] = 0.0;
    for (int r = 1; r <= run_count; r++) {
        for (int j = r; j <= n; j++) {
            for (int i = r - 1; i < j; i++) {
                double seg_w = prefix[j] - prefix[i];
                double run_w = range_width(runs[r - 1]);
                double run_mid = (runs[r - 1].start + runs[r - 1].end) * 0.5;
                double old_mid = (before_s[i] + before_e[j - 1]) * 0.5;
                double cost;
                if (dp[r - 1][i] >= INF * 0.5) continue;
                cost = fabs(seg_w - run_w) * 4.0 + fabs(old_mid - run_mid) * 0.25;
                if (seg_w > run_w + 2.4) cost += (seg_w - run_w) * 2.0;
                if (dp[r - 1][i] + cost < dp[r][j]) {
                    dp[r][j] = dp[r - 1][i] + cost;
                    prev[r][j] = i;
                }
            }
        }
    }
    if (prev[run_count][n] < 0) return;
    {
        int j = n;
        for (int r = run_count; r >= 1; r--) {
            int i = prev[r][j];
            assign_start[r - 1] = i;
            assign_end[r - 1] = j;
            j = i;
        }
    }

    for (int r = 0; r < run_count; r++) {
        int a = assign_start[r], z = assign_end[r];
        double seg_w = prefix[z] - prefix[a];
        double cursor = runs[r].start;
        if (runs[r].end >= 38.0 - 0.2 &&
            range_width(runs[r]) > seg_w + 0.5) {
            cursor = runs[r].end - seg_w;
        }
        if (cursor + seg_w > 38.0)
            cursor = 38.0 - seg_w;
        if (cursor < 0.0)
            cursor = 0.0;
        for (int k = a; k < z; k++) {
            int i = books[k];
            double ns = cursor, ne = ns + widths[k];
            cursor = ne;
            if (ne > 38.0) {
                ne = 38.0;
                ns = ne - widths[k];
                if (ns < 0.0) ns = 0.0;
            }
            if (fabs(ns - before_s[k]) > 0.5 ||
                fabs(ne - before_e[k]) > 0.5) {
                db_shelf_upsert(f->db, f->sc.book_epc[i], f->sc.book_title[i], "",
                                cfg->layer, ns, ne, f->sc.last_rssi[i], 1);
                db_log_add(f->db, f->sc.book_epc[i], f->sc.book_title[i],
                           "moved", cfg->layer, ns, ne, f->sc.last_rssi[i],
                           reason ? reason : "operation reflow");
            }
        }
    }
}

static int op_insert_returned_book(fsm_t *f, int idx, int target_layer,
                                   int have_ir, double ir_s, double ir_e,
                                   const char *reason,
                                   double *out_s, double *out_e)
{
    int books[SCANNER_MAX_BOOKS];
    double widths[SCANNER_MAX_BOOKS];
    double starts[SCANNER_MAX_BOOKS];
    double ends[SCANNER_MAX_BOOKS];
    int n = 0;
    int insert_pos = 0;
    int inserted = 0;
    double returned_w = 1.2;
    double target_s;
    double total_w = 0.0;
    double prefix_before = 0.0;
    double block_start;
    double cursor;

    if (!f || idx < 0 || idx >= f->sc.book_count || target_layer < 0)
        return 0;

    op_book_width(f, idx, &returned_w);
    if (have_ir && ir_e > ir_s + 0.05) {
        target_s = (ir_s + ir_e - returned_w) * 0.5;
    } else {
        target_s = f->op_before_start[idx];
        if (!(f->op_before_end[idx] > target_s + 0.05)) {
            double es = 0.0, ee = 0.0;
            book_expected_range(f, idx, &es, &ee);
            target_s = es;
        }
    }
    if (target_s < 0.0) target_s = 0.0;
    if (target_s + returned_w > SHELF_LEN_CM) target_s = SHELF_LEN_CM - returned_w;
    if (target_s < 0.0) target_s = 0.0;

    for (int i = 0; i < f->sc.book_count && i < SCANNER_MAX_BOOKS; i++) {
        shelf_item_t s;
        int present = f->sc.is_present[i];
        int layer = f->sc.actual_layer[i];
        double bs = f->op_before_start[i];
        double be = f->op_before_end[i];
        double w = 1.2;

        if (i == idx) continue;
        if (db_shelf_find(f->db, f->sc.book_epc[i], &s) == 1) {
            present = s.is_present;
            layer = s.layer;
            bs = s.start_cm;
            be = s.end_cm;
        }
        if (!present || layer != target_layer) continue;

        op_book_width(f, i, &w);
        if (be > bs + 0.05)
            w = be - bs;
        if (w < 0.6) w = 0.6;
        if (w > 8.0) w = 8.0;
        books[n] = i;
        widths[n] = w;
        starts[n] = bs;
        ends[n] = be;
        n++;
    }

    for (int a = 0; a < n; a++) {
        for (int b = a + 1; b < n; b++) {
            if (starts[b] < starts[a]) {
                int ti = books[a];
                double td;
                books[a] = books[b];
                books[b] = ti;
                td = widths[a]; widths[a] = widths[b]; widths[b] = td;
                td = starts[a]; starts[a] = starts[b]; starts[b] = td;
                td = ends[a]; ends[a] = ends[b]; ends[b] = td;
            }
        }
    }

    while (insert_pos < n) {
        double mid = (starts[insert_pos] + ends[insert_pos]) * 0.5;
        if (mid >= target_s + returned_w * 0.5) break;
        prefix_before += widths[insert_pos];
        insert_pos++;
    }

    total_w = returned_w;
    for (int i = 0; i < n; i++) total_w += widths[i];
    block_start = target_s - prefix_before;
    if (block_start + total_w > SHELF_LEN_CM) block_start = SHELF_LEN_CM - total_w;
    if (block_start < 0.0) block_start = 0.0;
    cursor = block_start;

    for (int pos = 0; pos <= n; pos++) {
        int bi;
        double w;
        double ns;
        double ne;
        const char *action;

        if (pos == insert_pos && !inserted) {
            bi = idx;
            w = returned_w;
            inserted = 1;
        } else {
            int src = pos;
            if (inserted) src = pos - 1;
            if (src < 0 || src >= n) continue;
            bi = books[src];
            w = widths[src];
        }

        ns = cursor;
        ne = ns + w;
        cursor = ne;
        if (ne > SHELF_LEN_CM) {
            ne = SHELF_LEN_CM;
            ns = ne - w;
            if (ns < 0.0) ns = 0.0;
        }

        action = (bi == idx) ? "returned" : "moved";
        db_shelf_upsert(f->db, f->sc.book_epc[bi], f->sc.book_title[bi], "",
                        target_layer, ns, ne, f->sc.last_rssi[bi], 1);
        if (bi == idx ||
            fabs(ns - f->op_before_start[bi]) > 0.35 ||
            fabs(ne - f->op_before_end[bi]) > 0.35) {
            db_log_add(f->db, f->sc.book_epc[bi], f->sc.book_title[bi],
                       action, target_layer, ns, ne, f->sc.last_rssi[bi],
                       reason ? reason : "returned book inserted by order");
        }
        f->sc.is_present[bi] = 1;
        f->sc.actual_layer[bi] = target_layer;
        if (bi == idx) {
            if (out_s) *out_s = ns;
            if (out_e) *out_e = ne;
        }
    }

    return inserted;
}

static void op_commit(fsm_t *f, uint32_t now)
{
    int removed[SCANNER_MAX_BOOKS], added[SCANNER_MAX_BOOKS], relayer[SCANNER_MAX_BOOKS];
    int rn = 0, an = 0, ln = 0;
    int board = op_changed_board(f);
    int layer = board >= 0 ? g_ir_boards[board].layer : -1;
    double removed_s = 0.0, removed_e = 0.0, added_s = 0.0, added_e = 0.0;
    uint16_t removed_bits = 0, added_bits = 0;

    for (int b = 0; b < FSM_IR_BOARD_MAX; b++)
        f->op_after_ir[b] = f->ir_bitmap[b];

    if (board >= 0) {
        removed_bits = f->op_before_ir[board] & (uint16_t)~f->op_after_ir[board];
        added_bits = f->op_after_ir[board] & (uint16_t)~f->op_before_ir[board];
        removed_bits |= f->op_seen_removed_ir[board];
        added_bits |= f->op_seen_added_ir[board];
        op_range_from_bits(&g_ir_boards[board], removed_bits, &removed_s, &removed_e);
        op_range_from_bits(&g_ir_boards[board], added_bits, &added_s, &added_e);
    }

    for (int i = 0; i < f->sc.book_count && i < SCANNER_MAX_BOOKS; i++) {
        int before_p = f->op_before_present[i];
        int after_p = f->sc.is_present[i];
        int after_l = f->sc.actual_layer[i];
        if (before_p && !after_p) removed[rn++] = i;
        else if (!before_p && after_p) added[an++] = i;
        else if (before_p && after_p && after_l != f->op_before_layer[i])
            relayer[ln++] = i;
    }

    printf("[FSM] 操作提交: board=%s removed=%d added=%d relayer=%d ir_removed=%.1f-%.1f ir_added=%.1f-%.1f\n",
           board >= 0 ? g_ir_boards[board].name : "none",
           rn, an, ln, removed_s, removed_e, added_s, added_e);

    if (rn == 0 && an == 0 && ln == 0) {
        if (board >= 0) {
            int changed_bits = __builtin_popcount((unsigned)(f->op_before_ir[board] ^ f->op_after_ir[board]));
            double move_s = removed_s < added_s ? removed_s : added_s;
            double move_e = removed_e > added_e ? removed_e : added_e;
            if (changed_bits < IR_ONLY_MOVE_MIN_BITS ||
                !(move_e > move_s + IR_REORDER_MIN_SPAN_CM)) {
                db_log_add(f->db, "SYSTEM", "红外轻微变化", "ignored",
                           layer, g_ir_boards[board].start_cm, g_ir_boards[board].end_cm,
                           0, "infrared-only jitter below movement threshold");
                return;
            }
            op_reflow_board_from_ir(f, board, "same-tag infrared movement");
            db_log_add(f->db, "SYSTEM", "红外整理", "moved",
                       layer, g_ir_boards[board].start_cm, g_ir_boards[board].end_cm,
                       0, "UHF unchanged; reflowed by infrared/history");
        }
        return;
    }

    if (rn == 1 && an == 0) {
        int i = removed[0];
        double rs = 0.0, re = 0.0;
        int ir_match = removed_ir_matches_book(f, i, removed_bits, removed_s, removed_e) ||
                       recent_removed_ir_matches_book(f, i, now, &rs, &re);
        if (!ir_match &&
            uhf_missing_is_recent(f, i, now)) {
            restore_transient_missing(f, i,
                                      "short UHF missing ignored; no current/recent infrared empty evidence");
            return;
        }
        confirm_book_removed(f, i, now,
                             ir_match ? "UHF missing confirmed by current/recent infrared empty gap" :
                                        "UHF missing aged enough; infrared evidence weak");
        if (board >= 0) op_reflow_board_from_ir(f, board, "book removed; neighbors reflowed");
        return;
    }

    if (rn == 0 && an == 1) {
        int i = added[0];
        double s = 0.0, e = 0.0;
        int target_layer = layer >= 0 ? layer : f->sc.actual_layer[i];
        int have_ir = added_bits && added_e > added_s + 0.05;
        if (!op_insert_returned_book(f, i, target_layer, have_ir, added_s, added_e,
                                     have_ir ? "returned; inserted at infrared position" :
                                               "returned; restored previous relative order",
                                     &s, &e)) {
            op_book_width(f, i, &e);
            s = f->op_before_start[i] >= 0.0 ? f->op_before_start[i] : 0.0;
            e = s + e;
            db_shelf_upsert(f->db, f->sc.book_epc[i], f->sc.book_title[i], "",
                            target_layer, s, e, f->sc.last_rssi[i], 1);
        }
        mqtt_publish_event(f, "returned", f->sc.book_epc[i], f->sc.book_title[i],
                           target_layer, s, f->sc.last_rssi[i]);
        return;
    }

    if (rn == 1 && an == 1) {
        int r = removed[0], a = added[0];
        double s = added_bits ? added_s : f->op_before_start[r];
        double e;
        int target_layer = layer >= 0 ? layer : f->op_before_layer[r];
        if (!removed_ir_matches_book(f, r, removed_bits, removed_s, removed_e) &&
            uhf_missing_is_recent(f, r, now)) {
            restore_transient_missing(f, r,
                                      "UHF missing ignored during return; no infrared empty at previous position");
            op_insert_returned_book(f, a,
                                    layer >= 0 ? layer : f->sc.actual_layer[a],
                                    added_bits && added_e > added_s + 0.05,
                                    added_s, added_e,
                                    "returned; ignored unrelated transient UHF missing",
                                    &s, &e);
            return;
        }
        db_shelf_upsert(f->db, f->sc.book_epc[r], f->sc.book_title[r], "",
                        f->op_before_layer[r], f->op_before_start[r],
                        f->op_before_end[r], 0, 0);
        db_log_add(f->db, f->sc.book_epc[r], f->sc.book_title[r],
                   "borrowed", f->op_before_layer[r], f->op_before_start[r],
                   f->op_before_end[r], 0, "replaced during same operation");
        op_insert_returned_book(f, a, target_layer,
                                added_bits && added_e > added_s + 0.05,
                                added_s, added_e,
                                "replacement/one-out-one-in inserted by order",
                                &s, &e);
        mqtt_publish_event(f, "returned", f->sc.book_epc[a], f->sc.book_title[a],
                           target_layer, s, f->sc.last_rssi[a]);
        return;
    }

    for (int k = 0; k < ln; k++) {
        int i = relayer[k];
        const char *action = f->sc.actual_layer[i] == f->sc.book_layer[i] ? "returned" : "misplaced";
        const ir_board_cfg_t *cfg = board >= 0 ? &g_ir_boards[board] : NULL;
        double width = 1.2;
        double s = added_bits ? added_s : f->op_before_start[i];
        double e;
        op_book_width(f, i, &width);
        e = s + width;
        op_clamp_fixed_range(cfg, width, &s, &e);
        if (!added_bits && action && strcmp(action, "misplaced") == 0) {
            fsm_set_issue(f, "检测到跨层错放, 位置证据不足");
        }
        db_shelf_upsert(f->db, f->sc.book_epc[i], f->sc.book_title[i], "",
                        f->sc.actual_layer[i], s, e, f->sc.last_rssi[i], 1);
        db_log_add(f->db, f->sc.book_epc[i], f->sc.book_title[i],
                   action, f->sc.actual_layer[i], s, e, f->sc.last_rssi[i],
                   added_bits ? "layer changed with infrared position" :
                                "layer changed; infrared position unavailable");
    }

    if (rn > 1 && !removed_bits) {
        for (int k = 0; k < rn; k++) {
            int i = removed[k];
            double rs = 0.0, re = 0.0;
            if (recent_removed_ir_matches_book(f, i, now, &rs, &re) &&
                now - f->missing_since[i] >= 8) {
                confirm_book_removed(f, i, now,
                                     "persistent multi UHF missing matched recent infrared empty gap");
            } else if (uhf_missing_is_recent(f, i, now)) {
                restore_transient_missing(f, i,
                                          "multi UHF missing treated as shielding; kept previous position");
            } else {
                confirm_book_removed(f, i, now,
                                     "persistent multi UHF missing confirmed without reordering");
            }
        }
        return;
    }

    if ((rn > 1 || an > 1) && board >= 0 && (removed_bits || added_bits)) {
        int handled = 0;

        if (rn > 1 && removed_bits) {
            for (int k = 0; k < rn; k++) {
                int i = removed[k];
                if (uhf_missing_is_recent(f, i, now)) {
                    restore_transient_missing(f, i,
                                              "multi UHF missing with infrared movement treated as shielding");
                } else {
                    confirm_book_removed(f, i, now,
                                         "persistent multi UHF missing confirmed as group removal");
                }
            }
            handled = 1;
        }

        if (an > 1 && added_bits) {
            for (int k = 0; k < an; k++) {
                int i = added[k];
                db_shelf_upsert(f->db, f->sc.book_epc[i], f->sc.book_title[i], "",
                                f->op_before_layer[i], f->op_before_start[i],
                                f->op_before_end[i], f->sc.last_rssi[i], 1);
                db_log_add(f->db, f->sc.book_epc[i], f->sc.book_title[i],
                           "returned", f->op_before_layer[i], f->op_before_start[i],
                           f->op_before_end[i], f->sc.last_rssi[i],
                           "multi returned; restored previous position/order");
                f->sc.is_present[i] = 1;
                f->sc.actual_layer[i] = f->op_before_layer[i];
                mqtt_publish_event(f, "returned", f->sc.book_epc[i], f->sc.book_title[i],
                                   f->op_before_layer[i],
                                   f->op_before_start[i], f->sc.last_rssi[i]);
            }
            handled = 1;
        }

        if (handled) {
            db_log_add(f->db, "SYSTEM", "复杂取放已处理", "resolved",
                       layer, removed_s, added_e, 0,
                       "handled multi RFID changes with infrared evidence");
            return;
        }
    }

    if (rn > 1 || an > 1) {
        char names[128];
        char issue[256];
        if (rn > 1 && an == 0 && !removed_bits) {
            fsm_join_book_titles(f, removed, rn, names, sizeof(names));
            snprintf(issue, sizeof(issue),
                     "UHF疑似漏读: %s；红外无空出，已保留状态", names);
            db_log_add(f->db, "SYSTEM", "UHF漏读", "ignored",
                       layer, removed_s, removed_e, 0, issue);
            return;
        } else if (an > 1 && rn == 0 && !added_bits) {
            fsm_join_book_titles(f, added, an, names, sizeof(names));
            snprintf(issue, sizeof(issue),
                     "UHF疑似外泄读到: %s；红外无新增，已保留状态", names);
            db_log_add(f->db, "SYSTEM", "UHF外泄", "ignored",
                       layer, added_s, added_e, 0, issue);
            return;
        } else if (rn > 1 && removed_bits) {
            fsm_join_book_titles(f, removed, rn, names, sizeof(names));
            snprintf(issue, sizeof(issue),
                     "多本疑似取走但红外无法匹配: %s；空出%.1f-%.1fcm",
                     names, removed_s, removed_e);
            db_log_add(f->db, "SYSTEM", "多本取走未匹配", "ignored",
                       layer, removed_s, removed_e, 0, issue);
            return;
        } else if (an > 1 && added_bits) {
            fsm_join_book_titles(f, added, an, names, sizeof(names));
            snprintf(issue, sizeof(issue),
                     "多本疑似放回但红外无法匹配: %s；新增%.1f-%.1fcm",
                     names, added_s, added_e);
            db_log_add(f->db, "SYSTEM", "多本放回未匹配", "ignored",
                       layer, added_s, added_e, 0, issue);
            return;
        } else {
            snprintf(issue, sizeof(issue),
                     "复杂取放: UHF少%d本多%d本，红外空出%.1f-%.1fcm新增%.1f-%.1fcm",
                     rn, an, removed_s, removed_e, added_s, added_e);
        }
        fsm_set_issue(f, issue);
        db_log_add(f->db, "SYSTEM", "复杂操作", "uncertain",
                   layer, removed_s, added_e, 0,
                   issue);
    }
}

static void ir_reconcile_with_shelf_state(fsm_t *f, uint32_t now)
{
    if (!f || f->ir_fd < 0 || f->op_active) return;
    if (now - f->t_last_ir_reconcile < 2) return;
    f->t_last_ir_reconcile = now;

    op_capture_before(f);
    for (int b = 0; b < FSM_IR_BOARD_MAX; b++)
        f->op_after_ir[b] = f->ir_bitmap[b];

    for (int layer = 0; layer < 2; layer++) {
        cm_range_t runs[32];
        int run_count = 0;
        int changed = 0;
        int present_count = 0;
        int trigger_board = -1;
        double book_min = 999.0;
        double book_max = -1.0;

        for (int b = 0; b < FSM_IR_BOARD_MAX; b++) {
            const ir_board_cfg_t *cfg = &g_ir_boards[b];
            cm_range_t bruns[16];
            uint16_t bitmap;
            int rn;
            if (!cfg->enabled || !f->ir_board_ok[b] || cfg->layer != layer)
                continue;
            if (trigger_board < 0) trigger_board = b;
            bitmap = ir_fill_single_gaps(f->ir_bitmap[b]);
            rn = ir_occupied_ranges_for_board(cfg, bitmap, bruns, 16);
            for (int r = 0; r < rn && run_count < 32; r++)
                runs[run_count++] = bruns[r];
        }
        if (trigger_board < 0 || run_count <= 0) continue;

        for (int i = 0; i < run_count; i++) {
            for (int j = i + 1; j < run_count; j++) {
                if (runs[j].start < runs[i].start) {
                    cm_range_t tmp = runs[i];
                    runs[i] = runs[j];
                    runs[j] = tmp;
                }
            }
        }
        {
            int out = 0;
            for (int i = 0; i < run_count; i++) {
                if (runs[i].start < 0.0) runs[i].start = 0.0;
                if (runs[i].end > 38.0) runs[i].end = 38.0;
                if (!(runs[i].end > runs[i].start)) continue;
                if (out == 0 || runs[i].start > runs[out - 1].end + 0.08) {
                    runs[out++] = runs[i];
                } else if (runs[i].end > runs[out - 1].end) {
                    runs[out - 1].end = runs[i].end;
                }
            }
            run_count = out;
        }
        if (run_count <= 0) continue;

        for (int i = 0; i < f->sc.book_count && i < SCANNER_MAX_BOOKS; i++) {
            shelf_item_t s;
            double center;
            double best_dist = 999.0;
            if (db_shelf_find(f->db, f->sc.book_epc[i], &s) != 1) continue;
            if (!s.is_present || s.layer != layer) continue;
            present_count++;
            if (s.start_cm < book_min) book_min = s.start_cm;
            if (s.end_cm > book_max) book_max = s.end_cm;
            if (s.start_cm < -0.1 || s.end_cm > 38.1) {
                changed = 1;
                break;
            }
            center = (s.start_cm + s.end_cm) * 0.5;
            for (int r = 0; r < run_count; r++) {
                double dist = 0.0;
                if (center < runs[r].start) dist = runs[r].start - center;
                else if (center > runs[r].end) dist = center - runs[r].end;
                if (dist < best_dist) best_dist = dist;
            }
            if (best_dist > 4.0) {
                changed = 1;
                break;
            }
        }
        if (!changed && present_count > 0) {
            if (fabs(book_min - runs[0].start) > 4.0 ||
                fabs(book_max - runs[run_count - 1].end) > 4.0) {
                changed = 1;
            }
        }
        if (changed) {
            op_reflow_board_from_ir(f, trigger_board, "periodic layer infrared correction");
            fsm_clear_issue(f);
        }
    }
}

static void op_window_update(fsm_t *f, uint32_t now)
{
    if (!f || f->ir_fd < 0) return;

    if (f->ir_changed) {
        if (!f->op_active) {
            op_capture_before(f);
            f->op_active = 1;
            f->op_started_at = now;
            printf("[FSM] 操作窗口开始: 等待红外/UHF稳定\n");
        }
        for (int b = 0; b < FSM_IR_BOARD_MAX; b++) {
            uint16_t before = f->ir_prev_bitmap[b];
            uint16_t after = f->ir_bitmap[b];
            uint16_t removed = before & (uint16_t)~after;
            f->op_seen_removed_ir[b] |= removed;
            f->op_seen_added_ir[b] |= after & (uint16_t)~before;
            if (removed) {
                op_range_from_bits(&g_ir_boards[b], removed,
                                   &f->recent_removed_start_cm[b],
                                   &f->recent_removed_end_cm[b]);
                f->recent_removed_ir[b] = removed;
                f->t_recent_removed_ir[b] = now;
            }
        }
        f->op_last_ir_change = now;
        f->ir_changed = 0;
    }

    if (f->op_active && now - f->op_last_ir_change >= FSM_OP_SETTLE_SEC) {
        op_commit(f, now);
        f->op_active = 0;
        mqtt_publish_status(f);
    }
}

static void schedule_layer_change(fsm_t *f, int idx, int layer, int rssi, uint32_t now)
{
    if (!f || idx < 0 || idx >= SCANNER_MAX_BOOKS) return;
    if (!f->layer_pending_valid[idx] || f->layer_pending_value[idx] != layer) {
        f->layer_pending_valid[idx] = 1;
        f->layer_pending_value[idx] = layer;
        f->t_layer_pending[idx] = now;
        printf("[FSM] UHF跨层候选: %s -> L%d, 等红外稳定%ds再提交\n",
               f->sc.book_title[idx], layer, FSM_LAYER_CHANGE_SETTLE_SEC);
    }
    f->layer_pending_rssi[idx] = rssi;
}

static void clear_layer_change(fsm_t *f, int idx)
{
    if (!f || idx < 0 || idx >= SCANNER_MAX_BOOKS) return;
    f->layer_pending_valid[idx] = 0;
    f->layer_pending_value[idx] = 0;
    f->layer_pending_rssi[idx] = 0;
    f->t_layer_pending[idx] = 0;
}

static void confirm_pending_layer_changes(fsm_t *f, uint32_t now)
{
    if (!f || f->op_active) return;
    if (f->op_last_ir_change &&
        now - f->op_last_ir_change < FSM_LAYER_CHANGE_SETTLE_SEC) {
        return;
    }

    for (int i = 0; i < f->sc.book_count && i < SCANNER_MAX_BOOKS; i++) {
        shelf_item_t s;
        int layer;
        const char *action;
        if (!f->layer_pending_valid[i]) continue;
        if (now - f->t_layer_pending[i] < FSM_LAYER_CHANGE_SETTLE_SEC) continue;
        if (!f->sc.is_present[i]) {
            clear_layer_change(f, i);
            continue;
        }

        layer = f->layer_pending_value[i];
        if (f->sc.actual_layer[i] != layer) {
            if (f->sc.actual_layer[i] == f->sc.book_layer[i]) {
                clear_layer_change(f, i);
            } else {
                schedule_layer_change(f, i, f->sc.actual_layer[i],
                                      f->sc.last_rssi[i], now);
            }
            continue;
        }

        if (db_shelf_find(f->db, f->sc.book_epc[i], &s) != 1) {
            double s0 = 0.0, e0 = 0.0;
            book_expected_range(f, i, &s0, &e0);
            if (!(e0 > s0)) {
                s0 = 0.0;
                e0 = 1.2;
            }
            s.layer = f->sc.book_layer[i];
            s.start_cm = s0;
            s.end_cm = e0;
        }
        if (s.layer == layer && s.is_present) {
            clear_layer_change(f, i);
            continue;
        }

        action = layer == f->sc.book_layer[i] ? "returned" : "misplaced";
        db_shelf_upsert(f->db, f->sc.book_epc[i], f->sc.book_title[i], "",
                        layer, s.start_cm, s.end_cm,
                        f->layer_pending_rssi[i], 1);
        db_log_add(f->db, f->sc.book_epc[i], f->sc.book_title[i],
                   action, layer, s.start_cm, s.end_cm,
                   f->layer_pending_rssi[i],
                   "UHF layer confirmed after infrared settle; preserved previous position");
        if (strcmp(action, "misplaced") == 0)
            fsm_set_issue(f, "UHF确认跨层错放, 红外已稳定");
        printf("[FSM] UHF跨层提交: %s L%d -> %s\n",
               f->sc.book_title[i], layer,
               strcmp(action, "misplaced") == 0 ? "错放" : "归位");
        clear_layer_change(f, i);
    }
}

/* ══════════════════════════════════════════════════════════════
 * tick
 * ══════════════════════════════════════════════════════════════ */

static void process_events(fsm_t *f, uint32_t now) {
    for (int e = 0; e < f->sc.event_count; e++) {
        scan_event_t *ev = &f->sc.events[e];
        switch (ev->type) {
        case SCAN_EV_MISSING:
            for (int i = 0; i < f->sc.book_count; i++) {
                if (strcmp(f->sc.book_epc[i], ev->epc) == 0) {
                    double rs = 0.0, re = 0.0;
                    if (!f->missing_since[i]) f->missing_since[i] = now;
                    if (recent_removed_ir_matches_book(f, i, now, &rs, &re)) {
                        confirm_book_removed(f, i, now,
                                             "UHF missing matched recent infrared empty gap");
                        printf("[FSM] UHF+红外确认取出: %s recent_ir=%.1f-%.1fcm\n",
                               ev->title, rs, re);
                    }
                    break;
                }
            }
            if (f->state == FSM_FIND && f->find_target_epc[0] &&
                strcmp(f->find_target_epc, ev->epc) == 0) {
                f->find_status = 2;
            }
            printf("[FSM] UHF疑似取出: %s, 等操作窗口提交\n", ev->title);
            break;
        case SCAN_EV_RETURNED:
        {
            for (int i = 0; i < f->sc.book_count; i++) {
                if (strcmp(f->sc.book_epc[i], ev->epc) == 0) {
                    shelf_item_t s;
                    f->missing_since[i] = 0;
                    if (db_shelf_find(f->db, ev->epc, &s) == 1) {
                        const char *action = ev->layer == ev->expected_layer ? "returned" : "misplaced";
                        int layer_changed = s.layer != ev->layer;
                        int write_layer = layer_changed ? s.layer : ev->layer;
                        db_shelf_upsert(f->db, ev->epc, ev->title, "",
                                        write_layer, s.start_cm, s.end_cm,
                                        ev->rssi, 1);
                        f->sc.is_present[i] = 1;
                        f->sc.actual_layer[i] = ev->layer;
                        if (layer_changed) {
                            schedule_layer_change(f, i, ev->layer, ev->rssi, now);
                        } else {
                            clear_layer_change(f, i);
                        }
                        if (!s.is_present && !layer_changed) {
                            db_log_add(f->db, ev->epc, ev->title,
                                       action, ev->layer, s.start_cm, s.end_cm, ev->rssi,
                                       "UHF confirmed presence/layer; preserved previous position");
                        }
                    } else {
                        double s0 = 0.0, e0 = 0.0;
                        int write_layer = ev->layer == ev->expected_layer ?
                                          ev->layer : ev->expected_layer;
                        book_expected_range(f, i, &s0, &e0);
                        if (!(e0 > s0)) {
                            s0 = 0.0;
                            e0 = 1.2;
                        }
                        db_shelf_upsert(f->db, ev->epc, ev->title, "",
                                        write_layer, s0, e0, ev->rssi, 1);
                        f->sc.is_present[i] = 1;
                        f->sc.actual_layer[i] = ev->layer;
                        if (write_layer != ev->layer) {
                            schedule_layer_change(f, i, ev->layer, ev->rssi, now);
                        } else {
                            clear_layer_change(f, i);
                            db_log_add(f->db, ev->epc, ev->title,
                                       "returned", ev->layer, s0, e0, ev->rssi,
                                       "UHF confirmed new presence; used expected position");
                        }
                    }
                    break;
                }
            }
            printf("[FSM] UHF疑似放回/换层: %s L%d, 等操作窗口提交\n",
                   ev->title, ev->layer);
            break;
        }
        case SCAN_EV_UNKNOWN:
            if (f->add_mode && !f->register_pending) {
                snprintf(f->pending_epc, sizeof(f->pending_epc), "%s", ev->epc);
                f->pending_layer = ev->layer;
                f->register_pending = 1;
                f->sc.detect_unknown = 0;
                printf("[FSM] 发现待登记 EPC: %s L%d\n", f->pending_epc, f->pending_layer);
            }
            break;
        default: break;
        }
    }
}

static void confirm_stale_uhf_missing(fsm_t *f, uint32_t now)
{
    if (!f || f->op_active) return;
    for (int i = 0; i < f->sc.book_count && i < SCANNER_MAX_BOOKS; i++) {
        shelf_item_t s;
        double rs = 0.0, re = 0.0;
        uint32_t age;
        if (db_shelf_find(f->db, f->sc.book_epc[i], &s) != 1) continue;
        if (!s.is_present) continue;
        if (f->sc.is_present[i]) continue;
        if (!f->missing_since[i]) {
            f->missing_since[i] = now;
            continue;
        }
        age = now - f->missing_since[i];
        if (recent_removed_ir_matches_book(f, i, now, &rs, &re)) {
            confirm_book_removed(f, i, now,
                                 "stale UHF missing matched recent infrared empty gap");
            printf("[FSM] 延迟确认取出: %s UHF缺失%us + 红外%.1f-%.1fcm\n",
                   f->sc.book_title[i], (unsigned)age, rs, re);
        } else if (age >= 12) {
            confirm_book_removed(f, i, now,
                                 "long UHF missing confirmed without fresh infrared evidence");
            printf("[FSM] UHF长时间缺失确认取出: %s 缺失%us\n",
                   f->sc.book_title[i], (unsigned)age);
        }
    }
}

int fsm_tick(fsm_t *f) {
    if (!f) return -1;
    uint32_t now = (uint32_t)time(NULL);

    fsm_poll_local_cmd(f);
    fsm_poll_led_brightness_file(f, now);

    /* ── 延迟对时 (WiFi 就绪后, 单次后台, 不阻塞 tick) ── */
    static uint32_t last_time_sync_try = 0;
    if (f->mod_wifi.ok && time(NULL) < 1704067200 && now - last_time_sync_try >= 60) {
        last_time_sync_try = now;  /* 每60秒重试一次，直到时间有效 */
        const char *servers[] = {"time.nist.gov", "time.cloudflare.com", NULL};
        for (const char **ts = servers; *ts; ts++) {
            char cmd[160];
            snprintf(cmd, sizeof(cmd), "timeout 3 rdate -s %s 2>/dev/null &", *ts);
            system(cmd);
        }
    }

    /* ── MQTT 轮询 (非阻塞) + 重连 ── */
    if (f->mod_mqtt.enabled && f->mod_wifi.ok) {
        if (!f->mqtt.connected) {
            /* 仅在 WiFi 已连热点时重试 (缓存1秒, 避免每tick读文件) */
            static uint32_t last_wifi_check = 0;
            static int cached_wifi_up = 0;
            if (now - last_wifi_check >= 1) {
                last_wifi_check = now;
                cached_wifi_up = 0;
                int wfd = open("/sys/class/net/wlan0/operstate", O_RDONLY);
                if (wfd >= 0) {
                    char ws[16] = {0};
                    read(wfd, ws, sizeof(ws)-1);
                    close(wfd);
                    cached_wifi_up = (strncmp(ws, "up", 2) == 0);
                }
            }
            if (cached_wifi_up) {
                static uint32_t last_reconnect = 0;
                if (now - last_reconnect >= 60) {   /* 每60秒重试一次, 减少阻塞 */
                    last_reconnect = now;
                    if (mqtt_connect(&f->mqtt) == 0) {
                        f->mod_mqtt.ok = 1;
                        mqtt_subscribe(&f->mqtt, "bookshelf/cmd/find", 1);
                        mqtt_subscribe(&f->mqtt, "bookshelf/cmd/refresh", 0);
                        printf("[MQTT] 重连成功\n");
                        mqtt_publish_status(f);
                    }
                }
            }
        }
        if (f->mqtt.connected)
            mqtt_poll(&f->mqtt, FSM_MQTT_POLL_MS);
    }

    /* ── 雷达轮询 (非阻塞) ── */
    int radar_has_person = 0;
    if (f->mod_radar.enabled && f->mod_radar.ok) {
        ld2410c_poll(&f->radar, 1);
        const ld2410c_data_t *rd = ld2410c_get_data(&f->radar);
        /* 仅运动/静止/兼有为有效人体 (排除底噪 4/5/6) */
        if (rd && rd->target_state >= 1 && rd->target_state <= 3)
            radar_has_person = 1;
    }
    fsm_update_radar_runtime(f, radar_has_person, now);

    /* ── 硬件检查 ── */
    if (f->mod_uhf.enabled) {
        for (int d = 0; d < 2; d++)
            if (f->sc.dev[d].fd < 0) {
                enter_fault_reason(f, d,
                                   d == 0 ? "上层 UHF 运行中断开" : "下层 UHF 运行中断开",
                                   "检查对应 UHF USB 连接，设备恢复后点击重新检测");
            }
    }

    switch (f->state) {

    /* ── BOOT ──────────────────────────────── */
    case FSM_BOOT: {
        scanner_poll(&f->sc);
        if (f->ir_fd >= 0) ir_scan(f);
        int r = baseline(f);
        if (r == 1) {
            if (f->ir_fd >= 0) {
                ir_scan(f);
                f->ir_changed = 0;
            }
            enter_active(f);
        }
        else if (r == -1) enter_fault(f, -1);
        break;
    }

    /* ── ACTIVE ────────────────────────────── */
    case FSM_ACTIVE: {
        scanner_poll(&f->sc);
        if (f->ir_fd >= 0) ir_scan(f);

        if (f->mod_radar.ok) {
            if (radar_has_person) {
                f->t_last_present = now;
            } else if (f->radar_sleep_reliable &&
                       now - f->t_last_present >= FSM_SLEEP_TIMEOUT) {
                enter_sleep(f);
                break;
            } else if (!f->radar_sleep_reliable) {
                f->t_last_present = now;
            }
        }

        process_events(f, now);
        op_window_update(f, now);
        confirm_pending_layer_changes(f, now);
        confirm_stale_uhf_missing(f, now);
        ir_reconcile_with_shelf_state(f, now);

        /* MQTT 周期推送: ACTIVE 每30s全量, 每60s传感器 */
        if (now - f->t_last_mqtt_status >= FSM_MQTT_STATUS_ACTIVE_SEC)
            mqtt_publish_status(f);
        if (now - f->t_last_mqtt_sensor >= FSM_MQTT_SENSOR_SEC)
            mqtt_publish_sensors(f);

        if (f->led_ok) {
            shelf_led_clear();
            if (f->shelf_issue) {
                shelf_led_status(0, 255, 0, 180);   /* 粉: 需要人工整理 */
                shelf_led_status(1, 255, 0, 180);
            } else {
                shelf_led_status(0, 0, 255, 0);     /* 绿 */
                shelf_led_status(1, 0, 255, 0);
            }
            led_render(f);
        }
        break;
    }

    /* ── SLEEP ─────────────────────────────── */
    case FSM_SLEEP: {
        if (f->ir_fd >= 0 &&
            now - f->t_last_ir_scan >= FSM_IR_SLEEP_SCAN_SEC) {
            if (ir_scan(f) == 0 && f->ir_changed) {
                printf("[FSM] 红外变化唤醒\n");
                op_window_update(f, now);
                enter_active(f);
                break;
            }
        }

        /* MQTT 周期推送: SLEEP 每5min全量+传感器 */
        if (now - f->t_last_mqtt_status >= FSM_MQTT_STATUS_SLEEP_SEC)
            mqtt_publish_status(f);
        if (now - f->t_last_mqtt_sensor >= FSM_MQTT_STATUS_SLEEP_SEC)
            mqtt_publish_sensors(f);

        if (f->mod_radar.ok && (radar_has_person || !f->radar_sleep_reliable))
            enter_active(f);
        break;
    }

    /* ── FIND ──────────────────────────────── */
    case FSM_FIND: {
        scanner_poll(&f->sc);
        if (f->ir_fd >= 0) ir_scan(f);
        process_events(f, now);
        op_window_update(f, now);

        /* 检查是否全部取走 → 提前退出 */
        int all_gone = (f->find_count > 0);
        for (int m = 0; m < f->find_count; m++) {
            int b = f->find_matches[m];
            if (b >= 0 && b < f->sc.book_count && f->sc.is_present[b])
                { all_gone = 0; break; }
        }

        if (all_gone) {
            printf("[FSM] 🔍 全部取走, 结束 FIND\n");
            f->find_count = 0;
            f->find_query[0] = '\0';
            enter_active(f);
            break;
        }

        /* 超时退出 */
        if (now - f->t_find_start >= FSM_FIND_TIMEOUT) {
            printf("[FSM] 🔍 FIND 超时 (%ds), 返回 %s\n",
                   FSM_FIND_TIMEOUT,
                   f->prev_state == FSM_SLEEP ? "SLEEP" : "ACTIVE");
            f->find_status = 3;
            f->find_count = 0;
            f->find_query[0] = '\0';
            if (f->prev_state == FSM_SLEEP)
                enter_sleep(f);
            else
                enter_active(f);
            break;
        }

        if (f->led_ok) {
            shelf_led_clear();
            shelf_led_status(0, 255, 255, 0);  /* 黄 */
            shelf_led_status(1, 255, 255, 0);
            led_render(f);
        }
        break;
    }

    /* ── FAULT ─────────────────────────────── */
    case FSM_FAULT:
        if (f->led_ok) led_status(f, 255, 0, 0);
        return 0;
    }

    /* ── 周期打印 ── */
    if (f->verbose >= 2 && now - f->t_last_dump >= FSM_DUMP_IVAL) {
        f->t_last_dump = now;
        int present = 0;
        for (int i = 0; i < f->sc.book_count; i++)
            if (f->sc.is_present[i]) present++;

        const char *sname[] = {"BOOT","ACTIVE","SLEEP","FIND","FAULT"};
        printf("[FSM] %-6s 书:%d 在架:%d/%d 雷达:%s\n",
               sname[f->state], f->sc.book_count, present, f->sc.book_count,
               f->mod_radar.ok ? (radar_has_person ? "有人" : "无人") : "-");
    }

    /* ── 定期归档清理 (每10分钟) ── */
    if (now - f->t_last_archive >= FSM_ARCHIVE_INTERVAL_SEC) {
        f->t_last_archive = now;
        db_cleanup(f->db, f->archive_ok ? &f->db_archive : NULL);
    }

    if (now - f->t_last_state_save >= 5) {
        f->t_last_state_save = now;
        fsm_save_runtime_snapshot(f, 0);
    }

    fsm_write_state_file(f);

    return 0;
}

/* ══════════════════════════════════════════════════════════════
 * stop
 * ══════════════════════════════════════════════════════════════ */

void fsm_stop(fsm_t *f) {
    if (!f) return;
    printf("[FSM] stopping...\n");

    /* 推送离线 + 断开 MQTT */
    if (f->mod_mqtt.ok && f->mqtt.connected) {
        mqtt_publish(&f->mqtt,
            mqtt_can_publish_shelf_status(f) ? "bookshelf/status" : "bookshelf/device/status",
            "{\"status\":\"offline\"}", 21, 1, 1);
        usleep(100000);
        mqtt_disconnect(&f->mqtt);
    }

    fsm_save_runtime_snapshot(f, 1);

    /* 最后一次归档 */
    if (f->archive_ok)
        db_cleanup(f->db, &f->db_archive);
    else
        db_cleanup(f->db, NULL);

    if (f->mod_uhf.ok) scanner_stop(&f->sc);
    if (f->mod_radar.ok) ld2410c_close(&f->radar);
    sensor_close(f);
    if (f->ir_fd >= 0) {
        ir_shift_byte(0x00);
        close(f->ir_fd);
        f->ir_fd = -1;
    }
    if (f->led_ok) { shelf_led_all_off(); shelf_led_show(); shelf_led_close(); }
    if (f->archive_ok) db_close(&f->db_archive);
    unlink(SB_FSM_STATE_FILE);
    printf("[FSM] done.\n");
}

int fsm_request_find(fsm_t *f, const char *query) {
    if (!f || !query || !query[0]) return -1;
    if (f->state != FSM_ACTIVE && f->state != FSM_SLEEP && f->state != FSM_FIND)
        return -1;

    snprintf(f->find_query, sizeof(f->find_query), "%s", query);
    f->find_count = 0;
    enter_find(f);
    return f->find_count;
}

void fsm_finish_find(fsm_t *f, int cancelled) {
    if (!f) return;
    leave_find(f, cancelled);
}

int fsm_force_state(fsm_t *f, fsm_state_t state) {
    if (!f) return -1;

    switch (state) {
    case FSM_BOOT:
        f->prev_state = f->state;
        f->state = FSM_BOOT;
        return 0;
    case FSM_ACTIVE:
        enter_active(f);
        return 0;
    case FSM_SLEEP:
        enter_sleep(f);
        return 0;
    case FSM_FIND:
        if (f->state != FSM_ACTIVE && f->state != FSM_SLEEP)
            f->prev_state = FSM_ACTIVE;
        enter_find(f);
        return 0;
    case FSM_FAULT:
        enter_fault(f, -1);
        return 0;
    default:
        return -1;
    }
}

int fsm_register_book(fsm_t *f, const char *epc, const char *title,
                      const char *author, int layer,
                      double start_cm, double end_cm) {
    return fsm_register_book_pages(f, epc, title, author, layer, start_cm, end_cm, 0);
}

int fsm_register_book_pages(fsm_t *f, const char *epc, const char *title,
                            const char *author, int layer,
                            double start_cm, double end_cm, int pages) {
    if (!f || !f->db || !epc || !epc[0] || !title || !title[0]) return -1;

    if (pages < 0) pages = 0;
    if (db_book_upsert_pages(f->db, epc, title, author ? author : "",
                             layer, start_cm, end_cm, pages) < 0)
        return -1;

    db_shelf_upsert(f->db, epc, title, author ? author : "",
                    layer, start_cm, end_cm, 0, 1);
    db_log_add(f->db, epc, title, "registered",
               layer, start_cm, end_cm, 0, "saved from screen UI");
    return 0;
}
