#include "archive/window.h"
#include "ui_model.h"
#include "../common/sb_ipc.h"

#include <lvgl/lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DB_PATH "/root/bookshelf.db"
#define SHELF_LEN_CM 38.0

typedef enum {
    PAGE_SHELF = 0,
    PAGE_SEARCH = 1,
    PAGE_ADD = 2,
    PAGE_TREND = 3,
    PAGE_NETWORK = 4
} page_t;

static db_ctx_t g_db;
static int g_db_ready;
static ui_model_t g_model;
static ui_fsm_snapshot_t g_fsm;
static lv_obj_t *ir_dot[2][32];
static page_t g_page = PAGE_SHELF;
static page_t g_find_return_page = PAGE_SHELF;
static int g_shelf_edit_mode;
static int g_ack_misplaced_count;
static char g_find_epc[64];
static char g_search_query[128];
static time_t g_find_started_at;
static time_t g_find_completed_at;
static int g_find_active;

static lv_font_t *font_main;
static lv_obj_t *screen;
static lv_obj_t *topbar;
static lv_obj_t *title_label;
static lv_obj_t *state_pill;
static lv_obj_t *summary_row;
static lv_obj_t *content;
static lv_obj_t *footer;
static lv_obj_t *sensor_label;
static lv_obj_t *time_label;
static lv_obj_t *sleep_overlay;
static lv_obj_t *nav_btn[5];
static lv_obj_t *search_box;
static lv_obj_t *search_results;
static lv_obj_t *keyboard;
static int g_search_results_dirty = 1;
static int g_screen_sleeping;
static int g_backlight_restore = -1;
static lv_obj_t *add_title_box;
static lv_obj_t *add_pages_box;
static lv_obj_t *add_form_box;
static lv_obj_t *wifi_ssid_box;
static lv_obj_t *wifi_pass_box;
static char g_add_title_draft[128];
static char g_add_pages_draft[32] = "200";
static char g_delete_epc[64];
static char g_delete_title[128];
static char g_wifi_ssid_draft[64];
static char g_wifi_pass_draft[96];
static int g_wifi_draft_ready;
static lv_obj_t *trend_scroll_box;
static lv_obj_t *wifi_ap_list;
static lv_obj_t *add_delete_list;
static int g_trend_range = 0; /* 0=实时 1=1小时 2=1天 3=1周 */
static int g_power_action; /* 1=reboot, 2=poweroff */

static void render_all(void);
static void on_msg_find(lv_event_t *ev);
static void on_msg_close(lv_event_t *ev);
static void on_clear_issue(lv_event_t *e);
static void populate_search_results(void);
static void on_search_focus(lv_event_t *e);
static int keyboard_is_visible(void);
static void hide_keyboard(void);

static int read_int_file(const char *path, int fallback)
{
    FILE *fp = fopen(path, "r");
    int value = fallback;
    if (!fp) return fallback;
    if (fscanf(fp, "%d", &value) != 1) value = fallback;
    fclose(fp);
    return value;
}

static void write_int_file(const char *path, int value)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fprintf(fp, "%d\n", value);
    fclose(fp);
}

static void set_screen_sleeping(int sleeping)
{
    const char *brightness = "/sys/class/backlight/backlight/brightness";
    const char *max_brightness = "/sys/class/backlight/backlight/max_brightness";
    const char *bl_power = "/sys/class/backlight/backlight/bl_power";

    if (sleep_overlay) {
        if (sleeping) {
            lv_obj_clear_flag(sleep_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(sleep_overlay);
        } else {
            lv_obj_add_flag(sleep_overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (!sleeping) {
        int cur = read_int_file(brightness, -1);
        int power = read_int_file(bl_power, 0);
        if (cur <= 0 || power != 0) {
            int max = read_int_file(max_brightness, 100);
            int restore = g_backlight_restore > 0 ? g_backlight_restore : max;
            if (restore <= 0) restore = 100;
            write_int_file(bl_power, 0);
            write_int_file(brightness, restore);
        }
    }

    if (sleeping == g_screen_sleeping) return;
    g_screen_sleeping = sleeping;

    if (sleeping) {
        int cur = read_int_file(brightness, -1);
        if (cur > 0) g_backlight_restore = cur;
        write_int_file(brightness, 0);
        write_int_file(bl_power, 4);
    } else {
        int max = read_int_file(max_brightness, 100);
        int restore = g_backlight_restore > 0 ? g_backlight_restore : max;
        if (restore <= 0) restore = 100;
        write_int_file(bl_power, 0);
        write_int_file(brightness, restore);
    }
}

static void style_plain(lv_obj_t *obj)
{
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, int size_hint, lv_color_t color)
{
    (void)size_hint;
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text ? text : "");
    lv_obj_set_style_text_font(obj, font_main, 0);
    lv_obj_set_style_text_color(obj, color, 0);
    return obj;
}

static lv_obj_t *card(lv_obj_t *parent, lv_color_t bg, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_style_bg_color(obj, bg, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 14, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static const char *state_name(fsm_state_t s)
{
    switch (s) {
    case FSM_ACTIVE: return "ACTIVE";
    case FSM_SLEEP: return "SLEEP";
    case FSM_FIND: return "FIND";
    case FSM_FAULT: return "FAULT";
    case FSM_BOOT:
    default: return "BOOT";
    }
}

static lv_color_t state_color(fsm_state_t s)
{
    switch (s) {
    case FSM_ACTIVE: return lv_color_hex(0x236A4B);
    case FSM_SLEEP: return lv_color_hex(0x3C5F8A);
    case FSM_FIND: return lv_color_hex(0xB7791F);
    case FSM_FAULT: return lv_color_hex(0x9F2F2F);
    case FSM_BOOT:
    default: return lv_color_hex(0x34515E);
    }
}

static void set_nav_visual(void)
{
    for (int i = 0; i < 5; ++i) {
        if (!nav_btn[i]) continue;
        int active = (i == (int)g_page);
        lv_obj_set_style_bg_color(nav_btn[i], active ? lv_color_hex(0x243B35) : lv_color_hex(0xE6DDD1), 0);
        if (lv_obj_get_child_count(nav_btn[i]) > 0) {
            lv_obj_t *txt = lv_obj_get_child(nav_btn[i], 0);
            lv_obj_set_style_text_color(txt, active ? lv_color_white() : lv_color_hex(0x42505A), 0);
        }
    }
}

static void switch_page(page_t page)
{
    g_page = page;
    if (page == PAGE_SEARCH) g_search_results_dirty = 1;
    if (keyboard && page != PAGE_SEARCH && page != PAGE_ADD && page != PAGE_NETWORK) {
        hide_keyboard();
    }
    set_nav_visual();
    render_all();
}

static void on_nav(lv_event_t *e)
{
    switch_page((page_t)(uintptr_t)lv_event_get_user_data(e));
}

static void update_top_and_footer(void)
{
    char buf[128];
    lv_obj_set_style_bg_color(topbar, state_color(g_fsm.state), 0);
    lv_label_set_text(state_pill, state_name(g_fsm.state));

    if (g_model.has_sensor) {
        snprintf(buf, sizeof(buf), "温度 %.1fC   湿度 %.1f%%   光照 %.0flx",
                 g_model.sensor.temperature, g_model.sensor.humidity, g_model.sensor.lux);
    } else {
        snprintf(buf, sizeof(buf), "等待传感器数据");
    }
    lv_label_set_text(sensor_label, buf);

    time_t now = time(NULL);
    struct tm *tmv = localtime(&now);
    if (tmv) {
        strftime(buf, sizeof(buf), "%H:%M", tmv);
        lv_label_set_text(time_label, buf);
    }
}

static void on_edit_toggle(lv_event_t *e)
{
    (void)e;
    g_shelf_edit_mode = !g_shelf_edit_mode;
    db_meta_set(&g_db, "shelf_issue", "0");
    db_meta_set(&g_db, "shelf_issue_text", "");
    render_all();
}

static void render_summary(void)
{
    char buf[64];
    lv_obj_clean(summary_row);
    lv_obj_set_flex_flow(summary_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(summary_row, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    struct item { const char *name; int value; lv_color_t color; } items[] = {
        {"在架", g_model.present_count, lv_color_hex(0x2E7D5B)},
        {"取走", g_model.missing_count, lv_color_hex(0x59748A)},
        {"错放", g_model.misplaced_count, lv_color_hex(0xB7791F)},
    };

    for (int i = 0; i < 3; ++i) {
        lv_obj_t *pill = card(summary_row, lv_color_hex(0xFFF9EF), 16);
        lv_obj_set_size(pill, 170, 42);
        lv_obj_set_style_pad_hor(pill, 20, 0);
        lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        label(pill, items[i].name, 18, lv_color_hex(0x536068));
        snprintf(buf, sizeof(buf), "%d", items[i].value);
        label(pill, buf, 24, items[i].color);
    }

    lv_obj_t *edit = lv_btn_create(summary_row);
    lv_obj_set_size(edit, 170, 42);
    lv_obj_set_style_radius(edit, 16, 0);
    lv_obj_set_style_shadow_width(edit, 0, 0);
    lv_obj_set_style_bg_color(edit, g_shelf_edit_mode ? lv_color_hex(0x005CFF) : lv_color_hex(0x243B35), 0);
    lv_obj_add_event_cb(edit, on_edit_toggle, LV_EVENT_CLICKED, NULL);
    lv_obj_t *txt = label(edit, g_shelf_edit_mode ? "完成调整" : "调整顺序", 18, lv_color_white());
    lv_obj_center(txt);
}

static lv_color_t book_color(const ui_book_t *b)
{
    if (!b->known) return lv_color_hex(0xB36B94);
    if (b->misplaced) return lv_color_hex(0xA56A2A);
    if (!b->is_present) return lv_color_hex(0xAEB8BE);
    return lv_color_hex(0x5F7F62);
}

static int book_raw_x(const ui_book_t *b, int shelf_w, int fallback_index, int fallback_count)
{
    double start = b->start_cm;
    double end = b->end_cm;
    if (!(end > start + 0.5)) {
        start = b->expected_start;
        end = b->expected_end;
    }
    if (end > start + 0.5) {
        int x = 20 + (int)(start / SHELF_LEN_CM * (shelf_w - 60));
        if (x < 12) x = 12;
        if (x > shelf_w - 54) x = shelf_w - 54;
        return x;
    }
    if (fallback_count <= 0) return 20;
    return 20 + fallback_index * ((shelf_w - 80) / fallback_count);
}

static int book_w(const ui_book_t *b)
{
    double start = b->expected_start;
    double end = b->expected_end;
    if (!(end > start + 0.5)) {
        start = b->start_cm;
        end = b->end_cm;
    }
    int w = (end > start + 0.5) ? (int)((end - start) * 18.0) : 46;
    if (w < 38) w = 38;
    if (w > 116) w = 116;
    return w;
}

static double book_width_cm(const ui_book_t *b)
{
    double w = 0.0;
    if (!b) return 1.2;
    if (b->expected_end > b->expected_start + 0.1)
        w = b->expected_end - b->expected_start;
    else if (b->end_cm > b->start_cm + 0.1)
        w = b->end_cm - b->start_cm;
    if (w <= 0.1) w = 1.2;
    if (w < 0.6) w = 0.6;
    if (w > 8.0) w = 8.0;
    return w;
}

static double clamp_start_cm(double start, double width)
{
    if (width <= 0.1) width = 1.2;
    if (start < 0.0) start = 0.0;
    if (start + width > SHELF_LEN_CM) start = SHELF_LEN_CM - width;
    if (start < 0.0) start = 0.0;
    return start;
}

static double snap_half_cm(double start)
{
    int half_steps = (int)(start * 2.0 + 0.5);
    return (double)half_steps / 2.0;
}

static double abs_cm(double v)
{
    return v < 0.0 ? -v : v;
}

static double snap_book_start_cm(int layer_id, const char *epc, double start, double width)
{
    const double snap_threshold = 0.8;
    double best = snap_half_cm(start);
    double best_diff = abs_cm(best - start);

    if (width <= 0.1) width = 1.2;

    if (abs_cm(start) < best_diff && abs_cm(start) <= snap_threshold) {
        best = 0.0;
        best_diff = abs_cm(start);
    }
    if (abs_cm(start + width - SHELF_LEN_CM) < best_diff &&
        abs_cm(start + width - SHELF_LEN_CM) <= snap_threshold) {
        best = SHELF_LEN_CM - width;
        best_diff = abs_cm(start + width - SHELF_LEN_CM);
    }

    for (int i = 0; i < g_model.count; i++) {
        const ui_book_t *b = &g_model.books[i];
        int layer = b->layer >= 0 ? b->layer : b->expected_layer;
        double edges[2];
        if (!b->is_present || layer != layer_id) continue;
        if (epc && strcmp(b->epc, epc) == 0) continue;
        edges[0] = b->start_cm;
        edges[1] = b->end_cm;
        for (int e = 0; e < 2; e++) {
            double candidates[2] = { edges[e], edges[e] - width };
            for (int c = 0; c < 2; c++) {
                double diff = abs_cm(candidates[c] - start);
                if (diff <= snap_threshold && diff < best_diff) {
                    best = candidates[c];
                    best_diff = diff;
                }
            }
        }
    }

    return clamp_start_cm(best, width);
}

typedef struct {
    const ui_book_t *book;
    double start;
    double width;
} manual_order_item_t;

static int compare_manual_order(const void *a, const void *b)
{
    const manual_order_item_t *ia = (const manual_order_item_t *)a;
    const manual_order_item_t *ib = (const manual_order_item_t *)b;
    if (ia->start < ib->start) return -1;
    if (ia->start > ib->start) return 1;
    return strcmp(ia->book->title, ib->book->title);
}

static void manual_reorder_layer(int layer_id, const char *moved_epc, double target_start)
{
    manual_order_item_t items[UI_MAX_BOOKS];
    int n = 0;
    double moved_start = target_start;
    double moved_width = 1.2;
    double total_width = 0.0;

    if (!g_db_ready || !moved_epc || !moved_epc[0]) return;
    for (int i = 0; i < g_model.count && n < UI_MAX_BOOKS; i++) {
        ui_book_t *b = &g_model.books[i];
        int layer = b->layer >= 0 ? b->layer : b->expected_layer;
        double s;
        if (!b->is_present || layer != layer_id) continue;
        s = (strcmp(b->epc, moved_epc) == 0) ? target_start : b->start_cm;
        if (!(s >= 0.0 && s <= SHELF_LEN_CM)) s = b->expected_start;
        items[n].book = b;
        items[n].width = book_width_cm(b);
        if (strcmp(b->epc, moved_epc) == 0)
            s = snap_book_start_cm(layer_id, moved_epc, s, items[n].width);
        items[n].start = clamp_start_cm(s, items[n].width);
        if (strcmp(b->epc, moved_epc) == 0) {
            moved_start = items[n].start;
            moved_width = items[n].width;
        }
        total_width += items[n].width;
        n++;
    }
    if (n <= 0) return;

    qsort(items, (size_t)n, sizeof(items[0]), compare_manual_order);
    double before_width = 0.0;
    double block_start = 0.0;
    int found_moved = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(items[i].book->epc, moved_epc) == 0) {
            found_moved = 1;
            break;
        }
        before_width += items[i].width;
    }
    (void)moved_width;
    block_start = found_moved ? moved_start - before_width : items[0].start;
    if (block_start + total_width > SHELF_LEN_CM) block_start = SHELF_LEN_CM - total_width;
    if (block_start < 0.0) block_start = 0.0;

    double cursor = block_start;
    for (int i = 0; i < n; i++) {
        const ui_book_t *b = items[i].book;
        double s = cursor;
        double e = s + items[i].width;
        db_shelf_upsert(&g_db, b->epc, b->title, "",
                        layer_id, s, e, b->rssi, 1);
        db_log_add(&g_db, b->epc, b->title, "manual",
                   layer_id, s, e, b->rssi, "screen manual order update");
        cursor = e;
    }
    db_meta_set(&g_db, "shelf_issue", "0");
    db_meta_set(&g_db, "shelf_issue_text", "");
    ui_model_load(&g_db, &g_model);
}

static double pointer_to_start_cm(lv_obj_t *shelf, lv_obj_t *spine, int shelf_w)
{
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t p;
    lv_area_t area;
    int w = lv_obj_get_width(spine);
    int x;
    double start;

    if (!indev || !shelf || !spine) return 0.0;
    lv_indev_get_point(indev, &p);
    lv_obj_get_coords(shelf, &area);
    x = p.x - area.x1 - w / 2;
    if (x < 20) x = 20;
    if (x > shelf_w - 40 - w) x = shelf_w - 40 - w;
    start = (double)(x - 20) / (double)(shelf_w - 60) * SHELF_LEN_CM;
    return start;
}

static void ir_layer_bits(int layer_id, uint32_t *known, uint32_t *active)
{
    int left_board = layer_id == 0 ? 0 : 2;
    int right_board = layer_id == 0 ? 1 : 3;
    uint32_t k = 0;
    uint32_t a = 0;

    if (left_board >= 0 && left_board < 4 && g_fsm.ir_ok[left_board]) {
        k |= 0x0000FFFFu;
        a |= (uint32_t)g_fsm.ir_bitmap[left_board];
    }
    if (right_board >= 0 && right_board < 4 && g_fsm.ir_ok[right_board]) {
        k |= 0xFFFF0000u;
        a |= ((uint32_t)g_fsm.ir_bitmap[right_board]) << 16;
    }
    if (known) *known = k;
    if (active) *active = a;
}

static void update_ir_dots_visual(int layer_id)
{
    uint32_t known = 0;
    uint32_t active = 0;

    if (layer_id < 0 || layer_id > 1) return;
    ir_layer_bits(layer_id, &known, &active);
    for (int i = 0; i < 32; i++) {
        lv_obj_t *p = ir_dot[layer_id][i];
        int is_known = (known & (1u << i)) != 0;
        int is_active = (active & (1u << i)) != 0;
        if (!p) continue;
        lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(p,
                                  is_active ? lv_color_hex(0x005CFF) :
                                  is_known ? lv_color_hex(0xD2CCC1) :
                                             lv_color_hex(0xE7DDD0), 0);
        lv_obj_set_style_border_width(p, is_active ? 2 : 0, 0);
        lv_obj_set_style_border_color(p, lv_color_hex(0xFFFFFF), 0);
    }
}

static void render_ir_dots(lv_obj_t *shelf, int layer_id, int shelf_w)
{
    uint32_t known = 0;
    uint32_t active = 0;
    const int count = 32;
    const int dot = 10;
    const int x0 = 46;
    const int span = 852;

    ir_layer_bits(layer_id, &known, &active);
    for (int i = 0; i < count; i++) {
        int x = x0 + (i * span) / (count - 1);
        int is_known = (known & (1u << i)) != 0;
        int is_active = (active & (1u << i)) != 0;
        lv_obj_t *p = lv_obj_create(shelf);
        style_plain(p);
        lv_obj_set_size(p, dot, dot);
        lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(p,
                                  is_active ? lv_color_hex(0x005CFF) :
                                  is_known ? lv_color_hex(0xD2CCC1) :
                                             lv_color_hex(0xE7DDD0), 0);
        lv_obj_set_style_border_width(p, is_active ? 2 : 0, 0);
        lv_obj_set_style_border_color(p, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(p, LV_ALIGN_BOTTOM_LEFT, x, -5);
        ir_dot[layer_id][i] = p;
    }
}

static void book_display_pos(const ui_book_t *b, double *start, double *end)
{
    double s = b->start_cm;
    double e = b->end_cm;
    if (!(e > s + 0.5)) {
        s = b->expected_start;
        e = b->expected_end;
    }
    if (!(e > s + 0.5)) {
        s = 0.0;
        e = 0.0;
    }
    if (start) *start = s;
    if (end) *end = e;
}

static void on_book_click(lv_event_t *e)
{
    const ui_book_t *b = (const ui_book_t *)lv_event_get_user_data(e);
    if (g_shelf_edit_mode) return;
    if (!b) return;

    char msg[512];
    double start, end;
    book_display_pos(b, &start, &end);
    snprintf(msg, sizeof(msg),
             "%s\n\n位置: %s %.1f-%.1fcm\n状态: %s%s\nEPC: %s",
             b->title,
             ui_layer_name(b->layer >= 0 ? b->layer : b->expected_layer),
             start, end,
             b->is_present ? "在架" : "取走",
             b->misplaced ? " / 错放" : "",
             b->epc);

    lv_obj_t *box = lv_msgbox_create(NULL);
    lv_obj_set_size(box, 380, 290);
    lv_obj_t *t = lv_msgbox_add_title(box, "书籍详情");
    lv_obj_set_style_text_font(t, font_main, 0);
    lv_obj_t *body = lv_msgbox_add_text(box, msg);
    lv_obj_set_style_text_font(body, font_main, 0);
    lv_obj_set_width(body, 330);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_t *find = lv_msgbox_add_footer_button(box, "查找");
    lv_obj_t *close = lv_msgbox_add_footer_button(box, "关闭");
    lv_obj_set_style_text_font(find, font_main, 0);
    lv_obj_set_style_text_font(close, font_main, 0);
    lv_obj_add_event_cb(find, on_msg_find, LV_EVENT_CLICKED, (void *)b);
    lv_obj_add_event_cb(close, on_msg_close, LV_EVENT_CLICKED, NULL);
}

static void on_book_drag(lv_event_t *e)
{
    const ui_book_t *b = (const ui_book_t *)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *spine = lv_event_get_target(e);
    lv_obj_t *shelf = lv_obj_get_parent(spine);
    int shelf_w = 944;
    double start;
    int x;

    if (!g_shelf_edit_mode || !b || !spine || !shelf) return;
    if (code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED) return;

    start = pointer_to_start_cm(shelf, spine, shelf_w);
    start = snap_book_start_cm(b->layer >= 0 ? b->layer : b->expected_layer,
                               b->epc, start, book_width_cm(b));
    x = 20 + (int)(start / SHELF_LEN_CM * (shelf_w - 60));
    lv_obj_align(spine, LV_ALIGN_BOTTOM_LEFT, x, -34);

    if (code == LV_EVENT_RELEASED) {
        int layer = b->layer >= 0 ? b->layer : b->expected_layer;
        manual_reorder_layer(layer, b->epc, start);
        render_all();
    }
}

static void close_msg_from_button(lv_event_t *ev)
{
    lv_obj_t *btn = lv_event_get_target(ev);
    lv_obj_t *box = lv_obj_get_parent(lv_obj_get_parent(btn));
    if (box) lv_msgbox_close(box);
}

static void on_msg_find(lv_event_t *ev)
{
    const ui_book_t *book = (const ui_book_t *)lv_event_get_user_data(ev);
    if (book) {
        g_find_return_page = g_page;
        snprintf(g_find_epc, sizeof(g_find_epc), "%s", book->epc);
        g_find_started_at = time(NULL);
        g_find_completed_at = 0;
        g_find_active = 1;
        ui_cmd_find(book->epc);
        switch_page(PAGE_SHELF);
    }
    close_msg_from_button(ev);
}

static void on_msg_close(lv_event_t *ev)
{
    close_msg_from_button(ev);
}

static void render_layer(lv_obj_t *parent, int layer_id)
{
    int shelf_w = 944;
    int count = 0;
    int next_x = 22;
    char title[32];
    snprintf(title, sizeof(title), "%s书架", ui_layer_name(layer_id));

    lv_obj_t *shelf = card(parent, lv_color_hex(0xF8F0E4), 20);
    lv_obj_set_size(shelf, shelf_w, 168);
    lv_obj_set_style_border_width(shelf, 2, 0);
    lv_obj_set_style_border_color(shelf, lv_color_hex(0xD2B98E), 0);

    lv_obj_t *shelf_title = label(shelf, title, 20, lv_color_hex(0x4D3E30));
    lv_obj_align(shelf_title, LV_ALIGN_TOP_LEFT, 4, 0);

    for (int i = 0; i < g_model.count; ++i) {
        if (!g_model.books[i].is_present) continue;
        int layer = g_model.books[i].layer >= 0 ? g_model.books[i].layer : g_model.books[i].expected_layer;
        if (layer == layer_id) count++;
    }

    int seen = 0;
    for (int i = 0; i < g_model.count; ++i) {
        const ui_book_t *b = &g_model.books[i];
        if (!b->is_present) continue;
        int layer = b->layer >= 0 ? b->layer : b->expected_layer;
        if (layer != layer_id) continue;

        int raw_x = book_raw_x(b, shelf_w, seen, count);
        int w = book_w(b);
        int x = raw_x;
        if (!g_shelf_edit_mode && x < next_x) x = next_x;
        if (x + w > shelf_w - 24) {
            x = shelf_w - 24 - w;
            if (!g_shelf_edit_mode && x < next_x) {
                w = shelf_w - 24 - next_x;
                x = next_x;
                if (w < 28) w = 28;
            }
        }
        next_x = x + w + 6;
        seen++;

        lv_obj_t *spine = lv_btn_create(shelf);
        lv_obj_set_size(spine, w, 86);
        lv_obj_align(spine, LV_ALIGN_BOTTOM_LEFT, x, -34);
        lv_obj_set_style_radius(spine, 8, 0);
        lv_obj_set_style_bg_color(spine, book_color(b), 0);
        lv_obj_set_style_shadow_width(spine, 0, 0);
        lv_obj_set_style_border_width(spine, g_shelf_edit_mode ? 3 : (b->is_present ? 0 : 2), 0);
        lv_obj_set_style_border_color(spine, g_shelf_edit_mode ? lv_color_hex(0x005CFF) : lv_color_hex(0x7F8B91), 0);
        lv_obj_add_event_cb(spine, on_book_click, LV_EVENT_CLICKED, (void *)b);
        lv_obj_add_event_cb(spine, on_book_drag, LV_EVENT_ALL, (void *)b);

        lv_obj_t *txt = label(spine, b->title, 16, lv_color_white());
        lv_label_set_long_mode(txt, LV_LABEL_LONG_DOT);
        lv_obj_set_width(txt, w - 8);
        lv_obj_center(txt);

        if (b->misplaced) {
            lv_obj_t *bar = lv_obj_create(spine);
            style_plain(bar);
            lv_obj_set_size(bar, 5, 76);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
            lv_obj_align(bar, LV_ALIGN_LEFT_MID, 0, 0);
        }
    }

    if (count == 0) {
        lv_obj_t *empty = label(shelf, "当前无在架书", 22, lv_color_hex(0x8A7A68));
        lv_obj_center(empty);
    }

    lv_obj_t *base = lv_obj_create(shelf);
    style_plain(base);
    lv_obj_set_size(base, shelf_w - 44, 10);
    lv_obj_set_style_bg_color(base, lv_color_hex(0x8B643C), 0);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(base, 4, 0);
    lv_obj_align(base, LV_ALIGN_BOTTOM_MID, 0, -18);

    render_ir_dots(shelf, layer_id, shelf_w);
}

static void render_issue_banner(void)
{
    char buf[192];
    int show_misplaced = g_model.misplaced_count > g_ack_misplaced_count;
    if (g_model.misplaced_count <= 0) g_ack_misplaced_count = 0;
    if (!g_fsm.shelf_issue && !show_misplaced) return;

    lv_obj_t *box = card(content, lv_color_hex(0xFFF0F6), 12);
    lv_obj_set_size(box, 944, 44);
    lv_obj_set_style_pad_hor(box, 16, 0);
    lv_obj_set_style_pad_ver(box, 6, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0xC04B8D), 0);
    snprintf(buf, sizeof(buf), "%s%s",
             g_fsm.shelf_issue_text[0] ? g_fsm.shelf_issue_text : "书架状态需要人工整理",
             show_misplaced ? "；存在错放" : "");
    lv_obj_t *txt = label(box, buf, 18, lv_color_hex(0x8A2F62));
    lv_obj_set_width(txt, 780);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_DOT);
    lv_obj_align(txt, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *btn = lv_btn_create(box);
    lv_obj_set_size(btn, 104, 32);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x8A2F62), 0);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(btn, on_clear_issue, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bt = label(btn, "已整理", 16, lv_color_white());
    lv_obj_center(bt);
}

static void on_clear_issue(lv_event_t *e)
{
    (void)e;
    ui_cmd_clear_issue();
    db_meta_set(&g_db, "shelf_issue", "0");
    db_meta_set(&g_db, "shelf_issue_text", "");
    g_fsm.shelf_issue = 0;
    g_fsm.shelf_issue_text[0] = '\0';
    g_ack_misplaced_count = g_model.misplaced_count;
    render_all();
}

static void render_placeholder(const char *headline, const char *hint)
{
    lv_obj_t *box = card(content, lv_color_hex(0xFFF9EF), 24);
    lv_obj_set_size(box, 944, 382);
    lv_obj_center(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    label(box, headline, 28, state_color(g_fsm.state));
    lv_obj_t *h = label(box, hint, 20, lv_color_hex(0x63717A));
    lv_obj_set_style_margin_top(h, 16, 0);
}

static const char *ok_text(int ok)
{
    return ok ? "正常" : "异常";
}

static void add_fault_row(lv_obj_t *parent, const char *name, int ok, const char *detail)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "%s: %s%s%s", name, ok_text(ok),
             detail && detail[0] ? "  " : "", detail && detail[0] ? detail : "");
    label(parent, buf, 18, lv_color_hex(ok ? 0x2F6B4F : 0x9F2F2F));
}

static void on_fault_retry(lv_event_t *e)
{
    (void)e;
    ui_cmd_fault_retry();
    render_all();
}

static void render_fault(void)
{
    char buf[128];
    lv_obj_t *box = card(content, lv_color_hex(0xFFF2F2), 18);
    lv_obj_set_size(box, 944, 382);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 8, 0);

    label(box, "系统故障", 28, lv_color_hex(0x9F2F2F));
    snprintf(buf, sizeof(buf), "故障代码: %d  正常功能已暂停", g_fsm.fault_dev);
    label(box, buf, 18, lv_color_hex(0x5A4A4A));

    lv_obj_t *grid = lv_obj_create(box);
    style_plain(grid);
    lv_obj_set_size(grid, 900, 210);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(grid, 4, 0);

    add_fault_row(grid, "LED灯带", g_fsm.mod_led_ok, "");
    add_fault_row(grid, "雷达", g_fsm.mod_radar_ok, "");
    add_fault_row(grid, "UHF", g_fsm.mod_uhf_ok, "上/下层读写器");
    add_fault_row(grid, "WiFi", g_fsm.mod_wifi_ok, g_fsm.wifi_error);
    snprintf(buf, sizeof(buf), "BH1750=%d SHT30=%d", g_fsm.bh1750_ok, g_fsm.sht30_ok);
    add_fault_row(grid, "传感器", g_fsm.mod_sensor_ok, buf);
    add_fault_row(grid, "MQTT", g_fsm.mod_mqtt_ok || !g_fsm.wifi_connected, g_fsm.mqtt_connected ? "已连接" : "离线");
    add_fault_row(grid, "屏幕", g_fsm.mod_screen_ok, "");

    lv_obj_t *retry = lv_btn_create(box);
    lv_obj_set_size(retry, 160, 44);
    lv_obj_set_style_radius(retry, 14, 0);
    lv_obj_set_style_shadow_width(retry, 0, 0);
    lv_obj_set_style_bg_color(retry, lv_color_hex(0x9F2F2F), 0);
    lv_obj_add_event_cb(retry, on_fault_retry, LV_EVENT_CLICKED, NULL);
    lv_obj_t *txt = label(retry, "重新检测", 18, lv_color_white());
    lv_obj_center(txt);
}

static void render_shelf(void)
{
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 10, 0);
    render_summary();

    if (!g_db_ready) {
        render_placeholder("数据库未连接", "请确认 /root/bookshelf.db 可读");
        return;
    }
    if (g_fsm.state == FSM_BOOT) {
        render_placeholder("系统启动中", g_fsm.exists ? "正在等待 FSM 建立书架基线" : "等待 FSM 写入 /tmp/fsm_state");
        return;
    }
    if (g_fsm.state == FSM_SLEEP) {
        render_placeholder("书架休眠中", "靠近书架后会自动恢复盘点");
        return;
    }
    if (g_fsm.state == FSM_FAULT) {
        render_fault();
        return;
    }

    render_issue_banner();
    render_layer(content, 0);
    render_layer(content, 1);
}

static void on_search_changed(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    snprintf(g_search_query, sizeof(g_search_query), "%s", lv_textarea_get_text(ta));
    g_search_results_dirty = 1;
    populate_search_results();
}

static void on_search_focus(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);
    if (!keyboard) return;

    if (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED) {
        lv_keyboard_set_textarea(keyboard, ta);
        lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_DEFOCUSED || code == LV_EVENT_READY ||
               code == LV_EVENT_CANCEL) {
        hide_keyboard();
    }
}

static int keyboard_is_visible(void)
{
    return keyboard && !lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void hide_keyboard(void)
{
    if (!keyboard) return;
    lv_keyboard_set_textarea(keyboard, NULL);
    lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void on_keyboard_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    (void)e;
    if (code == LV_EVENT_CANCEL || code == LV_EVENT_READY) {
        hide_keyboard();
    }
}

static void on_text_focus(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);
    on_search_focus(e);
    if (g_page == PAGE_ADD && add_form_box &&
        (code == LV_EVENT_FOCUSED || code == LV_EVENT_CLICKED)) {
        lv_obj_update_layout(add_form_box);
        if (ta == add_pages_box)
            lv_obj_scroll_to_y(add_form_box, 135, LV_ANIM_OFF);
        else if (ta == add_title_box)
            lv_obj_scroll_to_y(add_form_box, 70, LV_ANIM_OFF);
    }
}

static void on_result_find(lv_event_t *e)
{
    const ui_book_t *b = (const ui_book_t *)lv_event_get_user_data(e);
    if (!b) return;
    g_find_return_page = g_page;
    snprintf(g_find_epc, sizeof(g_find_epc), "%s", b->epc);
    g_find_started_at = time(NULL);
    g_find_completed_at = 0;
    g_find_active = 1;
    ui_cmd_find(b->epc);
    hide_keyboard();
    switch_page(PAGE_SEARCH);
}

static void populate_search_results(void)
{
    int shown = 0;
    if (!search_results) return;
    if (!g_search_results_dirty) return;
    g_search_results_dirty = 0;
    lv_obj_clean(search_results);

    for (int i = 0; i < g_model.count; ++i) {
        const ui_book_t *b = &g_model.books[i];
        char row_text[256];
        double start, end;
        if (!ui_book_match(b, g_search_query)) continue;
        book_display_pos(b, &start, &end);
        snprintf(row_text, sizeof(row_text), "%s   %s %.1f-%.1fcm   %s",
                 b->title, ui_layer_name(b->layer >= 0 ? b->layer : b->expected_layer),
                 start, end, b->is_present ? "在架" : "取走");
        lv_obj_t *row = lv_btn_create(search_results);
        lv_obj_set_size(row, 900, 46);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_bg_color(row, book_color(b), 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_add_event_cb(row, on_result_find, LV_EVENT_CLICKED, (void *)b);
        lv_obj_t *txt = label(row, row_text, 18, lv_color_white());
        lv_obj_align(txt, LV_ALIGN_LEFT_MID, 16, 0);
        shown++;
    }

    if (shown == 0) {
        lv_obj_t *empty = label(search_results, "没有匹配书籍", 22, lv_color_hex(0x7A6A5E));
        lv_obj_center(empty);
    }
}

static void refresh_search_results_keep_scroll(void)
{
    int y;
    if (!search_results) return;
    y = lv_obj_get_scroll_y(search_results);
    g_search_results_dirty = 1;
    populate_search_results();
    lv_obj_scroll_to_y(search_results, y, LV_ANIM_OFF);
}

static void render_search(void)
{
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 12, 0);

    search_box = lv_textarea_create(content);
    lv_obj_set_size(search_box, 944, 54);
    lv_obj_set_style_text_font(search_box, font_main, 0);
    lv_textarea_set_one_line(search_box, true);
    lv_textarea_set_placeholder_text(search_box, "输入书名 / 拼音 / EPC");
    lv_textarea_set_text(search_box, g_search_query);
    lv_obj_add_event_cb(search_box, on_search_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(search_box, on_search_focus, LV_EVENT_ALL, NULL);

    search_results = card(content, lv_color_hex(0xFFF9EF), 20);
    lv_obj_set_size(search_results, 944, 330);
    lv_obj_set_flex_flow(search_results, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(search_results, 8, 0);
    lv_obj_add_flag(search_results, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(search_results, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(search_results, LV_SCROLLBAR_MODE_AUTO);
    populate_search_results();
}

static void on_cancel_find(lv_event_t *e)
{
    (void)e;
    ui_cmd_cancel_find();
    g_find_epc[0] = '\0';
    g_find_started_at = 0;
    g_find_completed_at = 0;
    g_find_active = 0;
    switch_page(g_find_return_page);
}

static void on_add_start(lv_event_t *e)
{
    (void)e;
    ui_cmd_add_start();
    render_all();
}

static void on_add_cancel(lv_event_t *e)
{
    (void)e;
    ui_cmd_add_cancel();
    g_add_title_draft[0] = '\0';
    snprintf(g_add_pages_draft, sizeof(g_add_pages_draft), "200");
    render_all();
}

static void on_add_input_changed(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    const char *text = lv_textarea_get_text(ta);
    if (ta == add_title_box) {
        snprintf(g_add_title_draft, sizeof(g_add_title_draft), "%s", text ? text : "");
    } else if (ta == add_pages_box) {
        snprintf(g_add_pages_draft, sizeof(g_add_pages_draft), "%s", text ? text : "");
    }
}

static void on_add_save(lv_event_t *e)
{
    const char *title;
    int pages = 200;
    int layer = g_fsm.pending_layer >= 0 ? g_fsm.pending_layer : 0;
    (void)e;
    if (!add_title_box) return;
    title = lv_textarea_get_text(add_title_box);
    if (add_pages_box) pages = atoi(lv_textarea_get_text(add_pages_box));
    if (pages < 1) pages = 200;
    ui_cmd_register_book(layer, pages, title);
    g_add_title_draft[0] = '\0';
    snprintf(g_add_pages_draft, sizeof(g_add_pages_draft), "200");
    hide_keyboard();
    render_all();
}

static void on_delete_cancel(lv_event_t *e)
{
    close_msg_from_button(e);
}

static void on_delete_confirm(lv_event_t *e)
{
    if (g_delete_epc[0]) ui_cmd_delete_book(g_delete_epc);
    g_delete_epc[0] = '\0';
    g_delete_title[0] = '\0';
    close_msg_from_button(e);
    render_all();
}

static void on_delete_book(lv_event_t *e)
{
    const ui_book_t *b = (const ui_book_t *)lv_event_get_user_data(e);
    char msg[320];
    if (!b || !b->epc[0]) return;

    snprintf(g_delete_epc, sizeof(g_delete_epc), "%s", b->epc);
    snprintf(g_delete_title, sizeof(g_delete_title), "%s", b->title[0] ? b->title : b->epc);
    snprintf(msg, sizeof(msg),
             "确定删除这本书吗？\n\n%s\n\n删除后会从库存和书架状态中移除。",
             g_delete_title);

    lv_obj_t *box = lv_msgbox_create(NULL);
    lv_obj_set_size(box, 430, 270);
    lv_obj_t *t = lv_msgbox_add_title(box, "确认删除");
    lv_obj_set_style_text_font(t, font_main, 0);
    lv_obj_t *body = lv_msgbox_add_text(box, msg);
    lv_obj_set_style_text_font(body, font_main, 0);
    lv_obj_set_width(body, 380);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_t *del = lv_msgbox_add_footer_button(box, "删除");
    lv_obj_t *cancel = lv_msgbox_add_footer_button(box, "取消");
    lv_obj_set_style_text_font(del, font_main, 0);
    lv_obj_set_style_text_font(cancel, font_main, 0);
    lv_obj_set_style_bg_color(del, lv_color_hex(0x9B2D2D), 0);
    lv_obj_add_event_cb(del, on_delete_confirm, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(cancel, on_delete_cancel, LV_EVENT_CLICKED, NULL);
}

static void render_find(void)
{
    const ui_book_t *b = ui_model_find_epc(&g_model, g_find_epc);
    char buf[256];
    int elapsed = g_find_started_at ? (int)(time(NULL) - g_find_started_at) : 0;

    lv_obj_t *box = card(content, lv_color_hex(0xFFF4D8), 24);
    lv_obj_set_size(box, 944, 382);
    lv_obj_center(box);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    if (g_fsm.find_status == 2) {
        snprintf(buf, sizeof(buf), "已取出: %s",
                 b ? b->title : (g_fsm.find_target_title[0] ? g_fsm.find_target_title : g_find_epc));
    } else if (g_fsm.find_status == 3) {
        snprintf(buf, sizeof(buf), "查找超时: %s",
                 b ? b->title : (g_fsm.find_target_title[0] ? g_fsm.find_target_title : g_find_epc));
    } else {
        snprintf(buf, sizeof(buf), "正在查找: %s", b ? b->title : (g_find_epc[0] ? g_find_epc : "等待目标"));
    }
    label(box, buf, 28, lv_color_hex(0x634410));
    if (b) {
        double start, end;
        book_display_pos(b, &start, &end);
        snprintf(buf, sizeof(buf), "%s %.1f-%.1fcm",
                 ui_layer_name(b->layer >= 0 ? b->layer : b->expected_layer),
                 start, end);
        lv_obj_t *pos = label(box, buf, 20, lv_color_hex(0x6C5B3B));
        lv_obj_set_style_margin_top(pos, 14, 0);
    }

    lv_obj_t *hint = label(box,
                           g_fsm.find_status == 2 ? "系统已检测到目标书被取走" :
                           elapsed > 15 || g_fsm.find_status == 3 ? "未检测到取书，请确认书是否在架" :
                           "请看书架上的黄色灯光",
                           22, lv_color_hex(0x7A4D14));
    lv_obj_set_style_margin_top(hint, 28, 0);

    lv_obj_t *cancel = lv_btn_create(box);
    lv_obj_set_size(cancel, 180, 44);
    lv_obj_set_style_radius(cancel, 18, 0);
    lv_obj_set_style_bg_color(cancel, lv_color_hex(0x8A4B35), 0);
    lv_obj_set_style_shadow_width(cancel, 0, 0);
    lv_obj_set_style_margin_top(cancel, 22, 0);
    lv_obj_add_event_cb(cancel, on_cancel_find, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ct = label(cancel, "取消查找", 18, lv_color_white());
    lv_obj_center(ct);
}

static lv_obj_t *make_small_button(lv_obj_t *parent, const char *text, lv_color_t color,
                                   lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 150, 42);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, color, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *txt = label(btn, text, 18, lv_color_white());
    lv_obj_center(txt);
    return btn;
}

static void on_trend_range(lv_event_t *e)
{
    g_trend_range = (int)(uintptr_t)lv_event_get_user_data(e);
    render_all();
}

static lv_obj_t *make_trend_button(lv_obj_t *parent, const char *text, int range)
{
    lv_obj_t *btn = lv_btn_create(parent);
    int active = (g_trend_range == range);
    lv_obj_set_size(btn, 92, 34);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(active ? 0x2F6B4F : 0xD9D2C4), 0);
    lv_obj_add_event_cb(btn, on_trend_range, LV_EVENT_CLICKED, (void *)(uintptr_t)range);
    lv_obj_t *txt = label(btn, text, 16, lv_color_hex(active ? 0xFFFFFF : 0x42505A));
    lv_obj_center(txt);
    return btn;
}

static const char *action_cn(const char *action)
{
    if (!action) return "事件";
    if (!strcmp(action, "borrowed")) return "取走";
    if (!strcmp(action, "returned")) return "放回";
    if (!strcmp(action, "misplaced")) return "错放";
    if (!strcmp(action, "registered")) return "登记";
    if (!strcmp(action, "found")) return "找到";
    if (!strcmp(action, "manual")) return "手动调整";
    if (!strcmp(action, "baseline_mismatch")) return "开机不一致";
    if (!strcmp(action, "complex")) return "复杂取放";
    return action;
}

static void on_power_cancel(lv_event_t *e)
{
    g_power_action = 0;
    close_msg_from_button(e);
}

static void on_power_confirm(lv_event_t *e)
{
    int action = g_power_action;
    g_power_action = 0;
    close_msg_from_button(e);
    if (action == 1) {
        ui_cmd_system_reboot();
    } else if (action == 2) {
        ui_cmd_system_poweroff();
    }
}

static void show_power_confirm(int action)
{
    const char *title = action == 1 ? "确认重启" : "确认关机";
    const char *body_text = action == 1 ?
        "确定要重启书架吗？\n\n系统会先停止盘点并保存状态。" :
        "确定要关闭书架吗？\n\n关闭后需要重新上电才能启动。";
    lv_obj_t *box;
    lv_obj_t *t;
    lv_obj_t *body;
    lv_obj_t *ok;
    lv_obj_t *cancel;

    g_power_action = action;
    box = lv_msgbox_create(NULL);
    lv_obj_set_size(box, 430, 245);
    t = lv_msgbox_add_title(box, title);
    lv_obj_set_style_text_font(t, font_main, 0);
    body = lv_msgbox_add_text(box, body_text);
    lv_obj_set_style_text_font(body, font_main, 0);
    lv_obj_set_width(body, 380);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    ok = lv_msgbox_add_footer_button(box, action == 1 ? "重启" : "关机");
    cancel = lv_msgbox_add_footer_button(box, "取消");
    lv_obj_set_style_text_font(ok, font_main, 0);
    lv_obj_set_style_text_font(cancel, font_main, 0);
    lv_obj_set_style_bg_color(ok, lv_color_hex(action == 1 ? 0x8A5A2B : 0x9B2D2D), 0);
    lv_obj_add_event_cb(ok, on_power_confirm, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(cancel, on_power_cancel, LV_EVENT_CLICKED, NULL);
}

static void on_reboot_click(lv_event_t *e)
{
    (void)e;
    show_power_confirm(1);
}

static void on_poweroff_click(lv_event_t *e)
{
    (void)e;
    show_power_confirm(2);
}

static void render_recent_summary(lv_obj_t *box)
{
    char buf[224];
    book_log_t logs[8];
    int log_n = g_db_ready ? db_log_recent(&g_db, 8, logs, 8) : 0;

    lv_obj_t *summary = lv_obj_create(box);
    style_plain(summary);
    lv_obj_set_size(summary, 900, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(summary, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(summary, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(summary, 5, 0);

    label(summary, "设备状态", 22, lv_color_hex(0x334238));
    snprintf(buf, sizeof(buf), "UHF %s  雷达 %s  红外 %s%s%s%s  WiFi %s  MQTT %s",
             ok_text(g_fsm.mod_uhf_ok), ok_text(g_fsm.mod_radar_ok),
             g_fsm.ir_ok[0] ? "上左 " : "",
             g_fsm.ir_ok[1] ? "上右 " : "",
             g_fsm.ir_ok[2] ? "下左 " : "",
             g_fsm.ir_ok[3] ? "下右 " : "",
             g_fsm.wifi_connected ? g_fsm.wifi_ssid : ok_text(g_fsm.mod_wifi_ok),
             g_fsm.mqtt_connected ? "已连接" : "离线");
    label(summary, buf, 17, lv_color_hex(0x42505A));
    snprintf(buf, sizeof(buf), "传感器: 光照=%s 温湿度=%s  书籍: 在架%d 取走%d 错放%d",
             g_fsm.bh1750_ok ? "正常" : "异常",
             g_fsm.sht30_ok ? "正常" : "异常",
             g_model.present_count, g_model.missing_count, g_model.misplaced_count);
    label(summary, buf, 17, lv_color_hex(0x42505A));

    lv_obj_t *power = lv_obj_create(summary);
    style_plain(power);
    lv_obj_set_size(power, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(power, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(power, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(power, 12, 0);
    make_small_button(power, "重启", lv_color_hex(0x8A5A2B), on_reboot_click);
    make_small_button(power, "关机", lv_color_hex(0x9B2D2D), on_poweroff_click);

    label(summary, "最近事件", 22, lv_color_hex(0x334238));
    if (log_n <= 0) {
        label(summary, "暂无书籍事件", 17, lv_color_hex(0x65706A));
        return;
    }
    for (int i = 0; i < log_n; ++i) {
        snprintf(buf, sizeof(buf), "%s  %s  %s  L%d %.1f-%.1fcm",
                 logs[i].timestamp[11] ? logs[i].timestamp + 11 : logs[i].timestamp,
                 action_cn(logs[i].action),
                 logs[i].title[0] ? logs[i].title : logs[i].epc,
                 logs[i].layer, logs[i].start_cm, logs[i].end_cm);
        label(summary, buf, 16, lv_color_hex(0x42505A));
    }
}

static void add_legend_item(lv_obj_t *parent, lv_color_t color, const char *text)
{
    lv_obj_t *item = lv_obj_create(parent);
    style_plain(item);
    lv_obj_set_size(item, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(item, 6, 0);

    lv_obj_t *dot = lv_obj_create(item);
    style_plain(dot);
    lv_obj_set_size(dot, 18, 10);
    lv_obj_set_style_radius(dot, 5, 0);
    lv_obj_set_style_bg_color(dot, color, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);

    label(item, text, 16, lv_color_hex(0x42505A));
}

static void format_axis_time(const char *src, char *dst, size_t dst_sz, int compact)
{
    int mon = 0, day = 0, hour = 0, min = 0;
    if (!dst || dst_sz == 0) return;
    if (src && sscanf(src, "%*d-%d-%d %d:%d", &mon, &day, &hour, &min) == 4) {
        if (compact)
            snprintf(dst, dst_sz, "%02d:%02d", hour, min);
        else
            snprintf(dst, dst_sz, "%02d-%02d %02d时", mon, day, hour);
    } else {
        snprintf(dst, dst_sz, "--");
    }
}

static void format_axis_time_t(time_t ts, char *dst, size_t dst_sz, int compact)
{
    struct tm *tmv;
    if (!dst || dst_sz == 0) return;
    tmv = localtime(&ts);
    if (!tmv) {
        snprintf(dst, dst_sz, "--");
        return;
    }
    if (compact)
        strftime(dst, dst_sz, "%H:%M", tmv);
    else
        strftime(dst, dst_sz, "%m-%d %H时", tmv);
}

static lv_obj_t *make_input(lv_obj_t *parent, const char *placeholder, const char *text, int w)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, w, 48);
    lv_obj_set_style_text_font(ta, font_main, 0);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    if (text) lv_textarea_set_text(ta, text);
    lv_obj_add_event_cb(ta, on_text_focus, LV_EVENT_ALL, NULL);
    return ta;
}

static void render_delete_books(lv_obj_t *parent)
{
    char buf[220];
    int shown = 0;

    lv_obj_t *list = lv_obj_create(parent);
    add_delete_list = list;
    style_plain(list);
    lv_obj_set_size(list, 550, 245);
    lv_obj_set_style_bg_color(list, lv_color_hex(0xFFF9EF), 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(list, 14, 0);
    lv_obj_set_style_pad_all(list, 10, 0);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    label(list, "删除书籍", 20, lv_color_hex(0x334238));
    for (int i = 0; i < g_model.count; ++i) {
        const ui_book_t *b = &g_model.books[i];
        if (!b->known) continue;

        lv_obj_t *row = lv_obj_create(list);
        style_plain(row);
        lv_obj_set_size(row, 520, 46);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        snprintf(buf, sizeof(buf), "%s  %s",
                 b->title[0] ? b->title : b->epc,
                 b->is_present ? "在架" : "取走");
        lv_obj_t *name = label(row, buf, 17, lv_color_hex(0x42505A));
        lv_obj_set_width(name, 380);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);

        lv_obj_t *btn = lv_btn_create(row);
        lv_obj_set_size(btn, 82, 38);
        lv_obj_set_style_radius(btn, 14, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x9B2D2D), 0);
        lv_obj_add_event_cb(btn, on_delete_book, LV_EVENT_CLICKED, (void *)b);
        lv_obj_t *txt = label(btn, "删除", 17, lv_color_white());
        lv_obj_center(txt);
        shown++;
    }

    if (!shown) {
        label(list, "还没有已登记书籍。", 18, lv_color_hex(0x65706A));
    }
}

static void render_add(void)
{
    char buf[160];
    lv_obj_t *box = card(content, lv_color_hex(0xF6F2E8), 24);
    lv_obj_set_size(box, 944, 382);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 8, 0);
    add_title_box = NULL;
    add_pages_box = NULL;
    add_form_box = box;

    label(box, "加入新书", 28, lv_color_hex(0x334238));

    if (!g_fsm.add_mode) {
        lv_obj_t *body = lv_obj_create(box);
        style_plain(body);
        lv_obj_set_size(body, 900, 265);
        lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(body, 20, 0);

        lv_obj_t *left = lv_obj_create(body);
        style_plain(left);
        lv_obj_set_size(left, 300, 245);
        lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
        lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(left, 14, 0);
        lv_obj_t *once = label(left, "一次只加一本。", 22, lv_color_hex(0x334238));
        lv_obj_set_width(once, 280);
        lv_label_set_long_mode(once, LV_LABEL_LONG_WRAP);
        lv_obj_t *hint = label(left, "读到后先保存或取消，再加下一本。", 18, lv_color_hex(0x61706A));
        lv_obj_set_width(hint, 280);
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        make_small_button(left, "开始扫描", lv_color_hex(0x2F6B4F), on_add_start);

        render_delete_books(body);
        return;
    }

    if (!g_fsm.register_pending) {
        label(box, "请只放一本新书，等待标签读取。", 22, lv_color_hex(0x6A5A39));
        make_small_button(box, "取消", lv_color_hex(0x8A4B35), on_add_cancel);
        return;
    }

    lv_obj_add_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_bottom(box, 170, 0);

    snprintf(buf, sizeof(buf), "已锁定新书 EPC: %.96s", g_fsm.pending_epc);
    label(box, buf, 16, lv_color_hex(0x43515A));
    snprintf(buf, sizeof(buf), "识别层: %s", ui_layer_name(g_fsm.pending_layer));
    label(box, buf, 18, lv_color_hex(0x43515A));
    add_title_box = make_input(box, "书名", g_add_title_draft, 520);
    lv_obj_add_event_cb(add_title_box, on_add_input_changed, LV_EVENT_VALUE_CHANGED, NULL);

    add_pages_box = make_input(box, "页数", g_add_pages_draft[0] ? g_add_pages_draft : "200", 220);
    lv_obj_add_event_cb(add_pages_box, on_add_input_changed, LV_EVENT_VALUE_CHANGED, NULL);
    label(box, "位置看红外，厚度按页数计算。", 18, lv_color_hex(0x61706A));

    lv_obj_t *btns = lv_obj_create(box);
    style_plain(btns);
    lv_obj_set_style_bg_opa(btns, LV_OPA_TRANSP, 0);
    lv_obj_set_size(btns, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 12, 0);
    make_small_button(btns, "保存", lv_color_hex(0x2F6B4F), on_add_save);
    make_small_button(btns, "取消", lv_color_hex(0x8A4B35), on_add_cancel);
}

static void render_trend(void)
{
    sensor_log_t logs[180];
    int n = 0;
    int max_points = 72;
    const char *range_label = "实时: 最近约5分钟, 原始点";
    if (g_db_ready) {
        if (g_trend_range == 0) {
            max_points = 60;
            n = db_sensor_recent(&g_db, max_points, logs, max_points);
        } else if (g_trend_range == 1) {
            max_points = 120;
            range_label = "1小时: 每30秒聚合";
            n = db_sensor_range(&g_db, 1, logs, max_points);
        } else if (g_trend_range == 2) {
            max_points = 144;
            range_label = "1天: 每10分钟聚合";
            n = db_sensor_range(&g_db, 24, logs, max_points);
        } else {
            max_points = 168;
            range_label = "1周: 每1小时聚合";
            n = db_sensor_range(&g_db, 168, logs, max_points);
        }
    }
    lv_obj_t *box = card(content, lv_color_hex(0xF4F7F4), 22);
    trend_scroll_box = box;
    lv_obj_set_size(box, 944, 382);
    lv_obj_add_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 8, 0);

    lv_obj_t *head = lv_obj_create(box);
    style_plain(head);
    lv_obj_set_size(head, 900, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    label(head, "近期状态", 28, lv_color_hex(0x334238));
    lv_obj_t *ranges = lv_obj_create(head);
    style_plain(ranges);
    lv_obj_set_size(ranges, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(ranges, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(ranges, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(ranges, 8, 0);
    make_trend_button(ranges, "实时", 0);
    make_trend_button(ranges, "1小时", 1);
    make_trend_button(ranges, "1天", 2);
    make_trend_button(ranges, "1周", 3);

    if (n <= 0) {
        label(box, "暂无传感器记录", 22, lv_color_hex(0x65706A));
        render_recent_summary(box);
        return;
    }

    lv_obj_t *legend = lv_obj_create(box);
    style_plain(legend);
    lv_obj_set_size(legend, 900, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(legend, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(legend, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(legend, 18, 0);
    add_legend_item(legend, lv_color_hex(0xB85B34), "温度 C");
    add_legend_item(legend, lv_color_hex(0x2D6F8A), "湿度 %");
    add_legend_item(legend, lv_color_hex(0xB79A24), "光照 lux/10");
    label(legend, range_label, 16, lv_color_hex(0x65706A));

    lv_obj_t *chart_row = lv_obj_create(box);
    style_plain(chart_row);
    lv_obj_set_size(chart_row, 900, 220);
    lv_obj_set_style_bg_opa(chart_row, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(chart_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(chart_row, 8, 0);

    lv_obj_t *axis = lv_obj_create(chart_row);
    style_plain(axis);
    lv_obj_set_size(axis, 42, 210);
    lv_obj_set_style_bg_opa(axis, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(axis, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(axis, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    label(axis, "100", 14, lv_color_hex(0x647078));
    label(axis, "75", 14, lv_color_hex(0x647078));
    label(axis, "50", 14, lv_color_hex(0x647078));
    label(axis, "25", 14, lv_color_hex(0x647078));
    label(axis, "0", 14, lv_color_hex(0x647078));

    lv_obj_t *chart = lv_chart_create(chart_row);
    lv_obj_set_size(chart, 850, 210);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, (uint32_t)n);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(chart, 5, 8);
    lv_chart_series_t *temp = lv_chart_add_series(chart, lv_color_hex(0xB85B34), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_series_t *hum = lv_chart_add_series(chart, lv_color_hex(0x2D6F8A), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_series_t *lux = lv_chart_add_series(chart, lv_color_hex(0xB79A24), LV_CHART_AXIS_PRIMARY_Y);
    for (int i = 0; i < n; ++i) {
        int src = n - 1 - i;
        lv_chart_set_next_value(chart, temp, (int32_t)logs[src].temperature);
        lv_chart_set_next_value(chart, hum, (int32_t)logs[src].humidity);
        lv_chart_set_next_value(chart, lux, (int32_t)(logs[src].lux / 10.0 > 100 ? 100 : logs[src].lux / 10.0));
    }

    lv_obj_t *x_axis = lv_obj_create(box);
    style_plain(x_axis);
    lv_obj_set_size(x_axis, 900, 22);
    lv_obj_set_style_bg_opa(x_axis, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_left(x_axis, 50, 0);
    lv_obj_set_style_pad_right(x_axis, 8, 0);
    lv_obj_set_flex_flow(x_axis, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(x_axis, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    {
        char left[24], mid[24], right[24];
        int compact = (g_trend_range <= 1);
        if (g_trend_range == 0) {
            time_t now = time(NULL);
            format_axis_time_t(now - 300, left, sizeof(left), 1);
            format_axis_time_t(now - 150, mid, sizeof(mid), 1);
            format_axis_time_t(now, right, sizeof(right), 1);
        } else {
            format_axis_time(logs[n - 1].timestamp, left, sizeof(left), compact);
            format_axis_time(logs[n / 2].timestamp, mid, sizeof(mid), compact);
            format_axis_time(logs[0].timestamp, right, sizeof(right), compact);
        }
        label(x_axis, left, 14, lv_color_hex(0x647078));
        label(x_axis, mid, 14, lv_color_hex(0x647078));
        label(x_axis, right, 14, lv_color_hex(0x647078));
    }

    const sensor_log_t *latest = &logs[0];
    char line[160];
    snprintf(line, sizeof(line), "当前 %.1fC  %.1f%%  %.0flx", latest->temperature,
             latest->humidity, latest->lux);
    label(box, line, 20, lv_color_hex(0x42505A));

    const char *advice = "环境适合保存书籍";
    if (latest->humidity > 65.0) advice = "湿度偏高，建议通风或除湿";
    else if (latest->humidity < 35.0) advice = "湿度偏低，纸张可能变脆";
    else if (latest->temperature > 32.0) advice = "温度偏高，建议降温";
    else if (latest->lux > 500.0) advice = "光照偏强，注意避免长期直晒";
    label(box, advice, 22, lv_color_hex(0x2F6B4F));
    if (!g_fsm.wifi_connected) {
        label(box, "未联网时时间轴使用板端本地时间，可能不准确但会继续滚动",
              16, lv_color_hex(0x7A6A5E));
    }
    render_recent_summary(box);
}

typedef struct {
    char ssid[64];
    int signal;
    char flags[96];
} wifi_ap_row_t;

static wifi_ap_row_t g_wifi_aps[16];
static int g_wifi_ap_count;
static char g_wifi_scan_status[32];
static char g_wifi_scan_error[96];
static time_t g_wifi_scan_updated_at;
static time_t g_wifi_scan_requested_at;

static void read_wifi_scan_file(void)
{
    FILE *fp = fopen(SB_WIFI_SCAN_FILE, "r");
    char line[192];
    g_wifi_ap_count = 0;
    if (!fp) return;
    while (fgets(line, sizeof(line), fp) && g_wifi_ap_count < 16) {
        char *p;
        char *ssid;
        char *sig;
        char *flags;
        if (strncmp(line, "updated_at=", 11) == 0) {
            g_wifi_scan_updated_at = (time_t)strtoul(line + 11, NULL, 10);
            continue;
        }
        if (strncmp(line, "status=", 7) == 0) {
            snprintf(g_wifi_scan_status, sizeof(g_wifi_scan_status), "%s", line + 7);
            g_wifi_scan_status[strcspn(g_wifi_scan_status, "\r\n")] = '\0';
            continue;
        }
        if (strncmp(line, "error=", 6) == 0 || strncmp(line, "message=", 8) == 0) {
            char *msg = strchr(line, '=');
            snprintf(g_wifi_scan_error, sizeof(g_wifi_scan_error), "%s", msg ? msg + 1 : "");
            g_wifi_scan_error[strcspn(g_wifi_scan_error, "\r\n")] = '\0';
            continue;
        }
        if (strncmp(line, "ap=", 3) != 0) continue;
        p = line + 3;
        ssid = strsep(&p, "|");
        sig = strsep(&p, "|");
        flags = p;
        if (!ssid || !ssid[0]) continue;
        snprintf(g_wifi_aps[g_wifi_ap_count].ssid, sizeof(g_wifi_aps[g_wifi_ap_count].ssid), "%s", ssid);
        g_wifi_aps[g_wifi_ap_count].signal = sig ? atoi(sig) : 0;
        if (flags) {
            flags[strcspn(flags, "\r\n")] = '\0';
            snprintf(g_wifi_aps[g_wifi_ap_count].flags, sizeof(g_wifi_aps[g_wifi_ap_count].flags), "%s", flags);
        }
        g_wifi_ap_count++;
    }
    fclose(fp);
}

static void on_wifi_scan(lv_event_t *e)
{
    (void)e;
    g_wifi_scan_requested_at = time(NULL);
    snprintf(g_wifi_scan_status, sizeof(g_wifi_scan_status), "requested");
    snprintf(g_wifi_scan_error, sizeof(g_wifi_scan_error), "已请求扫描");
    ui_cmd_net_scan();
    render_all();
}

static void on_wifi_disconnect(lv_event_t *e)
{
    (void)e;
    ui_cmd_net_disconnect();
    render_all();
}

static void on_wifi_input_changed(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    const char *text = lv_textarea_get_text(ta);
    if (ta == wifi_ssid_box) {
        snprintf(g_wifi_ssid_draft, sizeof(g_wifi_ssid_draft), "%s", text ? text : "");
        g_wifi_draft_ready = 1;
    } else if (ta == wifi_pass_box) {
        snprintf(g_wifi_pass_draft, sizeof(g_wifi_pass_draft), "%s", text ? text : "");
    }
}

static void on_wifi_connect(lv_event_t *e)
{
    const char *ssid;
    const char *pass;
    (void)e;
    if (!wifi_ssid_box) return;
    ssid = lv_textarea_get_text(wifi_ssid_box);
    pass = wifi_pass_box ? lv_textarea_get_text(wifi_pass_box) : "";
    ui_cmd_net_connect(ssid, pass);
    hide_keyboard();
    render_all();
}

static void on_wifi_ap_select(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= g_wifi_ap_count || !wifi_ssid_box) return;
    lv_textarea_set_text(wifi_ssid_box, g_wifi_aps[idx].ssid);
    snprintf(g_wifi_ssid_draft, sizeof(g_wifi_ssid_draft), "%s", g_wifi_aps[idx].ssid);
    g_wifi_draft_ready = 1;
    if (wifi_pass_box && keyboard) {
        lv_keyboard_set_textarea(keyboard, wifi_pass_box);
        lv_obj_clear_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

static void render_network(void)
{
    char buf[192];
    lv_obj_t *box = card(content, lv_color_hex(0xF2F6F8), 24);
    lv_obj_set_size(box, 944, 382);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(box, 14, 0);

    lv_obj_t *left = card(box, lv_color_hex(0xFFFFFF), 22);
    lv_obj_set_size(left, 360, 350);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 12, 0);
    label(left, "网络", 28, lv_color_hex(0x263E4A));

    if (!g_wifi_draft_ready) {
        snprintf(g_wifi_ssid_draft, sizeof(g_wifi_ssid_draft), "%s", g_fsm.wifi_ssid);
        g_wifi_draft_ready = 1;
    }
    wifi_ssid_box = make_input(left, "WiFi 名称", g_wifi_ssid_draft, 310);
    wifi_pass_box = make_input(left, "密码", g_wifi_pass_draft, 310);
    lv_textarea_set_password_mode(wifi_pass_box, true);
    lv_obj_add_event_cb(wifi_ssid_box, on_wifi_input_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(wifi_pass_box, on_wifi_input_changed, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *btns = lv_obj_create(left);
    style_plain(btns);
    lv_obj_set_style_bg_opa(btns, LV_OPA_TRANSP, 0);
    lv_obj_set_size(btns, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 8, 0);
    make_small_button(btns, "连接", lv_color_hex(0x2F6B4F), on_wifi_connect);
    make_small_button(btns, "断开", lv_color_hex(0x8A4B35), on_wifi_disconnect);

    if (!g_fsm.wifi_hw) {
        label(left, "未检测到 wlan0", 22, lv_color_hex(0x9F2F2F));
        label(left, "请检查 RTL8192CU 网卡、驱动和 USB 供电。", 18, lv_color_hex(0x6A747A));
    } else if (g_fsm.wifi_connected) {
        snprintf(buf, sizeof(buf), "已连接: %s", g_fsm.wifi_ssid[0] ? g_fsm.wifi_ssid : "未知");
        label(left, buf, 22, lv_color_hex(0x2F6B4F));
        snprintf(buf, sizeof(buf), "IP: %s   信号: %ddBm", g_fsm.wifi_ip[0] ? g_fsm.wifi_ip : "-", g_fsm.wifi_signal);
        label(left, buf, 18, lv_color_hex(0x42505A));
    } else {
        label(left, "未连接 WiFi", 22, lv_color_hex(0x8A5A24));
        label(left, g_fsm.wifi_error[0] ? g_fsm.wifi_error : "请选择网络并输入密码", 18, lv_color_hex(0x6A747A));
    }
    snprintf(buf, sizeof(buf), "MQTT: %s", g_fsm.mqtt_connected ? "已连接" : "离线");
    label(left, buf, 18, lv_color_hex(g_fsm.mqtt_connected ? 0x2F6B4F : 0x8A5A24));

    lv_obj_t *right = card(box, lv_color_hex(0xFFFFFF), 22);
    lv_obj_set_size(right, 540, 350);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 8, 0);
    lv_obj_t *head = lv_obj_create(right);
    style_plain(head);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_size(head, 500, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    label(head, "附近网络", 24, lv_color_hex(0x263E4A));
    make_small_button(head, "刷新", lv_color_hex(0x3C6F8A), on_wifi_scan);

    read_wifi_scan_file();
    if (g_wifi_scan_requested_at || g_wifi_scan_updated_at || g_wifi_scan_status[0]) {
        char scan_line[160];
        const char *status = "等待刷新";
        int request_pending = (g_wifi_scan_requested_at > 0 &&
                               g_wifi_scan_updated_at < g_wifi_scan_requested_at);
        if (request_pending || !strcmp(g_wifi_scan_status, "requested")) status = "已请求扫描";
        else if (!strcmp(g_wifi_scan_status, "scanning")) status = "正在扫描";
        else if (!strcmp(g_wifi_scan_status, "done")) status = "扫描完成";
        else if (!strcmp(g_wifi_scan_status, "error")) status = "扫描失败";
        if (g_wifi_scan_updated_at > 0 && !request_pending) {
            struct tm *tmv = localtime(&g_wifi_scan_updated_at);
            char tbuf[16] = "刚刚";
            if (g_wifi_scan_updated_at > 1704067200 && tmv)
                strftime(tbuf, sizeof(tbuf), "%H:%M:%S", tmv);
            snprintf(scan_line, sizeof(scan_line), "%s  %s  %d 个网络",
                     status, tbuf, g_wifi_ap_count);
        } else {
            snprintf(scan_line, sizeof(scan_line), "%s", g_wifi_scan_error[0] ? g_wifi_scan_error : status);
        }
        label(right, scan_line, 16, lv_color_hex(0x5A6972));
    }

    lv_obj_t *list = lv_obj_create(right);
    wifi_ap_list = list;
    lv_obj_set_size(list, 500, 238);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    if (g_wifi_ap_count == 0) {
        label(list, g_wifi_scan_error[0] ? g_wifi_scan_error : "点击刷新扫描 WiFi",
              20, lv_color_hex(0x6A747A));
    }
    for (int i = 0; i < g_wifi_ap_count; ++i) {
        lv_obj_t *row = lv_btn_create(list);
        lv_obj_set_size(row, 480, 44);
        lv_obj_set_style_radius(row, 16, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0xE8F0F2), 0);
        lv_obj_add_event_cb(row, on_wifi_ap_select, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        snprintf(buf, sizeof(buf), "%s     %ddBm", g_wifi_aps[i].ssid, g_wifi_aps[i].signal);
        lv_obj_t *txt = label(row, buf, 18, lv_color_hex(0x263E4A));
        lv_obj_align(txt, LV_ALIGN_LEFT_MID, 14, 0);
    }
}

static void render_all(void)
{
    int restore_trend_y = 0;
    int restore_wifi_y = 0;
    int restore_add_delete_y = 0;
    int should_restore_trend = 0;
    int should_restore_wifi = 0;
    int should_restore_add_delete = 0;

    if (g_page == PAGE_TREND && trend_scroll_box) {
        restore_trend_y = lv_obj_get_scroll_y(trend_scroll_box);
        should_restore_trend = 1;
    }
    if (g_page == PAGE_NETWORK && wifi_ap_list) {
        restore_wifi_y = lv_obj_get_scroll_y(wifi_ap_list);
        should_restore_wifi = 1;
    }
    if (g_page == PAGE_ADD && add_delete_list) {
        restore_add_delete_y = lv_obj_get_scroll_y(add_delete_list);
        should_restore_add_delete = 1;
    }

    if (keyboard_is_visible())
        hide_keyboard();

    if (g_db_ready) ui_model_load(&g_db, &g_model);
    ui_fsm_read(&g_fsm);
    set_screen_sleeping(g_fsm.state == FSM_SLEEP);
    update_top_and_footer();

    if (g_fsm.find_status == 4 || (g_fsm.state != FSM_FIND && g_fsm.find_status == 0 && !g_find_active)) {
        g_find_active = 0;
        g_find_epc[0] = '\0';
        g_find_completed_at = 0;
    }

    if (g_fsm.state == FSM_FIND && g_fsm.find_target_epc[0] && !g_find_epc[0]) {
        snprintf(g_find_epc, sizeof(g_find_epc), "%s", g_fsm.find_target_epc);
        g_find_active = 1;
        g_find_completed_at = 0;
    }

    if (g_find_active && (g_fsm.find_status == 2 || g_fsm.find_status == 3)) {
        time_t now = time(NULL);
        if (!g_find_completed_at) g_find_completed_at = now;
        if (now - g_find_completed_at >= 3) {
            g_find_active = 0;
            g_find_epc[0] = '\0';
            g_find_started_at = 0;
            g_find_completed_at = 0;
            if (g_page == PAGE_SEARCH) g_search_results_dirty = 1;
            else g_page = PAGE_SHELF;
        }
    } else if (g_fsm.find_status == 1) {
        g_find_completed_at = 0;
    }

    if (g_page == PAGE_SEARCH && search_box && search_results &&
        !g_find_active && g_fsm.state != FSM_FIND) {
        refresh_search_results_keep_scroll();
        return;
    }

    lv_obj_clean(content);
    memset(ir_dot, 0, sizeof(ir_dot));
    search_box = NULL;
    search_results = NULL;
    add_form_box = NULL;
    trend_scroll_box = NULL;
    wifi_ap_list = NULL;
    add_delete_list = NULL;

    if (g_find_active || g_fsm.state == FSM_FIND) render_find();
    else if (g_page == PAGE_SEARCH) render_search();
    else if (g_page == PAGE_ADD) render_add();
    else if (g_page == PAGE_TREND) render_trend();
    else if (g_page == PAGE_NETWORK) render_network();
    else render_shelf();

    if (should_restore_trend && g_page == PAGE_TREND && trend_scroll_box) {
        lv_obj_update_layout(trend_scroll_box);
        lv_obj_scroll_to_y(trend_scroll_box, restore_trend_y, LV_ANIM_OFF);
    }
    if (should_restore_wifi && g_page == PAGE_NETWORK && wifi_ap_list) {
        lv_obj_update_layout(wifi_ap_list);
        lv_obj_scroll_to_y(wifi_ap_list, restore_wifi_y, LV_ANIM_OFF);
    }
    if (should_restore_add_delete && g_page == PAGE_ADD && add_delete_list) {
        lv_obj_update_layout(add_delete_list);
        lv_obj_scroll_to_y(add_delete_list, restore_add_delete_y, LV_ANIM_OFF);
    }
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    if (keyboard_is_visible()) {
        ui_fsm_read(&g_fsm);
        set_screen_sleeping(0);
        update_top_and_footer();
        return;
    }
    if (g_shelf_edit_mode && g_page == PAGE_SHELF) {
        ui_fsm_read(&g_fsm);
        update_top_and_footer();
        return;
    }
    render_all();
}

static void ir_tick(lv_timer_t *timer)
{
    ui_fsm_snapshot_t next;
    (void)timer;
    if (g_page != PAGE_SHELF) return;
    if (ui_fsm_read(&next) != 0) return;
    for (int i = 0; i < 4; i++) {
        g_fsm.ir_ok[i] = next.ir_ok[i];
        g_fsm.ir_bitmap[i] = next.ir_bitmap[i];
    }
    update_ir_dots_visual(0);
    update_ir_dots_visual(1);
}

static lv_obj_t *make_nav_button(lv_obj_t *parent, const char *text, page_t page)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 92, 38);
    lv_obj_set_style_radius(btn, 18, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, on_nav, LV_EVENT_CLICKED, (void *)(uintptr_t)page);
    lv_obj_t *txt = label(btn, text, 18, lv_color_white());
    lv_obj_center(txt);
    return btn;
}

static void build_ui(void)
{
    font_main = g_window.font_songti ? g_window.font_songti : (lv_font_t *)LV_FONT_DEFAULT;

    screen = lv_obj_create(NULL);
    lv_obj_set_size(screen, 1024, 600);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0xE9DFD0), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    topbar = lv_obj_create(screen);
    lv_obj_set_size(topbar, 1024, 64);
    lv_obj_align(topbar, LV_ALIGN_TOP_MID, 0, 0);
    style_plain(topbar);
    lv_obj_set_style_bg_opa(topbar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(topbar, 28, 0);
    lv_obj_set_flex_flow(topbar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(topbar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    title_label = label(topbar, "智能书架", 28, lv_color_white());
    state_pill = label(topbar, "BOOT", 18, lv_color_white());
    lv_obj_set_style_bg_color(state_pill, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(state_pill, LV_OPA_20, 0);
    lv_obj_set_style_radius(state_pill, 14, 0);
    lv_obj_set_style_pad_hor(state_pill, 14, 0);
    lv_obj_set_style_pad_ver(state_pill, 5, 0);

    summary_row = lv_obj_create(screen);
    lv_obj_set_size(summary_row, 1024, 58);
    lv_obj_align(summary_row, LV_ALIGN_TOP_MID, 0, 70);
    style_plain(summary_row);
    lv_obj_set_style_bg_opa(summary_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_hor(summary_row, 34, 0);

    content = lv_obj_create(screen);
    lv_obj_set_size(content, 1024, 412);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 130);
    style_plain(content);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_hor(content, 40, 0);

    footer = lv_obj_create(screen);
    lv_obj_set_size(footer, 1024, 58);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, 0);
    style_plain(footer);
    lv_obj_set_style_bg_color(footer, lv_color_hex(0xFFF9EF), 0);
    lv_obj_set_style_bg_opa(footer, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(footer, 28, 0);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    sensor_label = label(footer, "等待传感器数据", 18, lv_color_hex(0x5C6970));
    lv_obj_t *nav = lv_obj_create(footer);
    style_plain(nav);
    lv_obj_set_size(nav, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(nav, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_column(nav, 10, 0);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    nav_btn[0] = make_nav_button(nav, "书架", PAGE_SHELF);
    nav_btn[1] = make_nav_button(nav, "查找", PAGE_SEARCH);
    nav_btn[2] = make_nav_button(nav, "加书", PAGE_ADD);
    nav_btn[3] = make_nav_button(nav, "近期", PAGE_TREND);
    nav_btn[4] = make_nav_button(nav, "网络", PAGE_NETWORK);
    time_label = label(footer, "--:--", 18, lv_color_hex(0x5C6970));

    sleep_overlay = lv_obj_create(screen);
    lv_obj_set_size(sleep_overlay, 1024, 600);
    lv_obj_align(sleep_overlay, LV_ALIGN_CENTER, 0, 0);
    style_plain(sleep_overlay);
    lv_obj_set_style_bg_color(sleep_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(sleep_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(sleep_overlay, LV_OBJ_FLAG_HIDDEN);

    keyboard = g_window.create_keyboard(lv_layer_top(), false);
    if (keyboard) {
        lv_obj_set_style_text_font(keyboard, font_main, 0);
        lv_obj_add_event_cb(keyboard, on_keyboard_event, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(keyboard, on_keyboard_event, LV_EVENT_CANCEL, NULL);
        lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
    }

    lv_scr_load(screen);
    set_nav_visual();
    render_all();
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    init_stringlist();
    init_Strings();
    init_window_create(argc, argv);

    if (db_init(&g_db, DB_PATH) == 0) {
        g_db_ready = 1;
        db_set_verbose(&g_db, 0);
    } else {
        fprintf(stderr, "[UI] failed to open %s\n", DB_PATH);
    }

    build_ui();
    lv_timer_create(tick, 500, NULL);
    lv_timer_create(ir_tick, 150, NULL);
    exec_event(NULL, NULL);

    if (g_db_ready) db_close(&g_db);
    return 0;
}
