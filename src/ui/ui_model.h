#ifndef UI_MODEL_H
#define UI_MODEL_H

#include "../db/db.h"
#include "../fsm/fsm.h"
#include <stdint.h>
#include <time.h>

#define UI_MAX_BOOKS 96

typedef struct {
    char epc[64];
    char title[128];
    int expected_layer;
    double expected_start;
    double expected_end;
    int layer;
    double start_cm;
    double end_cm;
    int rssi;
    int is_present;
    int known;
    int misplaced;
    char last_seen[32];
} ui_book_t;

typedef struct {
    ui_book_t books[UI_MAX_BOOKS];
    int count;
    int present_count;
    int missing_count;
    int misplaced_count;
    int unknown_count;
    sensor_log_t sensor;
    int has_sensor;
    time_t loaded_at;
} ui_model_t;

typedef struct {
    fsm_state_t state;
    int fault_dev;
    int find_count;
    int find_status;
    char find_target_epc[64];
    char find_target_title[128];
    int add_mode;
    int register_pending;
    char pending_epc[64];
    int pending_layer;
    int wifi_hw;
    int wifi_connected;
    char wifi_ssid[64];
    char wifi_ip[32];
    int wifi_signal;
    char wifi_error[128];
    int mod_led_ok;
    int mod_radar_ok;
    int mod_uhf_ok;
    int mod_wifi_ok;
    int mod_sensor_ok;
    int mod_mqtt_ok;
    int mod_screen_ok;
    int bh1750_ok;
    int sht30_ok;
    int mqtt_connected;
    int shelf_issue;
    char shelf_issue_text[256];
    int ir_ok[4];
    uint16_t ir_bitmap[4];
    time_t mtime;
    int exists;
} ui_fsm_snapshot_t;

int ui_model_load(db_ctx_t *db, ui_model_t *model);
const ui_book_t *ui_model_find_epc(const ui_model_t *model, const char *epc);
int ui_book_match(const ui_book_t *book, const char *query);
const char *ui_layer_name(int layer);
const char *ui_rssi_text(int rssi);

int ui_fsm_read(ui_fsm_snapshot_t *out);
int ui_cmd_find(const char *epc);
int ui_cmd_cancel_find(void);
int ui_cmd_clear_issue(void);
int ui_cmd_add_start(void);
int ui_cmd_add_cancel(void);
int ui_cmd_register_book(int layer, int pages, const char *title);
int ui_cmd_delete_book(const char *epc);
int ui_cmd_net_scan(void);
int ui_cmd_net_connect(const char *ssid, const char *password);
int ui_cmd_net_disconnect(void);
int ui_cmd_fault_retry(void);
int ui_cmd_system_reboot(void);
int ui_cmd_system_poweroff(void);

#endif
