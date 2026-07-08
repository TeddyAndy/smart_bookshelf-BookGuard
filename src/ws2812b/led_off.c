/** 关灯小工具 — 关掉所有 WS2812B 灯珠 */
#include "shelf_led.h"
#include <stdio.h>

int main(void) {
    if (shelf_led_init("/dev/spidev1.0", 78) < 0) {
        fprintf(stderr, "led_off: SPI init failed\n");
        return 1;
    }
    shelf_led_all_off();
    shelf_led_show();
    shelf_led_close();
    return 0;
}
