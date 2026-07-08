/**
 * 书架灯带控制库 — 高层 API
 *
 * 基于 ws2812b 底层 SPI 驱动，封装书架物理布局映射。
 * 78 灯串联, SPI1 MOSI Pin38, 6.4MHz.
 *
 * 物理布局 (绕书架一圈):
 *   LED 0       = 上层状态指示灯 (左上角起点)
 *   LED 1~38    = 上层位置导引灯 (38灯, 左→右, ~1灯/cm)
 *   LED 39~76   = 下层位置导引灯 (38灯, 右→左, 反转!)
 *   LED 77      = 下层状态指示灯 (左下角终点)
 *
 * 位置映射:
 *   layer=0, 位置 cm → 亮 LED[cm + 1]
 *   layer=1, 位置 cm → 亮 LED[76 - cm]  (下层反转)
 *
 * 全局亮度:
 *   默认 10/255 (~4%), 通过 shelf_led_set_brightness() 调节。
 *   所有颜色设置自动乘以全局亮度缩放。
 *
 * 依赖: ws2812b.h / ws2812b.c
 */

#ifndef SHELF_LED_H
#define SHELF_LED_H

#include <stdint.h>

/* ================================================================
 * 书架常量
 * ================================================================ */

#define SHELF_LED_COUNT         78
#define SHELF_LAYER_MAX         2

/* 每层位置 LED 索引范围
 *
 * 物理走线: 左上(#0) → 上层左→右(#1~38) → 下层右→左(#39~76) → 左下(#77)
 * 下层位置灯是反的: #39=最右, #76=最左
 */
#define SHELF_LED_STATUS_0      0       /* 上层状态灯 (左上角) */
#define SHELF_LED_POS_0_FIRST   1       /* 上层位置灯起始 (最左) */
#define SHELF_LED_POS_0_LAST    38      /* 上层位置灯结束 (最右) */
#define SHELF_LED_STATUS_1      77      /* 下层状态灯 (左下角) */
#define SHELF_LED_POS_1_FIRST   39      /* 下层位置灯起始 (最右) */
#define SHELF_LED_POS_1_LAST    76      /* 下层位置灯结束 (最左) */

/* 每层位置灯数量 */
#define SHELF_POS_COUNT_0       38      /* 上层 38 灯 */
#define SHELF_POS_COUNT_1       38      /* 下层 38 灯 */

/* 书架物理尺寸 */
#define SHELF_WIDTH_CM          38      /* 每层宽度 (cm) */

/* 常用颜色预设 (RGB, 经全局亮度缩放后输出) */
#define SHELF_COLOR_OFF         0x000000
#define SHELF_COLOR_RED         0xFF0000
#define SHELF_COLOR_GREEN       0x00FF00
#define SHELF_COLOR_BLUE        0x0000FF
#define SHELF_COLOR_CYAN        0x00FFFF
#define SHELF_COLOR_MAGENTA     0xFF00FF
#define SHELF_COLOR_YELLOW      0xFFFF00
#define SHELF_COLOR_WHITE       0xFFFFFF
#define SHELF_COLOR_WARM_WHITE  0xFFE8C0
#define SHELF_COLOR_GOLD        0xFFC000
#define SHELF_COLOR_ORANGE      0xFF6000

/* 默认全局亮度 (0~255, ~4%) */
#define SHELF_DEFAULT_BRIGHTNESS 15

/* ================================================================
 * 公共 API — 初始化/关闭
 * ================================================================ */

/**
 * 初始化灯带
 *
 * @param spi_dev   SPI 设备路径, 如 "/dev/spidev1.0"
 * @param led_count 灯珠数量 (默认 78)
 * @return 0 成功, -1 失败
 */
int shelf_led_init(const char *spi_dev, int led_count);

/**
 * 关闭灯带 (全熄 + 释放资源)
 */
void shelf_led_close(void);

/* ================================================================
 * 公共 API — 全局亮度
 * ================================================================ */

/**
 * 设置全局亮度 (对所有后续颜色设置生效)
 *
 * @param b  亮度 0~255, 默认 SHELF_DEFAULT_BRIGHTNESS (10)
 *
 * 内部: 每次设置颜色时乘以 b/255 缩放。
 * 已设置的颜色不变，需重新设置或调用 shelf_led_show 前重新设置场景。
 */
void shelf_led_set_brightness(uint8_t b);

/**
 * 获取当前全局亮度
 */
uint8_t shelf_led_get_brightness(void);

/* ================================================================
 * 公共 API — 灯珠控制 (颜色值会自动乘以全局亮度)
 * ================================================================ */

/**
 * 设置单颗灯珠颜色 (不立即生效，需调用 shelf_led_show)
 *
 * @param index  物理灯珠索引 (0~77, 对应绕书架顺序)
 * @param r      红色 (0~255, 原值, 自动乘亮度)
 * @param g      绿色 (0~255)
 * @param b      蓝色 (0~255)
 */
void shelf_led_set(int index, uint8_t r, uint8_t g, uint8_t b);

/**
 * 全部灯珠填充同一颜色
 */
void shelf_led_fill(uint8_t r, uint8_t g, uint8_t b);

/**
 * 全部灯珠熄灭
 */
void shelf_led_clear(void);

/**
 * 将缓冲区数据发送到灯带 (含 >50µs RESET)
 */
void shelf_led_show(void);

/* ================================================================
 * 公共 API — 层状态指示 (颜色自动乘亮度)
 * ================================================================ */

/**
 * 设置层状态指示灯
 *
 * @param layer  0=上层(LED 0), 1=下层(LED 38)
 * @param r/g/b  颜色 (原值, 自动乘亮度)
 */
void shelf_led_status(int layer, uint8_t r, uint8_t g, uint8_t b);

/**
 * 熄灭层状态指示灯
 */
void shelf_led_clear_status(int layer);

/**
 * 同时设置两层状态指示灯
 */
void shelf_led_status_both(uint8_t r0, uint8_t g0, uint8_t b0,
                           uint8_t r1, uint8_t g1, uint8_t b1);

/* ================================================================
 * 公共 API — 位置导引 (颜色自动乘亮度)
 * ================================================================ */

/**
 * 指定书籍位置，点亮对应区间灯珠
 *
 * @param layer     0=上层, 1=下层
 * @param start_cm  书籍起始位置 (cm, 0~37)
 * @param end_cm    书籍结束位置 (cm, 0~37)
 * @param r/g/b     颜色 (原值, 自动乘亮度)
 *
 * 示例: 书在上层 5~8cm → 亮 LED[6]~LED[9]
 */
void shelf_led_position(int layer,
                        int start_cm, int end_cm,
                        uint8_t r, uint8_t g, uint8_t b);

/**
 * 点亮某层全部位置灯
 */
void shelf_led_position_all(int layer, uint8_t r, uint8_t g, uint8_t b);

/**
 * 熄灭某层全部位置灯
 */
void shelf_led_clear_position(int layer);

/* ================================================================
 * 公共 API — 组合场景 (内部使用 255 满值, 经亮度缩放输出)
 * ================================================================ */

/** 全部熄灭 */
void shelf_led_all_off(void);

/** 休眠 — 全熄 */
void shelf_led_scene_sleep(void);

/** 感知 — 状态灯绿 + 位置灯暖白 */
void shelf_led_scene_aware(void);

/** 故障 — 状态灯红 + 全灯带红 (快闪由调用方循环) */
void shelf_led_scene_error(void);

/** 书被取出 — 状态灯蓝 + 目标区间蓝 */
void shelf_led_scene_borrowed(int layer, int start_cm, int end_cm);

/** 书被放回 — 状态灯绿 + 目标区间绿 */
void shelf_led_scene_returned(int layer, int start_cm, int end_cm);

/** 错放警告 — 状态灯红 + 目标区间红 */
void shelf_led_scene_misplaced(int layer, int start_cm, int end_cm);

/** 查找模式 — 状态灯金 + 目标区间金 */
void shelf_led_scene_find(int layer, int start_cm, int end_cm);

#endif /* SHELF_LED_H */
