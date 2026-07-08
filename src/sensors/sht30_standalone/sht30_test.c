/**
 * SHT30 温湿度传感器 I2C 测试程序
 *
 * 功能：
 * - 单次/连续读取温度 (°C) 和相对湿度 (%RH)
 * - 支持多种精度/频率模式
 * - CRC-8 校验，确保数据完整性
 * - 加热器控制、软复位等高级功能
 *
 * 编译：arm-none-linux-gnueabihf-gcc sht30_test.c -o sht30_test
 * 运行：./sht30_test /dev/i2c-2
 *
 * 硬件连接 (GY-SHT30-D 模块):
 *   P9 Pin3 (I2C2_SDA) → SHT30 SDA
 *   P9 Pin5 (I2C2_SCL) → SHT30 SCL
 *   P9 Pin1 (VCC_3V3)   → SHT30 VCC
 *   P9 Pin6 (GND)       → SHT30 GND
 *   I2C 地址: 0x44 (ADDR 引脚默认接 GND)
 *
 * 参考: Sensirion SHT3x-DIS Datasheet
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
#include <getopt.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

/* ================================================================
 * SHT30 命令码 (2 字节，高字节在前)
 * ================================================================ */

/* 单次测量 (时钟拉伸使能) */
#define CMD_SINGLE_HIGH         0x2C06   /* 高重复精度 (最准, ~15ms) */
#define CMD_SINGLE_MED          0x2C0D   /* 中重复精度 (~6ms) */
#define CMD_SINGLE_LOW          0x2C10   /* 低重复精度 (最快, ~3ms) */

/* 单次测量 (时钟拉伸禁止) */
#define CMD_SINGLE_HIGH_NOCS    0x2400   /* 高重复精度，无时钟拉伸 */

/* 周期性测量 */
#define CMD_PERIODIC_0M5_LOW    0x202F   /* 0.5 mps, 低重复精度 */
#define CMD_PERIODIC_0M5_MED    0x2024   /* 0.5 mps, 中重复精度 */
#define CMD_PERIODIC_0M5_HIGH   0x2032   /* 0.5 mps, 高重复精度 */
#define CMD_PERIODIC_1_LOW      0x212D   /* 1 mps, 低重复精度 */
#define CMD_PERIODIC_1_MED      0x2126   /* 1 mps, 中重复精度 */
#define CMD_PERIODIC_1_HIGH     0x2130   /* 1 mps, 高重复精度 */
#define CMD_PERIODIC_2_LOW      0x2220   /* 2 mps, 低重复精度 */
#define CMD_PERIODIC_2_MED      0x222B   /* 2 mps, 中重复精度 */
#define CMD_PERIODIC_2_HIGH     0x2236   /* 2 mps, 高重复精度 */
#define CMD_PERIODIC_4_LOW      0x2334   /* 4 mps, 低重复精度 */
#define CMD_PERIODIC_4_MED      0x234B   /* 4 mps, 中重复精度 */
#define CMD_PERIODIC_4_HIGH     0x2362   /* 4 mps, 高重复精度 */
#define CMD_PERIODIC_10_LOW     0x2721   /* 10 mps, 低重复精度 */
#define CMD_PERIODIC_10_MED     0x2737   /* 10 mps, 中重复精度 */
#define CMD_PERIODIC_10_HIGH    0x2764   /* 10 mps, 高重复精度 */

/* 读取周期测量数据 */
#define CMD_FETCH_DATA          0xE000

/* 停止周期测量 */
#define CMD_BREAK               0x3093

/* 其他命令 */
#define CMD_SOFT_RESET          0x30A2
#define CMD_HEATER_ON           0x306D
#define CMD_HEATER_OFF          0x3066
#define CMD_READ_STATUS         0xF32D
#define CMD_CLEAR_STATUS        0x3041

#define DEFAULT_MODE            CMD_SINGLE_HIGH
#define DEFAULT_INTERVAL        1000    /* ms */
#define DEFAULT_I2C_ADDR        0x44

/* ================================================================
 * 全局变量
 * ================================================================ */

static int g_running = 1;
static int g_verbose = 0;

/* ================================================================
 * 工具函数
 * ================================================================ */

/**
 * CRC-8 校验 (多项式 0x31, 初始值 0xFF)
 * 用于验证 SHT30 温度和湿度数据的完整性
 */
static uint8_t sht30_crc8(const uint8_t *data, int len)
{
    const uint8_t POLY = 0x31;
    uint8_t crc = 0xFF;
    int i, j;

    for (j = 0; j < len; j++) {
        crc ^= data[j];
        for (i = 0; i < 8; i++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ POLY;
            } else {
                crc <<= 1;
            }
        }
    }

    return crc;
}

/**
 * 模式名称
 */
static const char *mode_name(unsigned short cmd)
{
    switch (cmd) {
        case CMD_SINGLE_HIGH:       return "单次高精度";
        case CMD_SINGLE_MED:        return "单次中精度";
        case CMD_SINGLE_LOW:        return "单次低精度";
        case CMD_PERIODIC_0M5_HIGH: return "周期0.5mps高精度";
        case CMD_PERIODIC_0M5_MED:  return "周期0.5mps中精度";
        case CMD_PERIODIC_0M5_LOW:  return "周期0.5mps低精度";
        case CMD_PERIODIC_1_HIGH:   return "周期1mps高精度";
        case CMD_PERIODIC_1_MED:    return "周期1mps中精度";
        case CMD_PERIODIC_1_LOW:    return "周期1mps低精度";
        case CMD_PERIODIC_2_HIGH:   return "周期2mps高精度";
        case CMD_PERIODIC_2_MED:    return "周期2mps中精度";
        case CMD_PERIODIC_2_LOW:    return "周期2mps低精度";
        case CMD_PERIODIC_4_HIGH:   return "周期4mps高精度";
        case CMD_PERIODIC_4_MED:    return "周期4mps中精度";
        case CMD_PERIODIC_4_LOW:    return "周期4mps低精度";
        case CMD_PERIODIC_10_HIGH:  return "周期10mps高精度";
        case CMD_PERIODIC_10_MED:   return "周期10mps中精度";
        case CMD_PERIODIC_10_LOW:   return "周期10mps低精度";
        default:                    return "未知";
    }
}

/**
 * 返回单次测量的等待时间 (ms)
 */
static int mode_delay_ms(unsigned short cmd)
{
    switch (cmd) {
        case CMD_SINGLE_HIGH:  return 20;
        case CMD_SINGLE_MED:   return 10;
        case CMD_SINGLE_LOW:   return 5;
        default:               return 20;
    }
}

/**
 * 判断是否为周期模式
 *
 * SHT30 命令编码规律:
 *   0x2Cxx = 单次测量 (时钟拉伸使能)
 *   0x24xx = 单次测量 (时钟拉伸禁止)
 *   0x20xx, 0x21xx, 0x22xx, 0x23xx, 0x27xx = 周期测量
 *   0x30xx = 系统命令 (复位/加热器/停止)
 *   0xF32D = 读状态寄存器
 */
static int is_periodic(unsigned short cmd)
{
    unsigned char msb = (cmd >> 8) & 0xFF;

    /* 周期模式命令的高字节范围 */
    if (msb == 0x20 || msb == 0x21 || msb == 0x22 ||
        msb == 0x23 || msb == 0x27) {
        return 1;
    }

    return 0;
}

/**
 * 信号处理
 */
static void signal_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/**
 * 获取时间字符串
 */
static void get_time_str(char *buf, int size)
{
    time_t now;
    struct tm *t;

    time(&now);
    t = localtime(&now);
    strftime(buf, size, "%H:%M:%S", t);
}

/* ================================================================
 * I2C 操作
 * ================================================================ */

/**
 * 发送 2 字节命令
 */
static int sht30_send_cmd(int fd, unsigned short cmd)
{
    unsigned char buf[2];

    buf[0] = (cmd >> 8) & 0xFF;
    buf[1] = cmd & 0xFF;

    if (write(fd, buf, 2) != 2) {
        perror("I2C write");
        return -1;
    }

    return 0;
}

/**
 * 读取温湿度数据 (6 字节) + CRC 校验
 */
static int sht30_read(int fd, float *temp_c, float *humidity)
{
    unsigned char buf[6];
    unsigned short raw_temp, raw_hum;
    uint8_t crc_temp, crc_hum;

    if (read(fd, buf, 6) != 6) {
        perror("I2C read");
        return -1;
    }

    /* 解析原始值 */
    raw_temp = (buf[0] << 8) | buf[1];
    raw_hum  = (buf[3] << 8) | buf[4];

    /* CRC-8 校验 */
    crc_temp = sht30_crc8(&buf[0], 2);
    crc_hum  = sht30_crc8(&buf[3], 2);

    if (crc_temp != buf[2]) {
        fprintf(stderr, "⚠ 温度 CRC 校验失败: 计算=0x%02X, 收到=0x%02X\n",
                crc_temp, buf[2]);
        return -1;
    }

    if (crc_hum != buf[5]) {
        fprintf(stderr, "⚠ 湿度 CRC 校验失败: 计算=0x%02X, 收到=0x%02X\n",
                crc_hum, buf[5]);
        return -1;
    }

    /* SHT30 官方公式:
     *   T[°C] = -45 + 175 * (ST / 65535)
     *   RH[%] = 100 * (SRH / 65535)
     */
    *temp_c   = -45.0f + 175.0f * ((float)raw_temp / 65535.0f);
    *humidity = 100.0f * ((float)raw_hum / 65535.0f);

    /* 裁剪到合理范围 */
    if (*humidity > 100.0f) *humidity = 100.0f;
    if (*humidity < 0.0f)   *humidity = 0.0f;

    return 0;
}

/**
 * 打开 I2C 设备
 */
static int sht30_open(const char *device, unsigned char addr)
{
    int fd;

    fd = open(device, O_RDWR);
    if (fd < 0) {
        perror("open I2C device");
        return -1;
    }

    if (ioctl(fd, I2C_SLAVE, addr) < 0) {
        perror("ioctl I2C_SLAVE");
        close(fd);
        return -1;
    }

    return fd;
}

/* ================================================================
 * 传感器操作
 * ================================================================ */

/**
 * 软复位传感器
 */
static int sht30_reset(int fd)
{
    if (sht30_send_cmd(fd, CMD_SOFT_RESET) < 0) {
        fprintf(stderr, "软复位失败\n");
        return -1;
    }

    /* 复位后需等待 1ms */
    usleep(2000);
    return 0;
}

/**
 * 发送测量命令并等待
 */
static int sht30_start_measure(int fd, unsigned short mode)
{
    if (sht30_send_cmd(fd, mode) < 0) {
        fprintf(stderr, "发送测量命令失败\n");
        return -1;
    }

    /* 对于无时钟拉伸的单次模式，需要等待 */
    if (!is_periodic(mode)) {
        usleep(mode_delay_ms(mode) * 1000);
    } else {
        /* 周期模式首次测量需要等待 */
        usleep(20000);
    }

    return 0;
}

/**
 * 读取状态寄存器
 */
static int sht30_read_status(int fd, uint16_t *status)
{
    unsigned char buf[3];

    if (sht30_send_cmd(fd, CMD_READ_STATUS) < 0) {
        return -1;
    }

    usleep(5000);

    if (read(fd, buf, 3) != 3) {
        perror("read status");
        return -1;
    }

    *status = (buf[0] << 8) | buf[1];

    /* 校验 */
    if (sht30_crc8(buf, 2) != buf[2]) {
        fprintf(stderr, "⚠ 状态寄存器 CRC 校验失败\n");
        return -1;
    }

    return 0;
}

/**
 * 打印状态寄存器内容
 */
static void print_status(uint16_t status)
{
    printf("\n状态寄存器: 0x%04X\n", status);
    printf("  [%c] 报警挂起\n",        (status & 0x8000) ? 'X' : ' ');
    printf("  [%c] 加热器开启\n",      (status & 0x2000) ? 'X' : ' ');
    printf("  [%c] RH 跟踪报警\n",     (status & 0x0800) ? 'X' : ' ');
    printf("  [%c] T 跟踪报警\n",      (status & 0x0400) ? 'X' : ' ');
    printf("  [%c] 系统复位检测\n",   (status & 0x0010) ? 'X' : ' ');
    printf("  [%c] 命令状态\n",        (status & 0x0002) ? 'X' : ' ');
    printf("  [%c] 写数据校验失败\n",  (status & 0x0001) ? 'X' : ' ');
}

/* ================================================================
 * 显示与输出
 * ================================================================ */

/**
 * 温度舒适度评价
 */
static const char *temp_comfort(float t)
{
    if (t < 10.0f)  return "寒冷";
    if (t < 18.0f)  return "偏冷";
    if (t < 26.0f)  return "舒适";
    if (t < 32.0f)  return "偏热";
    return "炎热";
}

/**
 * 湿度舒适度评价
 */
static const char *hum_comfort(float h)
{
    if (h < 20.0f)  return "干燥";
    if (h < 40.0f)  return "偏干";
    if (h < 60.0f)  return "舒适";
    if (h < 80.0f)  return "偏湿";
    return "潮湿";
}

/**
 * 绘制简单温度柱状图
 */
static void draw_temp_bar(float temp, int width)
{
    int i;
    int bar = (int)((temp + 10.0f) / 60.0f * width);  /* -10~50°C 映射到宽度 */
    if (bar < 0)  bar = 0;
    if (bar > width) bar = width;

    printf("[");
    for (i = 0; i < width; i++) {
        if (i < bar) {
            putchar('=');
        } else {
            putchar(' ');
        }
    }
    printf("]");
}

/**
 * 绘制简单湿度柱状图
 */
static void draw_hum_bar(float hum, int width)
{
    int i;
    int bar = (int)(hum / 100.0f * width);
    if (bar < 0)  bar = 0;
    if (bar > width) bar = width;

    printf("[");
    for (i = 0; i < width; i++) {
        if (i < bar) {
            putchar('=');
        } else {
            putchar(' ');
        }
    }
    printf("]");
}

/**
 * 计算露点温度 (Magnus 公式近似)
 */
static float dew_point(float temp_c, float humidity)
{
    float a = 17.27f;
    float b = 237.7f;
    float gamma = (a * temp_c) / (b + temp_c) + logf(humidity / 100.0f);
    return (b * gamma) / (a - gamma);
}

/**
 * 打印使用说明
 */
static void print_usage(const char *prog)
{
    printf("Usage: %s [options] <i2c_device>\n\n", prog);
    printf("Options:\n");
    printf("  -v              详细模式 (显示 CRC、原始值)\n");
    printf("  -a <addr>       I2C 地址 (默认: 0x%02X)\n", DEFAULT_I2C_ADDR);
    printf("  -m <mode>       测量模式 (默认: single-h)\n");
    printf("                    单次模式:\n");
    printf("                      single-h   高精度 (~15ms)\n");
    printf("                      single-m   中精度 (~6ms)\n");
    printf("                      single-l   低精度 (~3ms)\n");
    printf("                    周期模式:\n");
    printf("                      p0m5-h     0.5次/秒 高精度\n");
    printf("                      p1-h       1次/秒 高精度\n");
    printf("                      p1-m       1次/秒 中精度\n");
    printf("                      p2-l       2次/秒 低精度\n");
    printf("                      p4-l       4次/秒 低精度\n");
    printf("                      p10-l      10次/秒 低精度\n");
    printf("  -i <ms>         连续读取间隔 (毫秒, 默认: %d)\n", DEFAULT_INTERVAL);
    printf("  -n <count>      读取次数 (单次模式, 默认: 1)\n");
    printf("  -1              单次读取 (等同于 -m single-h)\n");
    printf("  --heater-on     开启加热器 (用于除湿/自检)\n");
    printf("  --heater-off    关闭加热器\n");
    printf("  --status        读取状态寄存器\n");
    printf("  --reset         软复位传感器\n");
    printf("\nExamples:\n");
    printf("  %s -1 /dev/i2c-2                  单次读取\n", prog);
    printf("  %s /dev/i2c-2                      连续读取 (1s间隔)\n", prog);
    printf("  %s -i 5000 /dev/i2c-2              每5秒读取\n", prog);
    printf("  %s -m p1-h -i 1000 /dev/i2c-2      周期模式 1次/秒\n", prog);
    printf("  %s --status /dev/i2c-2             读取状态寄存器\n", prog);
    printf("  %s --heater-on -1 /dev/i2c-2       加热器自检\n", prog);
}

/* ================================================================
 * 主流程
 * ================================================================ */

/**
 * 单次读取
 */
static int read_once(int fd, unsigned short mode, float *temp, float *hum)
{
    if (is_periodic(mode)) {
        /* 周期模式：用 FETCH DATA 读取 */
        if (sht30_read(fd, temp, hum) < 0) {
            return -1;
        }
    } else {
        /* 单次模式 */
        if (sht30_start_measure(fd, mode) < 0) {
            return -1;
        }
        if (sht30_read(fd, temp, hum) < 0) {
            return -1;
        }
    }
    return 0;
}

/**
 * 连续读取
 */
static int read_continuous(int fd, unsigned short mode, int interval_ms)
{
    float temp, hum, dp;

    /* 如果是周期模式，先发送周期测量命令 */
    if (is_periodic(mode)) {
        if (sht30_send_cmd(fd, mode) < 0) {
            fprintf(stderr, "启动周期测量失败\n");
            return -1;
        }
        usleep(20000);
    }

    printf("模式: %s\n", mode_name(mode));
    printf("间隔: %d ms\n", interval_ms);
    printf("按 Ctrl+C 停止...\n\n");

    /* 表头 */
    printf("%-10s %10s %8s %6s %6s %s\n",
           "时间", "温度(°C)", "湿度(%)", "体感", "环境", "露点(°C)");
    printf("----------------------------------------------------------------\n");

    while (g_running) {
        char time_str[16];

        if (read_once(fd, mode, &temp, &hum) < 0) {
            usleep(interval_ms * 1000);
            continue;
        }

        dp = dew_point(temp, hum);

        if (g_verbose) {
            /* 详细模式：显示更多信息 */
            get_time_str(time_str, sizeof(time_str));
            printf("%-10s %9.2f   %7.1f   %s  %s  %7.2f\n",
                   time_str, temp, hum,
                   temp_comfort(temp), hum_comfort(hum), dp);
            printf("    温度: ");
            draw_temp_bar(temp, 25);
            printf("\n    湿度: ");
            draw_hum_bar(hum, 25);
            printf("\n");
        } else {
            get_time_str(time_str, sizeof(time_str));
            printf("%-10s %9.2f   %7.1f   %s  %s  %7.2f  ",
                   time_str, temp, hum,
                   temp_comfort(temp), hum_comfort(hum), dp);
            draw_temp_bar(temp, 15);
            putchar('\n');
        }

        fflush(stdout);

        /* 非周期模式：用 sleep 控制间隔 */
        if (!is_periodic(mode)) {
            usleep(interval_ms * 1000);
        } else {
            /* 周期模式：传感器自动测量，按间隔读取 */
            usleep(interval_ms * 1000);
        }
    }

    /* 停止周期测量 */
    if (is_periodic(mode)) {
        sht30_send_cmd(fd, CMD_BREAK);
    }

    return 0;
}

int main(int argc, char *argv[])
{
    const char *device = NULL;
    unsigned char i2c_addr = DEFAULT_I2C_ADDR;
    unsigned short mode = DEFAULT_MODE;
    int interval_ms = DEFAULT_INTERVAL;
    int do_once = 0;
    int count = 1;
    int do_heater_on = 0;
    int do_heater_off = 0;
    int do_status = 0;
    int do_reset = 0;
    int fd;
    int opt;
    int long_idx = 0;

    /* 长选项 */
    struct option long_opts[] = {
        {"heater-on",  no_argument, &do_heater_on, 1},
        {"heater-off", no_argument, &do_heater_off, 1},
        {"status",     no_argument, &do_status, 1},
        {"reset",      no_argument, &do_reset, 1},
        {NULL, 0, NULL, 0}
    };

    /* 解析参数 */
    while ((opt = getopt_long(argc, argv, "va:m:n:i:1h?",
                              long_opts, &long_idx)) != -1) {
        switch (opt) {
            case 'v':
                g_verbose = 1;
                break;
            case 'a':
                i2c_addr = (unsigned char)strtol(optarg, NULL, 0);
                break;
            case 'm':
                if (strcmp(optarg, "single-h") == 0) {
                    mode = CMD_SINGLE_HIGH;
                } else if (strcmp(optarg, "single-m") == 0) {
                    mode = CMD_SINGLE_MED;
                } else if (strcmp(optarg, "single-l") == 0) {
                    mode = CMD_SINGLE_LOW;
                } else if (strcmp(optarg, "p0m5-h") == 0) {
                    mode = CMD_PERIODIC_0M5_HIGH;
                } else if (strcmp(optarg, "p0m5-m") == 0) {
                    mode = CMD_PERIODIC_0M5_MED;
                } else if (strcmp(optarg, "p0m5-l") == 0) {
                    mode = CMD_PERIODIC_0M5_LOW;
                } else if (strcmp(optarg, "p1-h") == 0) {
                    mode = CMD_PERIODIC_1_HIGH;
                } else if (strcmp(optarg, "p1-m") == 0) {
                    mode = CMD_PERIODIC_1_MED;
                } else if (strcmp(optarg, "p1-l") == 0) {
                    mode = CMD_PERIODIC_1_LOW;
                } else if (strcmp(optarg, "p2-h") == 0) {
                    mode = CMD_PERIODIC_2_HIGH;
                } else if (strcmp(optarg, "p2-m") == 0) {
                    mode = CMD_PERIODIC_2_MED;
                } else if (strcmp(optarg, "p2-l") == 0) {
                    mode = CMD_PERIODIC_2_LOW;
                } else if (strcmp(optarg, "p4-h") == 0) {
                    mode = CMD_PERIODIC_4_HIGH;
                } else if (strcmp(optarg, "p4-m") == 0) {
                    mode = CMD_PERIODIC_4_MED;
                } else if (strcmp(optarg, "p4-l") == 0) {
                    mode = CMD_PERIODIC_4_LOW;
                } else if (strcmp(optarg, "p10-h") == 0) {
                    mode = CMD_PERIODIC_10_HIGH;
                } else if (strcmp(optarg, "p10-m") == 0) {
                    mode = CMD_PERIODIC_10_MED;
                } else if (strcmp(optarg, "p10-l") == 0) {
                    mode = CMD_PERIODIC_10_LOW;
                } else {
                    fprintf(stderr, "未知模式: %s\n", optarg);
                    print_usage(argv[0]);
                    return 1;
                }
                break;
            case 'n':
                count = atoi(optarg);
                if (count < 1) count = 1;
                break;
            case 'i':
                interval_ms = atoi(optarg);
                if (interval_ms < 100) interval_ms = 100;
                break;
            case '1':
                mode = CMD_SINGLE_HIGH;
                do_once = 1;
                break;
            case 0:
                /* 长选项已由 flag 变量处理 */
                break;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Error: 未指定 I2C 设备\n\n");
        print_usage(argv[0]);
        return 1;
    }

    device = argv[optind];

    /* 打开 I2C 设备 */
    if (g_verbose) {
        printf("打开 %s, I2C 地址 0x%02X...\n", device, i2c_addr);
    }

    fd = sht30_open(device, i2c_addr);
    if (fd < 0) {
        fprintf(stderr, "无法打开 %s (地址 0x%02X)\n", device, i2c_addr);
        fprintf(stderr, "检查: 1) 接线  2) ls %s  3) i2cdetect -y -r <bus>\n",
                device);
        return 1;
    }

    if (g_verbose) {
        printf("I2C 设备已打开\n");
    }

    /* 设置信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* ---- 执行特殊命令 ---- */

    /* 软复位 */
    if (do_reset) {
        printf("正在软复位 SHT30...\n");
        if (sht30_reset(fd) == 0) {
            printf("软复位成功\n");
        }
    }

    /* 读取状态寄存器 */
    if (do_status) {
        uint16_t status;
        if (sht30_read_status(fd, &status) == 0) {
            print_status(status);
        } else {
            fprintf(stderr, "读取状态寄存器失败\n");
        }
    }

    /* 加热器 */
    if (do_heater_on) {
        printf("正在开启加热器...\n");
        if (sht30_send_cmd(fd, CMD_HEATER_ON) == 0) {
            printf("加热器已开启 (用于除湿/自检，注意功耗)\n");
        }
    }

    if (do_heater_off) {
        printf("正在关闭加热器...\n");
        if (sht30_send_cmd(fd, CMD_HEATER_OFF) == 0) {
            printf("加热器已关闭\n");
        }
    }

    /* ---- 读取温湿度数据 ---- */

    /* 如果只执行了特殊命令 (无数据读取)，则直接退出 */
    if (do_status || do_reset || do_heater_on || do_heater_off) {
        if (!do_once && mode == DEFAULT_MODE) {
            /* 用户仅执行了特殊命令 */
            /* 但如果也指定了 -1 或 -n，则还需读取数据 */
            if (!(do_once || count > 1)) {
                close(fd);
                return 0;
            }
        }
    }

    /* 执行数据读取 */
    if (do_once || is_periodic(mode)) {
        int i;

        if (is_periodic(mode)) {
            /* 周期模式：先启动 */
            if (sht30_send_cmd(fd, mode) < 0) {
                fprintf(stderr, "启动周期测量失败\n");
                close(fd);
                return 1;
            }
            usleep(20000);
        }

        for (i = 0; i < count && g_running; i++) {
            float temp, hum, dp;

            if (read_once(fd, mode, &temp, &hum) < 0) {
                if (is_periodic(mode)) sht30_send_cmd(fd, CMD_BREAK);
                close(fd);
                return 1;
            }

            dp = dew_point(temp, hum);

            if (g_verbose) {
                printf("\n--- 第 %d 次读取 ---\n", i + 1);
                printf("  温度: %.2f °C  (%s)\n", temp, temp_comfort(temp));
                printf("  湿度: %.1f %%RH  (%s)\n", hum, hum_comfort(hum));
                printf("  露点: %.2f °C\n", dp);
            } else {
                printf("温度: %.2f °C  |  湿度: %.1f %%  |  露点: %.2f °C  |  %s/%s\n",
                       temp, hum, dp,
                       temp_comfort(temp), hum_comfort(hum));
            }

            if (i < count - 1 && g_running) {
                usleep(interval_ms * 1000);
            }
        }

        /* 停止周期测量 */
        if (is_periodic(mode)) {
            sht30_send_cmd(fd, CMD_BREAK);
        }
    } else {
        read_continuous(fd, mode, interval_ms);
    }

    close(fd);
    printf("\nDone.\n");

    return 0;
}
