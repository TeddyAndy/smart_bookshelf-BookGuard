/**
 * I2C 传感器合并读取程序 — BH1750 光照 + SHT30 温湿度
 *
 * 两个传感器共享 I2C2 总线：
 *   BH1750: 0x23 — 环境光照 (lux)
 *   SHT30:  0x44 — 温度 (°C) + 相对湿度 (%RH)
 *
 * 编译：make
 * 运行：./i2c_sensors /dev/i2c-2
 *
 * 硬件连接 (I2C2, P9):
 *   Pin3 (SDA) → BH1750 SDA + SHT30 SDA
 *   Pin5 (SCL) → BH1750 SCL + SHT30 SCL
 *   Pin1 (3.3V) → BH1750 VCC + SHT30 VCC
 *   Pin6 (GND)  → BH1750 GND + SHT30 GND
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

/* ================================================================
 * I2C 地址
 * ================================================================ */
#define BH1750_ADDR  0x23
#define SHT30_ADDR   0x44

/* ================================================================
 * BH1750 命令码
 * ================================================================ */
#define BH1750_POWER_ON    0x01
#define BH1750_POWER_DOWN  0x00
#define BH1750_CONT_H_RES  0x10   /* 连续高分辨率, 1 lx, 120ms */

/* ================================================================
 * SHT30 命令码
 * ================================================================ */
#define SHT30_SINGLE_HIGH   0x2C06  /* 单次高精度, 时钟拉伸使能, ~15ms */
#define SHT30_SOFT_RESET    0x30A2

/* ================================================================
 * 默认参数
 * ================================================================ */
#define DEFAULT_INTERVAL_MS 1000
#define DEFAULT_I2C_DEV     "/dev/i2c-2"

/* ================================================================
 * 全局变量
 * ================================================================ */
static int g_running = 1;
static int g_verbose = 0;

/* ================================================================
 * 工具函数
 * ================================================================ */

static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

static void get_time_str(char *buf, int size)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buf, size, "%H:%M:%S", t);
}

/**
 * 切换 I2C 从设备地址
 * 同一 fd 上切换地址是安全的 — I2C 总线协议支持多从设备
 */
static int i2c_select(int fd, unsigned char addr)
{
    if (ioctl(fd, I2C_SLAVE, addr) < 0) {
        perror("ioctl I2C_SLAVE");
        return -1;
    }
    return 0;
}

/**
 * SHT30 CRC-8 校验 (多项式 0x31, 初始值 0xFF)
 */
static uint8_t sht30_crc8(const uint8_t *data, int len)
{
    const uint8_t POLY = 0x31;
    uint8_t crc = 0xFF;
    int i, j;

    for (j = 0; j < len; j++) {
        crc ^= data[j];
        for (i = 0; i < 8; i++) {
            if (crc & 0x80)
                crc = (crc << 1) ^ POLY;
            else
                crc <<= 1;
        }
    }
    return crc;
}

/* ================================================================
 * BH1750 光照传感器
 * ================================================================ */

static int bh1750_send_cmd(int fd, unsigned char cmd)
{
    if (write(fd, &cmd, 1) != 1) {
        perror("BH1750 write");
        return -1;
    }
    return 0;
}

/**
 * 初始化 BH1750 为连续高分辨率模式
 * 调用后传感器持续测量，每次 read 返回最新值
 */
static int bh1750_init(int fd)
{
    if (i2c_select(fd, BH1750_ADDR) < 0)
        return -1;

    /* 上电 */
    if (bh1750_send_cmd(fd, BH1750_POWER_ON) < 0)
        return -1;
    usleep(10000);

    /* 连续高分辨率模式 */
    if (bh1750_send_cmd(fd, BH1750_CONT_H_RES) < 0)
        return -1;

    /* 等待首次测量完成 (高分辨率需 120ms, 加余量) */
    usleep(180000);

    return 0;
}

static int bh1750_read(int fd, float *lux)
{
    unsigned char buf[2];

    if (i2c_select(fd, BH1750_ADDR) < 0)
        return -1;

    if (read(fd, buf, 2) != 2) {
        perror("BH1750 read");
        return -1;
    }

    unsigned short raw = (buf[0] << 8) | buf[1];
    *lux = (float)raw / 1.2f;

    return 0;
}

/* ================================================================
 * SHT30 温湿度传感器
 * ================================================================ */

static int sht30_send_cmd(int fd, unsigned short cmd)
{
    unsigned char buf[2];
    buf[0] = (cmd >> 8) & 0xFF;
    buf[1] = cmd & 0xFF;

    if (write(fd, buf, 2) != 2) {
        perror("SHT30 write");
        return -1;
    }
    return 0;
}

/**
 * 单次读取 SHT30:
 *   1. 切换到 SHT30 地址
 *   2. 发送单次测量命令
 *   3. 等待测量完成 (高精度 ~15ms, 给 20ms)
 *   4. 读取 6 字节 + CRC 校验
 */
static int sht30_read(int fd, float *temp_c, float *humidity)
{
    unsigned char buf[6];

    if (i2c_select(fd, SHT30_ADDR) < 0)
        return -1;

    /* 发送单次测量命令 */
    if (sht30_send_cmd(fd, SHT30_SINGLE_HIGH) < 0)
        return -1;

    /* 等待测量完成 (时钟拉伸模式下传感器会拉住 SCL, 但给个上限) */
    usleep(25000);

    if (read(fd, buf, 6) != 6) {
        perror("SHT30 read");
        return -1;
    }

    /* CRC-8 校验 */
    if (sht30_crc8(&buf[0], 2) != buf[2]) {
        fprintf(stderr, "SHT30 温度 CRC 错误\n");
        return -1;
    }
    if (sht30_crc8(&buf[3], 2) != buf[5]) {
        fprintf(stderr, "SHT30 湿度 CRC 错误\n");
        return -1;
    }

    unsigned short raw_temp = (buf[0] << 8) | buf[1];
    unsigned short raw_hum  = (buf[3] << 8) | buf[4];

    *temp_c   = -45.0f + 175.0f * ((float)raw_temp / 65535.0f);
    *humidity = 100.0f * ((float)raw_hum / 65535.0f);

    if (*humidity > 100.0f) *humidity = 100.0f;
    if (*humidity < 0.0f)   *humidity = 0.0f;

    return 0;
}

/* ================================================================
 * 舒适度评价
 * ================================================================ */

static const char *temp_feel(float t)
{
    if (t < 10.0f) return "寒冷";
    if (t < 18.0f) return "偏冷";
    if (t < 26.0f) return "舒适";
    if (t < 32.0f) return "偏热";
    return "炎热";
}

static const char *hum_feel(float h)
{
    if (h < 20.0f) return "干燥";
    if (h < 40.0f) return "偏干";
    if (h < 60.0f) return "舒适";
    if (h < 80.0f) return "偏湿";
    return "潮湿";
}

static const char *lux_level(float lux)
{
    if (lux < 0.3f)  return "黑夜";
    if (lux < 10.0f)  return "黄昏";
    if (lux < 50.0f)  return "较暗";
    if (lux < 200.0f) return "办公";
    if (lux < 500.0f) return "窗边";
    if (lux < 2000.0f)return "明亮";
    return "强光";
}

/**
 * 露点温度 (Magnus 公式近似)
 */
static float dew_point(float t, float rh)
{
    float a = 17.27f, b = 237.7f;
    float gamma = (a * t) / (b + t) + logf(rh / 100.0f);
    return (b * gamma) / (a - gamma);
}

/* ================================================================
 * 主程序
 * ================================================================ */

static void print_usage(const char *prog)
{
    printf("Usage: %s [options] [i2c_device]\n\n", prog);
    printf("Options:\n");
    printf("  -i <ms>   读取间隔 (ms), 默认 %d\n", DEFAULT_INTERVAL_MS);
    printf("  -n <n>    读取次数, 0=无限循环 (默认)\n");
    printf("  -v        详细输出\n");
    printf("  -1        单次读取\n");
    printf("  -j        JSON 格式输出\n");
    printf("\n默认设备: %s\n", DEFAULT_I2C_DEV);
}

int main(int argc, char *argv[])
{
    const char *device = DEFAULT_I2C_DEV;
    int interval_ms = DEFAULT_INTERVAL_MS;
    int max_count = 0;      /* 0 = 无限 */
    int json_mode = 0;
    int fd, count = 0;
    int opt;

    while ((opt = getopt(argc, argv, "i:n:v1jh?")) != -1) {
        switch (opt) {
            case 'i':
                interval_ms = atoi(optarg);
                if (interval_ms < 100) interval_ms = 100;
                break;
            case 'n':
                max_count = atoi(optarg);
                break;
            case 'v':
                g_verbose = 1;
                break;
            case '1':
                max_count = 1;
                break;
            case 'j':
                json_mode = 1;
                break;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (optind < argc)
        device = argv[optind];

    /* 打开 I2C 设备 */
    fd = open(device, O_RDWR);
    if (fd < 0) {
        perror("open I2C");
        fprintf(stderr, "检查: 设备 %s 是否存在\n", device);
        return 1;
    }

    /* 初始化 BH1750 为连续模式 */
    if (bh1750_init(fd) < 0) {
        fprintf(stderr, "BH1750 初始化失败\n");
        close(fd);
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 打印表头 */
    if (json_mode) {
        /* JSON 不需要表头 */
    } else {
        printf("%-10s %9s %7s %6s %6s %10s %6s %6s\n",
               "时间", "温度°C", "湿度%", "体感", "干湿",
               "光照lx", "亮度", "露点°C");
        printf("---------------------------------------------------------------\n");
    }

    /* 主循环 */
    while (g_running) {
        float temp, hum, lux;

        /* 读 SHT30 */
        if (sht30_read(fd, &temp, &hum) < 0) {
            usleep(100000);
            continue;
        }

        /* 读 BH1750 */
        if (bh1750_read(fd, &lux) < 0) {
            usleep(100000);
            continue;
        }

        count++;

        if (json_mode) {
            printf("{\"time\":\"");
            char ts[16]; get_time_str(ts, sizeof(ts));
            printf("%s\",\"temp\":%.2f,\"hum\":%.1f,\"lux\":%.1f,"
                   "\"dew\":%.2f,\"temp_feel\":\"%s\",\"hum_feel\":\"%s\","
                   "\"lux_level\":\"%s\"}\n",
                   ts, temp, hum, lux, dew_point(temp, hum),
                   temp_feel(temp), hum_feel(hum), lux_level(lux));
        } else {
            char ts[16]; get_time_str(ts, sizeof(ts));
            printf("%-10s %8.2f  %6.1f  %6s %6s %9.1f  %6s %6.2f\n",
                   ts, temp, hum,
                   temp_feel(temp), hum_feel(hum),
                   lux, lux_level(lux),
                   dew_point(temp, hum));
        }

        fflush(stdout);

        if (max_count > 0 && count >= max_count)
            break;

        usleep(interval_ms * 1000);
    }

    /* 关闭 BH1750 */
    i2c_select(fd, BH1750_ADDR);
    bh1750_send_cmd(fd, BH1750_POWER_DOWN);

    close(fd);

    if (!json_mode)
        printf("共读取 %d 次.\n", count);

    return 0;
}
