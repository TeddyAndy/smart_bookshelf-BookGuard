/**
 * 书架灯带控制库 — 实现
 *
 * 封装 ws2812b 底层, 提供书架语义的上层 API。
 * 所有颜色设置自动乘以全局亮度缩放 (默认 10/255)。
 * 非线程安全，调用方自行加锁。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "shelf_led.h"
#include "ws2812b.h"

/* ================================================================
 * 全局状态
 * ================================================================ */

static uint8_t g_brightness = SHELF_DEFAULT_BRIGHTNESS;  /* 全局亮度 0~255 */

/* ================================================================
 * 内部辅助
 * ================================================================ */

/** 按全局亮度缩放颜色分量 */
static inline uint8_t dim(uint8_t color)
{
    return (uint8_t)(((uint16_t)color * g_brightness) / 255);
}

/** 限定位置参数到有效范围 */
static int clamp_pos(int cm)
{
    if (cm < 0)  return 0;
    if (cm >= SHELF_WIDTH_CM) return SHELF_WIDTH_CM - 1;
    return cm;
}

/** 将层 + cm 位置转换为 LED 索引
 *
 *  上层: cm=0(最左)→LED#1, cm=37(最右)→LED#38
 *  下层: cm=0(最左)→LED#76, cm=37(最右)→LED#39 (反转!)
 */
static int pos_to_led(int layer, int cm)
{
    if (layer == 0) {
        return SHELF_LED_POS_0_FIRST + cm;
    } else {
        /* 下层反转: 最左 cm=0 对应 LED#76 */
        return SHELF_LED_POS_1_LAST - cm;
    }
}

/* ================================================================
 * 公共 API — 初始化/关闭
 * ================================================================ */

int shelf_led_init(const char *spi_dev, int led_count)
{
    g_brightness = SHELF_DEFAULT_BRIGHTNESS;

    if (led_count < SHELF_LED_COUNT) {
        fprintf(stderr, "shelf_led_init: led_count=%d < %d (书架最少需要 %d 灯)\n",
                led_count, SHELF_LED_COUNT, SHELF_LED_COUNT);
    }

    return ws2812b_init(spi_dev, led_count);
}

void shelf_led_close(void)
{
    ws2812b_close();
}

/* ================================================================
 * 公共 API — 全局亮度
 * ================================================================ */

void shelf_led_set_brightness(uint8_t b)
{
    g_brightness = b;
}

uint8_t shelf_led_get_brightness(void)
{
    return g_brightness;
}

/* ================================================================
 * 公共 API — 灯珠控制
 * ================================================================ */

void shelf_led_set(int index, uint8_t r, uint8_t g, uint8_t b)
{
    ws2812b_set_led(index, dim(r), dim(g), dim(b));
}

void shelf_led_fill(uint8_t r, uint8_t g, uint8_t b)
{
    ws2812b_fill(dim(r), dim(g), dim(b));
}

void shelf_led_clear(void)
{
    ws2812b_clear();
}

void shelf_led_show(void)
{
    ws2812b_show();
}

/* ================================================================
 * 公共 API — 层状态指示
 * ================================================================ */

void shelf_led_status(int layer, uint8_t r, uint8_t g, uint8_t b)
{
    int idx;

    if (layer == 0) {
        idx = SHELF_LED_STATUS_0;
    } else if (layer == 1) {
        idx = SHELF_LED_STATUS_1;
    } else {
        return;
    }

    ws2812b_set_led(idx, dim(r), dim(g), dim(b));
}

void shelf_led_clear_status(int layer)
{
    shelf_led_status(layer, 0, 0, 0);
}

void shelf_led_status_both(uint8_t r0, uint8_t g0, uint8_t b0,
                           uint8_t r1, uint8_t g1, uint8_t b1)
{
    shelf_led_status(0, r0, g0, b0);
    shelf_led_status(1, r1, g1, b1);
}

/* ================================================================
 * 公共 API — 位置导引
 * ================================================================ */

void shelf_led_position(int layer,
                        int start_cm, int end_cm,
                        uint8_t r, uint8_t g, uint8_t b)
{
    int s, e, i;
    uint8_t rd, gd, bd;

    s = clamp_pos(start_cm);
    e = clamp_pos(end_cm);

    if (s > e) {
        int tmp = s;
        s = e;
        e = tmp;
    }

    rd = dim(r);
    gd = dim(g);
    bd = dim(b);

    for (i = s; i <= e; i++) {
        ws2812b_set_led(pos_to_led(layer, i), rd, gd, bd);
    }
}

void shelf_led_position_all(int layer, uint8_t r, uint8_t g, uint8_t b)
{
    int first, last, i;
    uint8_t rd, gd, bd;

    if (layer == 0) {
        first = SHELF_LED_POS_0_FIRST;
        last  = SHELF_LED_POS_0_LAST;
    } else if (layer == 1) {
        first = SHELF_LED_POS_1_FIRST;
        last  = SHELF_LED_POS_1_LAST;
    } else {
        return;
    }

    rd = dim(r);
    gd = dim(g);
    bd = dim(b);

    for (i = first; i <= last; i++) {
        ws2812b_set_led(i, rd, gd, bd);
    }
}

void shelf_led_clear_position(int layer)
{
    shelf_led_position_all(layer, 0, 0, 0);
}

/* ================================================================
 * 公共 API — 组合场景 (使用 255 满值语义, 靠亮度缩放)
 * ================================================================ */

/* 休眠: 全熄 */
void shelf_led_scene_sleep(void)
{
    ws2812b_clear();
}

/* 感知: 仅状态灯绿, 位置灯熄灭 (不晃眼) */
void shelf_led_scene_aware(void)
{
    ws2812b_clear();
    shelf_led_status(0, 0, 255, 0);
    shelf_led_status(1, 0, 255, 0);
}

/* 故障: 全红 */
void shelf_led_scene_error(void)
{
    ws2812b_clear();
    shelf_led_status(0, 255, 0, 0);
    shelf_led_status(1, 255, 0, 0);
    shelf_led_position_all(0, 255, 0, 0);
    shelf_led_position_all(1, 255, 0, 0);
}

/* 书被取出: 状态灯蓝 + 目标区间蓝 + 另一层暗蓝 */
void shelf_led_scene_borrowed(int layer, int start_cm, int end_cm)
{
    ws2812b_clear();

    shelf_led_status(layer, 0, 0, 255);

    shelf_led_position(layer, start_cm, end_cm, 0, 0, 255);

    /* 另一层: 暗蓝 (用较小值) */
    if (layer == 0) {
        shelf_led_position_all(1, 0, 0, 64);
    } else {
        shelf_led_position_all(0, 0, 0, 64);
    }
}

/* 书被放回: 状态灯绿 + 目标区间绿 + 另一层暗绿 */
void shelf_led_scene_returned(int layer, int start_cm, int end_cm)
{
    ws2812b_clear();

    shelf_led_status(layer, 0, 255, 0);

    shelf_led_position(layer, start_cm, end_cm, 0, 255, 0);

    if (layer == 0) {
        shelf_led_position_all(1, 0, 64, 0);
    } else {
        shelf_led_position_all(0, 0, 64, 0);
    }
}

/* 错放: 状态灯红 + 目标区间红 + 另一层暗红 */
void shelf_led_scene_misplaced(int layer, int start_cm, int end_cm)
{
    ws2812b_clear();

    shelf_led_status(layer, 255, 0, 0);
    shelf_led_position(layer, start_cm, end_cm, 255, 0, 0);

    if (layer == 0) {
        shelf_led_status(1, 128, 0, 0);
        shelf_led_position_all(1, 64, 0, 0);
    } else {
        shelf_led_status(0, 128, 0, 0);
        shelf_led_position_all(0, 64, 0, 0);
    }
}

/* 查找: 状态灯金 + 目标区间金 + 另一层暗金 */
void shelf_led_scene_find(int layer, int start_cm, int end_cm)
{
    ws2812b_clear();

    shelf_led_status(layer, 255, 192, 0);
    shelf_led_position(layer, start_cm, end_cm, 255, 192, 0);

    if (layer == 0) {
        shelf_led_position_all(1, 64, 48, 0);
    } else {
        shelf_led_position_all(0, 64, 48, 0);
    }
}

/* 全部熄灭 */
void shelf_led_all_off(void)
{
    ws2812b_clear();
    ws2812b_show();
}
