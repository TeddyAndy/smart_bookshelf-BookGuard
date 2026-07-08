/**
 * WS2812B 可寻址 RGB LED 灯带驱动 — SPI 后端实现
 *
 * 通过 /dev/spidev1.0 (SPI1) 硬件时序驱动 WS2812B
 *
 * 原理:
 *   SPI 时钟 ~6.4MHz, 1 字节 (8 个 SPI bit) = 1 个 WS2812B bit (1.25µs)
 *   WS2812B bit 0: SPI 0xC0 (11000000) — 312ns 高, 937ns 低
 *   WS2812B bit 1: SPI 0xF8 (11111000) — 781ns 高, 469ns 低
 *   颜色顺序: GRB (绿→红→蓝), MSB 优先
 *   RESET: >50µs 低电平 (SPI MODE0 空闲低, usleep 100µs)
 *
 * 编译: arm-none-linux-gnueabihf-gcc -Wall -O2 -g -c ws2812b.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

#include "ws2812b.h"

/* ================================================================
 * WS2812B 协议常量
 * ================================================================ */

/* SPI 时钟 (Hz) — 使 1 字节 = 1 WS2812B bit */
#define SPI_SPEED_HZ         6400000     /* 6.4 MHz */

/* WS2812B bit → SPI 字节编码 */
#define WS2812B_BIT_0        0xC0        /* 11000000: 312ns 高, 937ns 低 */
#define WS2812B_BIT_1        0xF8        /* 11111000: 781ns 高, 469ns 低 */

/* WS2812B 协议要求 */
#define WS2812B_RESET_US     300         /* >50µs, 取 300µs 抗噪声 */

/* 每颗灯珠 24 bit (GRB × 8), 每 bit 1 个 SPI 字节 */
#define SPI_BYTES_PER_LED    24

/* ================================================================
 * 全局状态
 * ================================================================ */

static int   g_spi_fd   = -1;           /* SPI 文件描述符 */
static int   g_led_count = 0;           /* 灯珠数量 */
static uint8_t *g_colors = NULL;        /* RGB 颜色缓冲: [led][3] (GRB 顺序) */
static uint8_t *g_spi_buf = NULL;       /* SPI 字节流缓冲 */
static int   g_spi_buf_len = 0;         /* SPI 字节流长度 */

/* ================================================================
 * 内部函数
 * ================================================================ */

/**
 * 将 1 字节颜色值转换为 8 字节 SPI 脉冲流
 */
static void encode_byte(uint8_t byte, uint8_t *spi_out)
{
    int bit;

    for (bit = 7; bit >= 0; bit--) {
        *spi_out++ = (byte & (1 << bit)) ? WS2812B_BIT_1 : WS2812B_BIT_0;
    }
}

/**
 * 将一颗灯珠的 GRB 颜色转换为 24 字节 SPI 脉冲流
 */
static void encode_led(const uint8_t *grb, uint8_t *spi_out)
{
    /* 颜色顺序: GRB (Green, Red, Blue) */
    encode_byte(grb[0], spi_out + 0);   /* Green */
    encode_byte(grb[1], spi_out + 8);   /* Red   */
    encode_byte(grb[2], spi_out + 16);  /* Blue  */
}

/**
 * 重新编码全部灯珠到 SPI 缓冲区
 */
static void encode_all(void)
{
    int i;

    for (i = 0; i < g_led_count; i++) {
        encode_led(&g_colors[i * 3], &g_spi_buf[i * SPI_BYTES_PER_LED]);
    }
}

/* ================================================================
 * 公共 API 实现
 * ================================================================ */

int ws2812b_init(const char *spi_dev, int led_count)
{
    int fd;
    uint8_t mode;
    uint8_t bits;
    uint32_t speed;
    int buf_bytes;

    if (led_count < 1 || led_count > 1024) {
        fprintf(stderr, "ws2812b_init: 灯珠数量无效 (%d), 范围 1~1024\n",
                led_count);
        return -1;
    }

    /* ---- 打开或复用 SPI 设备 ---- */
    if (g_spi_fd >= 0) {
        /* 复用上次未关闭的 fd (MOSI 保持低电平策略) */
        fd = g_spi_fd;
    } else {
        fd = open(spi_dev, O_RDWR);
        if (fd < 0) {
            perror("ws2812b: open SPI");
            fprintf(stderr, "  设备: %s\n", spi_dev);
            fprintf(stderr, "  检查: 1) ls %s  2) 设备树是否启用\n", spi_dev);
            return -1;
        }
    }

    /* ---- 配置 SPI 模式 (仅首次) ---- */
    if (g_spi_fd < 0) {
    mode = SPI_MODE_0;          /* CPOL=0, CPHA=0 — 空闲低, 首沿采样 */
    if (ioctl(fd, SPI_IOC_WR_MODE, &mode) < 0) {
        perror("ws2812b: SPI_IOC_WR_MODE");
        close(fd);
        return -1;
    }

    bits = 8;
    if (ioctl(fd, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0) {
        perror("ws2812b: SPI_IOC_WR_BITS_PER_WORD");
        close(fd);
        return -1;
    }

    speed = SPI_SPEED_HZ;
    if (ioctl(fd, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) {
        perror("ws2812b: SPI_IOC_WR_MAX_SPEED_HZ");
        close(fd);
        return -1;
    }

        /* 读取实际速度 (内核可能取近似值) */
        if (ioctl(fd, SPI_IOC_RD_MAX_SPEED_HZ, &speed) == 0) {
            /* 允许范围: 5.7MHz ~ 6.9MHz (WS2812B 容差内) */
            if (speed < 5700000 || speed > 6900000) {
                fprintf(stderr,
                        "ws2812b: SPI 实际频率 %u Hz 超出 WS2812B 容差范围 "
                        "(5.7~6.9 MHz), 可能导致颜色异常\n", speed);
            }
        }
    } /* g_spi_fd < 0: 首次初始化 SPI 参数 */

    /* ---- 分配颜色缓冲 ---- */
    buf_bytes = led_count * 3;                    /* RGB 三字节/灯 */
    g_colors = (uint8_t *)malloc(buf_bytes);
    if (!g_colors) {
        perror("ws2812b: malloc colors");
        close(fd);
        return -1;
    }
    memset(g_colors, 0, buf_bytes);

    /* ---- 分配 SPI 缓冲并编码全零 ---- */
    g_spi_buf_len = led_count * SPI_BYTES_PER_LED;
    g_spi_buf = (uint8_t *)malloc(g_spi_buf_len);
    if (!g_spi_buf) {
        perror("ws2812b: malloc spi_buf");
        free(g_colors);
        g_colors = NULL;
        close(fd);
        return -1;
    }
    /* 正确编码全零 (g_colors 已 memset 为 0) */
    encode_all();

    g_spi_fd = fd;
    g_led_count = led_count;

    /* 首次发送全黑帧确保灯带初始状态 */
    ws2812b_show();

    return 0;
}

void ws2812b_set_led(int index, uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t *grb;

    if (index < 0 || index >= g_led_count) return;

    /* 颜色缓冲按 GRB 顺序存储 */
    grb = &g_colors[index * 3];
    grb[0] = g;
    grb[1] = r;
    grb[2] = b;

    /* 同步更新 SPI 缓冲 */
    encode_led(grb, &g_spi_buf[index * SPI_BYTES_PER_LED]);
}

void ws2812b_fill(uint8_t r, uint8_t g, uint8_t b)
{
    int i;

    for (i = 0; i < g_led_count; i++) {
        uint8_t *grb = &g_colors[i * 3];
        grb[0] = g;
        grb[1] = r;
        grb[2] = b;
    }

    /* 批量重新编码 */
    encode_all();
}

void ws2812b_clear(void)
{
    /* 注意: 不能直接 memset(g_spi_buf, 0)！
     * WS2812B 协议要求每个 bit 都有高脉冲: bit 0 = 0xC0, bit 1 = 0xF8
     * 全零字节没有高脉冲，灯带会误读导致上一帧颜色残留 */
    memset(g_colors, 0, g_led_count * 3);
    encode_all();  /* 正确编码: 全零颜色 → 24×0xC0 每灯 */
}

void ws2812b_show(void)
{
    struct spi_ioc_transfer tr;

    if (g_spi_fd < 0 || !g_spi_buf || g_spi_buf_len <= 0) return;

    /*
     * 单次 SPI 传输全部灯珠数据
     *
     * tx_buf 指向预编码的 SPI 字节流 (60灯 = 1440 字节)
     * 内核 SPI 驱动 + PL330 DMA 处理实际传输
     * 传输时间: 1440 × 1.25µs = 1.8ms
     */
    memset(&tr, 0, sizeof(tr));
    tr.tx_buf = (unsigned long)g_spi_buf;
    tr.rx_buf = 0;                              /* 不需要接收 */
    tr.len = g_spi_buf_len;
    tr.speed_hz = SPI_SPEED_HZ;
    tr.delay_usecs = 0;
    tr.bits_per_word = 8;
    tr.cs_change = 0;                           /* 不要切换 CS */

    if (ioctl(g_spi_fd, SPI_IOC_MESSAGE(1), &tr) < 0) {
        perror("ws2812b: SPI_IOC_MESSAGE");
        return;
    }

    /*
     * WS2812B RESET 脉冲: >50µs 低电平
     *
     * SPI MODE0 传输结束后 MOSI 空闲低, 自动满足
     * usleep(1) 确保至少一个调度周期 (>50µs 在 Linux 上)
     */
    usleep(WS2812B_RESET_US);
}

void ws2812b_close(void)
{
    /* 先熄灭所有灯珠 */
    if (g_spi_fd >= 0 && g_spi_buf && g_led_count > 0) {
        ws2812b_clear();
        ws2812b_show();

        /*
         * 额外发一个全零字节，确保 MOSI 终态为低电平。
         * WS2812B 空闲时要求 DIN < 0.3V，否则噪声会触发随机亮灯。
         * SPI MODE0 传输结束后 MOSI 保持最后一 bit 电平；
         * 发 0x00 (8 bit 全零) → MOSI 稳定在低。
         */
        {
            uint8_t zero = 0x00;
            struct spi_ioc_transfer tr;
            memset(&tr, 0, sizeof(tr));
            tr.tx_buf = (unsigned long)&zero;
            tr.len = 1;
            tr.speed_hz = SPI_SPEED_HZ;
            tr.bits_per_word = 8;
            ioctl(g_spi_fd, SPI_IOC_MESSAGE(1), &tr);
        }
    }

    /* 释放内存缓冲 */
    if (g_spi_buf) {
        free(g_spi_buf);
        g_spi_buf = NULL;
    }
    if (g_colors) {
        free(g_colors);
        g_colors = NULL;
    }

    /*
     * 不要关闭 SPI fd！
     * 保持 fd 打开 → 内核维持 SPI 控制器驱动 MOSI 为低。
     * 关闭 fd 会导致引脚回到 GPIO 输入态 (高阻浮空)，
     * 浮空引脚像天线一样拾取噪声 → 灯带随机闪烁。
     *
     * SPI fd 由操作系统在进程退出时自动回收。
     */
    g_led_count = 0;
    g_spi_buf_len = 0;
}

/**
 * 强制关闭 SPI 描述符 (仅在进程退出前调用)
 *
 * 正常使用 ws2812b_close() 即可 (保持 MOSI 拉低)。
 * 此函数在 close() 后释放 fd，进程退出时 OS 也会自动回收。
 */
void ws2812b_shutdown(void)
{
    if (g_spi_fd >= 0) {
        close(g_spi_fd);
        g_spi_fd = -1;
    }
}
