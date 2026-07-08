#include "ui_model.h"
#include "../common/sb_ipc.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void copy_text(char *dst, size_t dst_sz, const char *src)
{
    if (!dst || dst_sz == 0) return;
    snprintf(dst, dst_sz, "%s", src ? src : "");
}

static int find_index(ui_model_t *m, const char *epc)
{
    if (!m || !epc) return -1;
    for (int i = 0; i < m->count; ++i) {
        if (strcmp(m->books[i].epc, epc) == 0) return i;
    }
    return -1;
}

static double valid_start(const ui_book_t *b)
{
    if (b->end_cm > b->start_cm + 0.5) return b->start_cm;
    if (b->expected_end > b->expected_start + 0.5) return b->expected_start;
    return 0.0;
}

static int compare_books(const void *a, const void *b)
{
    const ui_book_t *ba = (const ui_book_t *)a;
    const ui_book_t *bb = (const ui_book_t *)b;
    int la = ba->layer >= 0 ? ba->layer : ba->expected_layer;
    int lb = bb->layer >= 0 ? bb->layer : bb->expected_layer;
    if (la != lb) return la - lb;
    double da = valid_start(ba);
    double db = valid_start(bb);
    if (da < db) return -1;
    if (da > db) return 1;
    return strcmp(ba->title, bb->title);
}

int ui_model_load(db_ctx_t *db, ui_model_t *model)
{
    book_info_t catalog[UI_MAX_BOOKS];
    shelf_item_t shelf[UI_MAX_BOOKS];
    int catalog_n;
    int shelf_n;

    if (!db || !model) return -1;
    memset(model, 0, sizeof(*model));
    model->loaded_at = time(NULL);

    catalog_n = db_book_list(db, catalog, UI_MAX_BOOKS);
    if (catalog_n < 0) catalog_n = 0;
    for (int i = 0; i < catalog_n && model->count < UI_MAX_BOOKS; ++i) {
        ui_book_t *b = &model->books[model->count++];
        copy_text(b->epc, sizeof(b->epc), catalog[i].epc);
        copy_text(b->title, sizeof(b->title), catalog[i].title[0] ? catalog[i].title : catalog[i].epc);
        b->expected_layer = catalog[i].expected_layer;
        b->expected_start = catalog[i].expected_start;
        b->expected_end = catalog[i].expected_end;
        b->layer = catalog[i].expected_layer;
        b->start_cm = catalog[i].expected_start;
        b->end_cm = catalog[i].expected_end;
        b->rssi = 0;
        b->is_present = 0;
        b->known = 1;
    }

    shelf_n = db_shelf_list_all(db, shelf, UI_MAX_BOOKS);
    if (shelf_n < 0) shelf_n = 0;
    for (int i = 0; i < shelf_n; ++i) {
        int idx = find_index(model, shelf[i].epc);
        ui_book_t *b;
        if (idx < 0) {
            continue;
        } else {
            b = &model->books[idx];
            if (!b->title[0] && shelf[i].title[0]) copy_text(b->title, sizeof(b->title), shelf[i].title);
        }
        b->layer = shelf[i].layer;
        if (shelf[i].end_cm > shelf[i].start_cm + 0.5) {
            b->start_cm = shelf[i].start_cm;
            b->end_cm = shelf[i].end_cm;
        }
        b->rssi = shelf[i].rssi;
        b->is_present = shelf[i].is_present;
        copy_text(b->last_seen, sizeof(b->last_seen), shelf[i].last_seen);
    }

    for (int i = 0; i < model->count; ++i) {
        ui_book_t *b = &model->books[i];
        b->misplaced = b->known && b->is_present && b->expected_layer >= 0 && b->layer != b->expected_layer;
        if (b->is_present) model->present_count++;
        else model->missing_count++;
        if (b->misplaced) model->misplaced_count++;
        if (!b->known) model->unknown_count++;
    }

    sensor_log_t sensors[1];
    if (db_sensor_recent(db, 1, sensors, 1) > 0) {
        model->sensor = sensors[0];
        model->has_sensor = 1;
    }

    qsort(model->books, (size_t)model->count, sizeof(model->books[0]), compare_books);
    return model->count;
}

const ui_book_t *ui_model_find_epc(const ui_model_t *model, const char *epc)
{
    if (!model || !epc) return NULL;
    for (int i = 0; i < model->count; ++i) {
        if (strcmp(model->books[i].epc, epc) == 0) return &model->books[i];
    }
    return NULL;
}

static void lower_ascii(char *dst, size_t dst_sz, const char *src)
{
    size_t j = 0;
    if (!dst || dst_sz == 0) return;
    for (size_t i = 0; src && src[i] && j + 1 < dst_sz; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (c < 128) dst[j++] = (char)tolower(c);
    }
    dst[j] = '\0';
}

static int text_has(const char *text, const char *needle)
{
    return text && needle && needle[0] && strstr(text, needle) != NULL;
}

static int query_hits(const char *q, const char *abbr, const char *pinyin)
{
    return text_has(abbr, q) || text_has(pinyin, q) ||
           text_has(q, abbr) || text_has(q, pinyin);
}

static int pinyin_match_title(const char *title, const char *q)
{
    struct py_item {
        const char *key;
        const char *abbr;
        const char *pinyin;
    };
    static const struct py_item items[] = {
        {"通信", "tx", "tongxin"},
        {"通信原理", "txyl", "tongxinyuanli"},
        {"马原", "my", "mayuan"},
        {"毛概", "mg", "maogai"},
        {"线代", "xd", "xiandai"},
        {"大物", "dw", "dawu"},
        {"复变", "fb", "fubian"},
        {"数电", "sd", "shudian"},
        {"模电", "md", "modian"},
        {"概统", "gt", "gaitong"},
        {"电磁", "dc", "dianci"},
        {"电磁场", "dcc", "diancichang"},
        {"信息", "xx", "xinxi"},
        {"信息论", "xxl", "xinxilun"},
        {"数理", "sl", "shuli"},
        {"数理方程", "slfc", "shulifangcheng"},
        {"标日", "br", "biaori"},
        {"标日上", "brs", "biaorishang"},
        {"标日下", "brx", "biaorixia"},
        {"fsf", "fsf", "fsf"},
        {"dsp", "dsp", "dsp"},
    };

    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
        if (strstr(title, items[i].key) && query_hits(q, items[i].abbr, items[i].pinyin)) {
            return 1;
        }
    }
    return 0;
}

int ui_book_match(const ui_book_t *book, const char *query)
{
    char q[96];
    char title[160];
    char epc[96];
    if (!book || !query || !query[0]) return 1;
    lower_ascii(q, sizeof(q), query);
    if (!q[0]) return strstr(book->title, query) != NULL;
    lower_ascii(title, sizeof(title), book->title);
    lower_ascii(epc, sizeof(epc), book->epc);
    if (strstr(title, q) || strstr(epc, q)) return 1;

    return pinyin_match_title(book->title, q);
}

const char *ui_layer_name(int layer)
{
    return layer == 0 ? "上层" : layer == 1 ? "下层" : "未分层";
}

const char *ui_rssi_text(int rssi)
{
    if (rssi >= 180) return "很强";
    if (rssi >= 140) return "强";
    if (rssi >= 100) return "中等";
    if (rssi >= 60) return "弱";
    return "很弱";
}

int ui_fsm_read(ui_fsm_snapshot_t *out)
{
    FILE *fp;
    char line[128];
    struct stat st;
    static fsm_state_t last_state = FSM_BOOT;

    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->state = last_state;
    out->fault_dev = -1;
    out->pending_layer = -1;

    if (stat(SB_FSM_STATE_FILE, &st) == 0) {
        out->exists = 1;
        out->mtime = st.st_mtime;
    }

    fp = fopen(SB_FSM_STATE_FILE, "r");
    if (!fp) return -1;

    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "state=", 6) == 0) {
            if (strstr(line, "ACTIVE")) out->state = FSM_ACTIVE;
            else if (strstr(line, "SLEEP")) out->state = FSM_SLEEP;
            else if (strstr(line, "FIND")) out->state = FSM_FIND;
            else if (strstr(line, "FAULT")) out->state = FSM_FAULT;
            else out->state = FSM_BOOT;
        } else if (strncmp(line, "fault_dev=", 10) == 0) {
            out->fault_dev = atoi(line + 10);
        } else if (strncmp(line, "find_count=", 11) == 0) {
            out->find_count = atoi(line + 11);
        } else if (strncmp(line, "find_status=", 12) == 0) {
            out->find_status = atoi(line + 12);
        } else if (strncmp(line, "find_target_epc=", 16) == 0) {
            snprintf(out->find_target_epc, sizeof(out->find_target_epc), "%s", line + 16);
            out->find_target_epc[strcspn(out->find_target_epc, "\r\n")] = '\0';
        } else if (strncmp(line, "find_target_title=", 18) == 0) {
            snprintf(out->find_target_title, sizeof(out->find_target_title), "%s", line + 18);
            out->find_target_title[strcspn(out->find_target_title, "\r\n")] = '\0';
        } else if (strncmp(line, "add_mode=", 9) == 0) {
            out->add_mode = atoi(line + 9);
        } else if (strncmp(line, "register_pending=", 17) == 0) {
            out->register_pending = atoi(line + 17);
        } else if (strncmp(line, "pending_epc=", 12) == 0) {
            snprintf(out->pending_epc, sizeof(out->pending_epc), "%s", line + 12);
            out->pending_epc[strcspn(out->pending_epc, "\r\n")] = '\0';
        } else if (strncmp(line, "pending_layer=", 14) == 0) {
            out->pending_layer = atoi(line + 14);
        } else if (strncmp(line, "shelf_issue=", 12) == 0) {
            out->shelf_issue = atoi(line + 12);
        } else if (strncmp(line, "shelf_issue_text=", 17) == 0) {
            snprintf(out->shelf_issue_text, sizeof(out->shelf_issue_text), "%s", line + 17);
            out->shelf_issue_text[strcspn(out->shelf_issue_text, "\r\n")] = '\0';
        } else if (strncmp(line, "wifi_hw=", 8) == 0) {
            out->wifi_hw = atoi(line + 8);
        } else if (strncmp(line, "wifi_connected=", 15) == 0) {
            out->wifi_connected = atoi(line + 15);
        } else if (strncmp(line, "wifi_ssid=", 10) == 0) {
            snprintf(out->wifi_ssid, sizeof(out->wifi_ssid), "%s", line + 10);
            out->wifi_ssid[strcspn(out->wifi_ssid, "\r\n")] = '\0';
        } else if (strncmp(line, "wifi_ip=", 8) == 0) {
            snprintf(out->wifi_ip, sizeof(out->wifi_ip), "%s", line + 8);
            out->wifi_ip[strcspn(out->wifi_ip, "\r\n")] = '\0';
        } else if (strncmp(line, "wifi_signal=", 12) == 0) {
            out->wifi_signal = atoi(line + 12);
        } else if (strncmp(line, "wifi_error=", 11) == 0) {
            snprintf(out->wifi_error, sizeof(out->wifi_error), "%s", line + 11);
            out->wifi_error[strcspn(out->wifi_error, "\r\n")] = '\0';
        } else if (strncmp(line, "mod_led_ok=", 11) == 0) {
            out->mod_led_ok = atoi(line + 11);
        } else if (strncmp(line, "mod_radar_ok=", 13) == 0) {
            out->mod_radar_ok = atoi(line + 13);
        } else if (strncmp(line, "mod_uhf_ok=", 11) == 0) {
            out->mod_uhf_ok = atoi(line + 11);
        } else if (strncmp(line, "mod_wifi_ok=", 12) == 0) {
            out->mod_wifi_ok = atoi(line + 12);
        } else if (strncmp(line, "mod_sensor_ok=", 14) == 0) {
            out->mod_sensor_ok = atoi(line + 14);
        } else if (strncmp(line, "mod_mqtt_ok=", 12) == 0) {
            out->mod_mqtt_ok = atoi(line + 12);
        } else if (strncmp(line, "mod_screen_ok=", 14) == 0) {
            out->mod_screen_ok = atoi(line + 14);
        } else if (strncmp(line, "bh1750_ok=", 10) == 0) {
            out->bh1750_ok = atoi(line + 10);
        } else if (strncmp(line, "sht30_ok=", 9) == 0) {
            out->sht30_ok = atoi(line + 9);
        } else if (strncmp(line, "mqtt_connected=", 15) == 0) {
            out->mqtt_connected = atoi(line + 15);
        } else if (strncmp(line, "ir0_ok=", 7) == 0) {
            out->ir_ok[0] = atoi(line + 7);
        } else if (strncmp(line, "ir0_bitmap=", 11) == 0) {
            out->ir_bitmap[0] = (uint16_t)strtoul(line + 11, NULL, 16);
        } else if (strncmp(line, "ir1_ok=", 7) == 0) {
            out->ir_ok[1] = atoi(line + 7);
        } else if (strncmp(line, "ir1_bitmap=", 11) == 0) {
            out->ir_bitmap[1] = (uint16_t)strtoul(line + 11, NULL, 16);
        } else if (strncmp(line, "ir2_ok=", 7) == 0) {
            out->ir_ok[2] = atoi(line + 7);
        } else if (strncmp(line, "ir2_bitmap=", 11) == 0) {
            out->ir_bitmap[2] = (uint16_t)strtoul(line + 11, NULL, 16);
        } else if (strncmp(line, "ir3_ok=", 7) == 0) {
            out->ir_ok[3] = atoi(line + 7);
        } else if (strncmp(line, "ir3_bitmap=", 11) == 0) {
            out->ir_bitmap[3] = (uint16_t)strtoul(line + 11, NULL, 16);
        }
    }
    fclose(fp);
    last_state = out->state;
    return 0;
}

static int write_cmd(const char *cmd)
{
    char line[320];
    int len;
    if (!cmd || !cmd[0]) return -1;
    if (strchr(cmd, '\n') || strchr(cmd, '\r')) return -1;
    len = snprintf(line, sizeof(line), "%s\n", cmd);
    if (len < 0 || len >= (int)sizeof(line)) return -1;
    return sb_write_text_atomic(SB_CMD_FILE, line);
}

int ui_cmd_find(const char *epc)
{
    char cmd[160];
    if (!epc || !epc[0]) return -1;
    snprintf(cmd, sizeof(cmd), "FIND %s", epc);
    return write_cmd(cmd);
}

int ui_cmd_cancel_find(void)
{
    return write_cmd("FIND_CANCEL");
}

int ui_cmd_clear_issue(void)
{
    return write_cmd("CLEAR_ISSUE");
}

int ui_cmd_add_start(void)
{
    return write_cmd("ADD_START");
}

int ui_cmd_add_cancel(void)
{
    return write_cmd("ADD_CANCEL");
}

int ui_cmd_register_book(int layer, int pages, const char *title)
{
    char cmd[256];
    if (!title || !title[0]) return -1;
    if (pages < 1) pages = 200;
    snprintf(cmd, sizeof(cmd), "REGISTER_PAGES %d %d %s", layer, pages, title);
    return write_cmd(cmd);
}

int ui_cmd_delete_book(const char *epc)
{
    char cmd[160];
    if (!epc || !epc[0]) return -1;
    snprintf(cmd, sizeof(cmd), "DELETE_BOOK %s", epc);
    return write_cmd(cmd);
}

int ui_cmd_net_scan(void)
{
    return write_cmd("NET_SCAN");
}

int ui_cmd_net_connect(const char *ssid, const char *password)
{
    char cmd[256];
    if (!ssid || !ssid[0]) return -1;
    snprintf(cmd, sizeof(cmd), "NET_CONNECT %s|%s", ssid, password ? password : "");
    return write_cmd(cmd);
}

int ui_cmd_net_disconnect(void)
{
    return write_cmd("NET_DISCONNECT");
}

int ui_cmd_fault_retry(void)
{
    return write_cmd("FAULT_RETRY");
}

int ui_cmd_system_reboot(void)
{
    return write_cmd("SYSTEM_REBOOT");
}

int ui_cmd_system_poweroff(void)
{
    return write_cmd("SYSTEM_POWEROFF");
}
