/**
 * WS2812B 可寻址 RGB LED 灯带驱动 — 公共接口
 *
 * 通过 SPI 硬件时序驱动 WS2812B 灯带（无 CS 引脚，单线协议）
 * 颜色顺序: GRB, MSB 优先
 *
 * 接线 (SPI1):
 *   P9 Pin 38 (SPI1_MOSI) → WS2812B DIN (经 3.3V→5V 电平转换)
 *   P9 Pin 2/4 (5V)       → WS2812B VCC
 *   P9 Pin 6 (GND)         → WS2812B GND
 *
 * 参考: Worldsemi WS2812B 数据手册
 */

#ifndef WS2812B_H
#define WS2812B_H

#include <stdint.h>

/* ================================================================
 * 公共 API
 * ================================================================ */

/**
 * 初始化灯带驱动
 *
 * @param spi_dev   SPI 设备路径，如 "/dev/spidev1.0"
 * @param led_count 灯珠数量 (本项目 1m 60灯)
 * @return 0 成功, -1 失败
 */
int ws2812b_init(const char *spi_dev, int led_count);

/**
 * 设置单颗灯珠颜色 (不立即生效，需调用 ws2812b_show)
 *
 * @param index  灯珠索引 (0 ~ led_count-1)
 * @param r      红色分量 (0~255)
 * @param g      绿色分量 (0~255)
 * @param b      蓝色分量 (0~255)
 */
void ws2812b_set_led(int index, uint8_t r, uint8_t g, uint8_t b);

/**
 * 全部灯珠填充同一颜色
 */
void ws2812b_fill(uint8_t r, uint8_t g, uint8_t b);

/**
 * 全部灯珠熄灭
 */
void ws2812b_clear(void);

/**
 * 将缓冲区数据发送到灯带 (SPI 硬件定时, 约 1.8ms @ 60灯)
 *
 * 包含 WS2812B 协议要求的 >50µs RESET 脉冲
 */
void ws2812b_show(void);

/**
 * 熄灭所有灯珠并释放内存缓冲
 *
 * MOSI 引脚保持低电平 (不关闭 SPI fd)，防止浮空噪声导致灯带误闪。
 * 如需释放 SPI fd，调用 ws2812b_shutdown()。
 */
void ws2812b_close(void);

/**
 * 强制关闭 SPI 描述符 (仅进程退出前调用)
 */
void ws2812b_shutdown(void);

#endif /* WS2812B_H */
