#ifndef SB_IPC_H
#define SB_IPC_H

#ifdef __cplusplus
extern "C" {
#endif

#define SB_FSM_STATE_FILE "/tmp/fsm_state"
#define SB_CMD_FILE       "/tmp/sb_cmd"
#define SB_WIFI_SCAN_FILE "/tmp/wifi_scan"
#define SB_LED_BRIGHTNESS_FILE "/tmp/bookshelf_led_brightness"

int sb_write_text_atomic(const char *path, const char *text);

#ifdef __cplusplus
}
#endif

#endif
