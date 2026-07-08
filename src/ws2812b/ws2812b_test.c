/**
 * WS2812B 灯带测试/演示程序
 *
 * 依次展示多种灯光模式，验证驱动功能
 *
 * 编译: arm-none-linux-gnueabihf-gcc -Wall -O2 -g -o ws2812b_test ws2812b_test.c ws2812b.c
 * 运行: ./ws2812b_test [-n 灯数] [-d SPI设备] [-b 亮度]
 *
 * 接线:
 *   P9 Pin 38 (SPI1_MOSI) → WS2812B DIN
 *   P9 Pin 2/4 (5V)       → WS2812B VCC
 *   P9 Pin 6 (GND)         → WS2812B GND
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

#include "ws2812b.h"

/* ================================================================
 * 默认参数
 * ================================================================ */

#define DEFAULT_SPI_DEV     "/dev/spidev1.0"
#define DEFAULT_LED_COUNT   78
#define DEFAULT_BRIGHTNESS  25        /* ~10%, 微光指示 */

/* ================================================================
 * 全局变量
 * ================================================================ */

static int g_running = 1;

/* ================================================================
 * 工具函数
 * ================================================================ */

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

static void sleep_ms(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/**
 * HSV → RGB 转换 (用于彩虹效果)
 *
 * h: 0~360, s: 0~255, v: 0~255
 * r/g/b: 0~255
 */
static void hsv_to_rgb(int h, int s, int v,
                       uint8_t *r, uint8_t *g, uint8_t *b)
{
    int region, remainder, p, q, t;

    if (s == 0) {
        *r = *g = *b = (uint8_t)v;
        return;
    }

    region = h / 60;
    remainder = (h % 60) * 256 / 60;

    p = (v * (255 - s)) / 255;
    q = (v * (255 - (s * remainder) / 256)) / 255;
    t = (v * (255 - (s * (256 - remainder)) / 256)) / 255;

    switch (region) {
        case 0:  *r = v; *g = t; *b = p; break;
        case 1:  *r = q; *g = v; *b = p; break;
        case 2:  *r = p; *g = v; *b = t; break;
        case 3:  *r = p; *g = q; *b = v; break;
        case 4:  *r = t; *g = p; *b = v; break;
        default: *r = v; *g = p; *b = q; break;
    }
}

/**
 * 应用全局亮度限制
 */
static uint8_t dim(uint8_t color, uint8_t brightness)
{
    return (uint8_t)(((uint16_t)color * brightness) / 255);
}

/* ================================================================
 * 演示模式
 * ================================================================ */

/**
 * 模式 1: 全部点亮 — 白/红/绿/蓝
 */
static void demo_solid_colors(int count, uint8_t brightness)
{
    printf("[1/5] 纯色测试: 白 → 红 → 绿 → 蓝\n");

    /* 白 */
    printf("  白色 (50%% 亮度)...\n");
    ws2812b_fill(dim(128, brightness), dim(128, brightness), dim(128, brightness));
    ws2812b_show();
    sleep_ms(1500);

    if (!g_running) return;

    /* 红 */
    printf("  红色...\n");
    ws2812b_fill(dim(255, brightness), 0, 0);
    ws2812b_show();
    sleep_ms(1500);

    if (!g_running) return;

    /* 绿 */
    printf("  绿色...\n");
    ws2812b_fill(0, dim(255, brightness), 0);
    ws2812b_show();
    sleep_ms(1500);

    if (!g_running) return;

    /* 蓝 */
    printf("  蓝色...\n");
    ws2812b_fill(0, 0, dim(255, brightness));
    ws2812b_show();
    sleep_ms(1500);
}

/**
 * 模式 2: 单灯追逐 (白色光点从左到右移动)
 */
static void demo_chase(int count, uint8_t brightness)
{
    int i;
    int rounds = 2;

    printf("[2/5] 追逐测试: 白色光点 × %d 轮 (按 Ctrl+C 跳过)\n", rounds);

    for (int r = 0; r < rounds && g_running; r++) {
        for (i = 0; i < count && g_running; i++) {
            ws2812b_clear();

            /* 当前灯 + 尾迹 */
            ws2812b_set_led(i,
                dim(255, brightness), dim(255, brightness), dim(255, brightness));

            if (i > 0) {
                ws2812b_set_led(i - 1,
                    dim(64, brightness), dim(64, brightness), dim(64, brightness));
            }
            if (i > 1) {
                ws2812b_set_led(i - 2,
                    dim(16, brightness), dim(16, brightness), dim(16, brightness));
            }

            ws2812b_show();
            sleep_ms(30);
        }

        /* 回来 */
        for (i = count - 1; i >= 0 && g_running; i--) {
            ws2812b_clear();

            ws2812b_set_led(i,
                dim(255, brightness), dim(255, brightness), dim(255, brightness));

            if (i < count - 1) {
                ws2812b_set_led(i + 1,
                    dim(64, brightness), dim(64, brightness), dim(64, brightness));
            }
            if (i < count - 2) {
                ws2812b_set_led(i + 2,
                    dim(16, brightness), dim(16, brightness), dim(16, brightness));
            }

            ws2812b_show();
            sleep_ms(30);
        }
    }
}

/**
 * 模式 3: 彩虹循环
 */
static void demo_rainbow(int count, uint8_t brightness, int duration_s)
{
    int hue_step = 2;   /* 每帧色相步进 */
    int hue = 0;
    int frame;

    printf("[3/5] 彩虹测试: 运行 %d 秒 (按 Ctrl+C 跳过)\n", duration_s);

    for (frame = 0; frame < duration_s * 20 && g_running; frame++) {
        int i;

        for (i = 0; i < count; i++) {
            uint8_t r, g, b;
            int led_hue = (hue + i * 360 / count) % 360;

            hsv_to_rgb(led_hue, 255, 255, &r, &g, &b);
            ws2812b_set_led(i,
                dim(r, brightness), dim(g, brightness), dim(b, brightness));
        }

        ws2812b_show();
        hue = (hue + hue_step) % 360;
        sleep_ms(50);
    }
}

/**
 * 模式 4: 呼吸灯效果 (蓝色)
 */
static void demo_breathing(int count, uint8_t brightness, int cycles)
{
    int i, c;

    printf("[4/5] 呼吸灯测试: %d 周期 (按 Ctrl+C 跳过)\n", cycles);

    for (c = 0; c < cycles && g_running; c++) {
        /* 渐亮 */
        for (i = 0; i <= 100 && g_running; i += 2) {
            uint8_t v = dim((uint8_t)(i * 255 / 100), brightness);
            ws2812b_fill(0, 0, v);
            ws2812b_show();
            sleep_ms(15);
        }
        /* 渐暗 */
        for (i = 100; i >= 0 && g_running; i -= 2) {
            uint8_t v = dim((uint8_t)(i * 255 / 100), brightness);
            ws2812b_fill(0, 0, v);
            ws2812b_show();
            sleep_ms(15);
        }
    }
}

/**
 * 模式 5: 进度条效果 (绿灯从 0 填充到 59)
 */
static void demo_progress(int count, uint8_t brightness)
{
    int i;

    printf("[5/5] 进度条测试: 绿灯填充 0→%d\n", count - 1);

    for (i = 0; i < count && g_running; i++) {
        ws2812b_set_led(i, 0, dim(255, brightness), 0);
        ws2812b_show();
        sleep_ms(50);
    }

    /* 反向逐个熄灭 */
    for (i = count - 1; i >= 0 && g_running; i--) {
        ws2812b_set_led(i, 0, 0, 0);
        ws2812b_show();
        sleep_ms(30);
    }
}

/* ================================================================
 * 使用说明
 * ================================================================ */

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -n <count>    灯珠数量 (默认: %d)\n", DEFAULT_LED_COUNT);
    printf("  -d <device>   SPI 设备 (默认: %s)\n", DEFAULT_SPI_DEV);
    printf("  -b <0-255>    全局亮度 (默认: %d)\n", DEFAULT_BRIGHTNESS);
    printf("  -v            详细模式 (打印 SPI 参数)\n");
    printf("  -t <sec>      彩虹模式运行秒数 (默认: 5)\n");
    printf("\n接线 (SPI1):\n");
    printf("  P9 Pin 38 (SPI1_MOSI) → WS2812B DIN\n");
    printf("  P9 Pin 2/4 (5V)       → WS2812B VCC\n");
    printf("  P9 Pin 6 (GND)         → WS2812B GND\n");
    printf("\nExamples:\n");
    printf("  %s                        默认: 78灯, SPI1\n", prog);
    printf("  %s -n 30 -b 64            30灯, 25%% 亮度\n", prog);
    printf("  %s -b 255                 全亮度 (注意功耗!)\n", prog);
}

/* ================================================================
 * 主程序
 * ================================================================ */

int main(int argc, char *argv[])
{
    const char *spi_dev = DEFAULT_SPI_DEV;
    int led_count = DEFAULT_LED_COUNT;
    int brightness = DEFAULT_BRIGHTNESS;
    int rainbow_duration = 5;
    int verbose = 0;
    int opt;

    /* 参数解析 */
    while ((opt = getopt(argc, argv, "n:d:b:t:vh")) != -1) {
        switch (opt) {
            case 'n':
                led_count = atoi(optarg);
                if (led_count < 1) led_count = 1;
                if (led_count > 1024) led_count = 1024;
                break;
            case 'd':
                spi_dev = optarg;
                break;
            case 'b':
                brightness = atoi(optarg);
                if (brightness < 0) brightness = 0;
                if (brightness > 255) brightness = 255;
                break;
            case 't':
                rainbow_duration = atoi(optarg);
                if (rainbow_duration < 1) rainbow_duration = 1;
                break;
            case 'v':
                verbose = 1;
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    printf("╔══════════════════════════════════════════╗\n");
    printf("║   WS2812B LED 灯带测试程序              ║\n");
    printf("╠══════════════════════════════════════════╣\n");
    printf("║   灯珠数: %-3d                            ║\n", led_count);
    printf("║   亮度:   %-3d/255                        ║\n", brightness);
    printf("║   SPI:    %s                  ║\n", spi_dev);
    printf("║   频率:   ~6.4 MHz                       ║\n");
    printf("╚══════════════════════════════════════════╝\n\n");

    /* 初始化灯带 */
    if (verbose) {
        printf("正在初始化 %s, %d 灯珠...\n", spi_dev, led_count);
    }

    if (ws2812b_init(spi_dev, led_count) < 0) {
        fprintf(stderr, "\n初始化失败!\n");
        fprintf(stderr, "检查:\n");
        fprintf(stderr, "  1) SPI 设备: ls %s\n", spi_dev);
        fprintf(stderr, "  2) 接线: P9 Pin38 (MOSI)→DIN, Pin2 (5V)→VCC, Pin6 (GND)→GND\n");
        fprintf(stderr, "  3) 供电: 5V 电源是否正常\n");
        return 1;
    }

    if (verbose) {
        printf("初始化成功\n\n");
    }

    /* 设置信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* ---- 运行演示模式 ---- */
    printf("按 Ctrl+C 随时停止\n\n");

    /* 1. 纯色 */
    demo_solid_colors(led_count, brightness);
    if (!g_running) goto cleanup;

    /* 2. 追逐 */
    demo_chase(led_count, brightness);
    if (!g_running) goto cleanup;

    /* 3. 彩虹 */
    demo_rainbow(led_count, brightness, rainbow_duration);
    if (!g_running) goto cleanup;

    /* 4. 呼吸灯 */
    demo_breathing(led_count, brightness, 3);
    if (!g_running) goto cleanup;

    /* 5. 进度条 */
    demo_progress(led_count, brightness);

cleanup:
    printf("\n正在熄灭灯带...\n");

    /* 熄灭所有灯珠并释放资源 */
    ws2812b_close();

    printf("Done.\n");
    return 0;
}
