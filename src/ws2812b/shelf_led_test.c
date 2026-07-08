/**
 * 书架灯带库测试程序
 *
 * 逐项验证 shelf_led API，方便确认物理 LED 映射是否正确。
 *
 * 编译: make shelf_led_test
 * 运行: ./shelf_led_test [-n 灯数] [-d SPI设备] [-b 亮度]
 *
 * 示例:
 *   ./shelf_led_test                    默认 78 灯, 亮度 10
 *   ./shelf_led_test -b 20              亮度 20 (~8%)
 *   ./shelf_led_test -b 0               静默模式 (不亮灯, 只打印)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

#include "shelf_led.h"

/* ================================================================
 * 默认参数
 * ================================================================ */

#define DEFAULT_SPI_DEV     "/dev/spidev1.0"
#define DEFAULT_LED_COUNT   78

/* ================================================================
 * 全局
 * ================================================================ */

static int g_running = 1;

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

/* ================================================================
 * 使用说明
 * ================================================================ */

static void print_usage(const char *prog)
{
    printf("Usage: %s [options]\n\n", prog);
    printf("Options:\n");
    printf("  -n <count>    灯珠数量 (默认: %d)\n", DEFAULT_LED_COUNT);
    printf("  -d <device>   SPI 设备 (默认: %s)\n", DEFAULT_SPI_DEV);
    printf("  -b <0-255>    全局亮度 (默认: %d)\n", SHELF_DEFAULT_BRIGHTNESS);
    printf("\n接线 (SPI1):\n");
    printf("  P9 Pin 38 (SPI1_MOSI) → WS2812B DIN\n");
    printf("  P9 Pin 2/4 (5V)       → WS2812B VCC\n");
    printf("  P9 Pin 6 (GND)         → WS2812B GND\n");
    printf("\nExamples:\n");
    printf("  %s                           默认亮度 %d\n", prog, SHELF_DEFAULT_BRIGHTNESS);
    printf("  %s -b 5                     暗光测试\n", prog);
    printf("  %s -b 0                     静默(不亮灯, 只打印)\n", prog);
}

/* ================================================================
 * 测试步骤
 * ================================================================ */

/**
 * 步骤 1: 物理顺序验证 — 逐个点亮，确认走线方向
 */
static void test_physical_order(int count)
{
    int i;

    printf("[1/7] 物理顺序验证: 逐个点亮 LED 0→%d\n", count - 1);
    printf("  确认: 左上角(#0) → 绕一圈 → 左下角(#%d)\n", count - 1);
    printf("  状态灯: #0(上层) #%d(下层)\n", SHELF_LED_STATUS_1);

    /* 逐个点亮前进 (绿色) */
    for (i = 0; i < count && g_running; i++) {
        shelf_led_clear();
        shelf_led_set(i, 0, 255, 0);
        shelf_led_show();
        sleep_ms(80);
    }

    /* 逐个熄灭后退 (红色) */
    for (i = count - 1; i >= 0 && g_running; i--) {
        shelf_led_clear();
        shelf_led_set(i, 255, 0, 0);
        shelf_led_show();
        sleep_ms(80);
    }
}

/**
 * 步骤 2: 状态灯测试
 */
static void test_status_leds(void)
{
    printf("[2/7] 状态指示灯测试\n");

    /* 上层状态灯 */
    printf("  上层状态灯 (#0): 红→绿→蓝→灭\n");
    shelf_led_clear();
    shelf_led_status(0, 255, 0, 0);   shelf_led_show();  sleep_ms(800);
    shelf_led_status(0, 0, 255, 0);   shelf_led_show();  sleep_ms(800);
    shelf_led_status(0, 0, 0, 255);   shelf_led_show();  sleep_ms(800);
    shelf_led_clear_status(0);        shelf_led_show();  sleep_ms(400);

    if (!g_running) return;

    /* 下层状态灯 */
    printf("  下层状态灯 (#%d): 红→绿→蓝→灭\n", SHELF_LED_STATUS_1);
    shelf_led_status(1, 255, 0, 0);   shelf_led_show();  sleep_ms(800);
    shelf_led_status(1, 0, 255, 0);   shelf_led_show();  sleep_ms(800);
    shelf_led_status(1, 0, 0, 255);   shelf_led_show();  sleep_ms(800);
    shelf_led_clear_status(1);        shelf_led_show();  sleep_ms(400);

    if (!g_running) return;

    /* 同时 */
    printf("  两层同时: 上层金 + 下层金\n");
    shelf_led_clear();
    shelf_led_status_both(255, 192, 0,  255, 192, 0);
    shelf_led_show();
    sleep_ms(1500);
}

/**
 * 步骤 3: 上层位置灯测试
 */
static void test_position_upper(void)
{
    int cm;

    printf("[3/7] 上层位置灯测试 (LED 1~37, 左→右)\n");

    /* 逐 cm 点亮 */
    for (cm = 0; cm < SHELF_WIDTH_CM && g_running; cm++) {
        shelf_led_clear();
        shelf_led_position(0, cm, cm, 0, 255, 0);
        shelf_led_show();
        sleep_ms(60);
    }

    /* 全亮 */
    shelf_led_clear();
    shelf_led_position_all(0, 0, 255, 0);
    shelf_led_show();
    sleep_ms(1000);

    /* 区间: 10~20cm */
    printf("  上层区间 10~20cm 亮蓝色\n");
    shelf_led_clear();
    shelf_led_position(0, 10, 20, 0, 0, 255);
    shelf_led_show();
    sleep_ms(2000);
}

/**
 * 步骤 4: 下层位置灯测试
 */
static void test_position_lower(void)
{
    int cm;

    printf("[4/7] 下层位置灯测试 (LED 39~77, 左→右)\n");

    for (cm = 0; cm < SHELF_WIDTH_CM && g_running; cm++) {
        shelf_led_clear();
        shelf_led_position(1, cm, cm, 0, 255, 0);
        shelf_led_show();
        sleep_ms(60);
    }

    shelf_led_clear();
    shelf_led_position_all(1, 0, 255, 0);
    shelf_led_show();
    sleep_ms(1000);

    /* 区间: 15~30cm */
    printf("  下层区间 15~30cm 亮蓝色\n");
    shelf_led_clear();
    shelf_led_position(1, 15, 30, 0, 0, 255);
    shelf_led_show();
    sleep_ms(2000);
}

/**
 * 步骤 5: 场景模式
 */
static void test_scenes(void)
{
    printf("[5/7] 场景模式测试\n");

    /* 休眠 */
    printf("  休眠 (全熄)\n");
    shelf_led_scene_sleep();
    shelf_led_show();
    sleep_ms(1000);

    if (!g_running) return;

    /* 感知 */
    printf("  感知 (绿灯 + 暖白底光)\n");
    shelf_led_scene_aware();
    shelf_led_show();
    sleep_ms(2000);

    if (!g_running) return;

    /* 取出 */
    printf("  书被取出: 上层 10~15cm\n");
    shelf_led_scene_borrowed(0, 10, 15);
    shelf_led_show();
    sleep_ms(2000);

    if (!g_running) return;

    /* 放回 */
    printf("  书被放回: 下层 20~25cm\n");
    shelf_led_scene_returned(1, 20, 25);
    shelf_led_show();
    sleep_ms(2000);

    if (!g_running) return;

    /* 错放 */
    printf("  错放警告: 下层 5~10cm\n");
    shelf_led_scene_misplaced(1, 5, 10);
    shelf_led_show();
    sleep_ms(2000);

    if (!g_running) return;

    /* 查找 */
    printf("  查找模式: 上层 25~30cm\n");
    shelf_led_scene_find(0, 25, 30);
    shelf_led_show();
    sleep_ms(2000);

    if (!g_running) return;

    /* 故障 */
    printf("  故障模式 (红灯)\n");
    shelf_led_scene_error();
    shelf_led_show();
    sleep_ms(1500);
}

/**
 * 步骤 6: 两本书同时显示
 */
static void test_two_books(void)
{
    printf("[6/7] 两本书同时显示\n");
    printf("  上层 5~8cm 红 + 下层 20~28cm 蓝\n");

    shelf_led_clear();
    shelf_led_position(0, 5, 8,   255, 0, 0);
    shelf_led_position(1, 20, 28, 0, 0, 255);
    shelf_led_show();
    sleep_ms(3000);
}

/**
 * 步骤 7: 彩虹扫光
 */
static void test_rainbow_sweep(int count)
{
    int i;

    printf("[7/7] 彩虹扫光 (视觉验收)\n");

    for (i = 0; i < count && g_running; i++) {
        int hue = (i * 360 / count) % 360;
        uint8_t r = 0, g = 0, b = 0;

        if (hue < 120) {
            r = (uint8_t)(255 * (120 - hue) / 120);
            g = (uint8_t)(255 * hue / 120);
            b = 0;
        } else if (hue < 240) {
            r = 0;
            g = (uint8_t)(255 * (240 - hue) / 120);
            b = (uint8_t)(255 * (hue - 120) / 120);
        } else {
            r = (uint8_t)(255 * (hue - 240) / 120);
            g = 0;
            b = (uint8_t)(255 * (360 - hue) / 120);
        }

        shelf_led_set(i, r, g, b);
    }
    shelf_led_show();
    sleep_ms(3000);
}

/* ================================================================
 * 主程序
 * ================================================================ */

int main(int argc, char *argv[])
{
    const char *spi_dev = DEFAULT_SPI_DEV;
    int led_count = DEFAULT_LED_COUNT;
    int brightness = SHELF_DEFAULT_BRIGHTNESS;
    int opt;

    while ((opt = getopt(argc, argv, "n:d:b:h")) != -1) {
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
            case 'h':
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    printf("╔══════════════════════════════════════════╗\n");
    printf("║   书架灯带库 (shelf_led) 测试程序       ║\n");
    printf("╠══════════════════════════════════════════╣\n");
    printf("║   灯珠数: %-3d   亮度: %-3d/255           ║\n",
           led_count, brightness);
    printf("║   SPI:    %s                  ║\n", spi_dev);
    printf("╚══════════════════════════════════════════╝\n\n");

    printf("物理布局:\n");
    printf("  上层 状态灯 #%-2d  位置灯 #%-2d ~ #%-2d  (%d灯)\n",
           SHELF_LED_STATUS_0, SHELF_LED_POS_0_FIRST,
           SHELF_LED_POS_0_LAST, SHELF_POS_COUNT_0);
    printf("  下层 状态灯 #%-2d  位置灯 #%-2d ~ #%-2d  (%d灯)\n",
           SHELF_LED_STATUS_1, SHELF_LED_POS_1_FIRST,
           SHELF_LED_POS_1_LAST, SHELF_POS_COUNT_1);
    printf("  走线: 左上角(#0) → 绕书架一圈 → 左下角(#%d)\n\n", led_count - 1);

    /* 初始化 */
    printf("初始化 %s...\n", spi_dev);
    if (shelf_led_init(spi_dev, led_count) < 0) {
        fprintf(stderr, "初始化失败!\n");
        fprintf(stderr, "检查: 1) SPI设备是否存在  2) 接线是否正确  3) 供电\n");
        return 1;
    }

    /* 设置亮度 */
    shelf_led_set_brightness((uint8_t)brightness);
    printf("初始化成功, 亮度=%d/255 (~%d%%)\n\n",
           brightness, brightness * 100 / 255);

    /* 信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    printf("按 Ctrl+C 随时停止\n\n");

    /* ---- 运行测试 ---- */

    test_physical_order(led_count);
    if (!g_running) goto cleanup;

    test_status_leds();
    if (!g_running) goto cleanup;

    test_position_upper();
    if (!g_running) goto cleanup;

    test_position_lower();
    if (!g_running) goto cleanup;

    test_scenes();
    if (!g_running) goto cleanup;

    test_two_books();
    if (!g_running) goto cleanup;

    test_rainbow_sweep(led_count);

cleanup:
    printf("\n正在熄灭灯带...\n");
    shelf_led_all_off();
    shelf_led_show();
    shelf_led_close();
    printf("Done.\n");
    return 0;
}
