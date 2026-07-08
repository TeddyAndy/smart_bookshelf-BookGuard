/**
 * BH1750FVI 环境光传感器 I2C 测试程序
 *
 * 功能：
 * - 单次/连续读取光照强度 (lux)
 * - 支持多种分辨率模式
 * - 可调测量间隔
 *
 * 编译：arm-none-linux-gnueabihf-gcc bh1750_test.c -o bh1750_test
 * 运行：./bh1750_test /dev/i2c-2
 *
 * 硬件连接:
 *   P9 Pin3 (I2C2_SDA) → BH1750 SDA
 *   P9 Pin5 (I2C2_SCL) → BH1750 SCL
 *   P9 Pin1 (VCC_3V3)   → BH1750 VCC
 *   P9 Pin6 (GND)       → BH1750 GND
 *   I2C 地址: 0x23 (ADDR 引脚接地)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

/* BH1750 命令码 */
#define CMD_POWER_DOWN          0x00
#define CMD_POWER_ON            0x01
#define CMD_RESET               0x07

#define CMD_CONT_H_RES          0x10   /* 连续高分辨率 (1 lx, 120ms) */
#define CMD_CONT_H_RES2         0x11   /* 连续高分辨率2 (0.5 lx, 120ms) */
#define CMD_CONT_L_RES          0x13   /* 连续低分辨率 (4 lx, 16ms) */

#define CMD_ONCE_H_RES          0x20   /* 单次高分辨率 (1 lx, 120ms) */
#define CMD_ONCE_H_RES2         0x21   /* 单次高分辨率2 (0.5 lx, 120ms) */
#define CMD_ONCE_L_RES          0x23   /* 单次低分辨率 (4 lx, 16ms) */

#define DEFAULT_MODE            CMD_CONT_H_RES
#define DEFAULT_INTERVAL        500    /* ms */
#define DEFAULT_I2C_ADDR        0x23

/* 全局变量 */
static int g_running = 1;
static int g_verbose = 0;

/**
 * 模式名称
 */
static const char *mode_name(unsigned char cmd)
{
    switch (cmd) {
        case CMD_CONT_H_RES:   return "连续高分辨率 (1 lx)";
        case CMD_CONT_H_RES2:  return "连续高分辨率2 (0.5 lx)";
        case CMD_CONT_L_RES:   return "连续低分辨率 (4 lx)";
        case CMD_ONCE_H_RES:   return "单次高分辨率 (1 lx)";
        case CMD_ONCE_H_RES2:  return "单次高分辨率2 (0.5 lx)";
        case CMD_ONCE_L_RES:   return "单次低分辨率 (4 lx)";
        default:               return "未知";
    }
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

/**
 * 发送 I2C 命令 (单字节写)
 */
static int bh1750_send_cmd(int fd, unsigned char cmd)
{
    if (write(fd, &cmd, 1) != 1) {
        perror("I2C write");
        return -1;
    }
    return 0;
}

/**
 * 读取光照数据 (2 字节)
 */
static int bh1750_read(int fd, float *lux)
{
    unsigned char buf[2];
    unsigned short raw;

    if (read(fd, buf, 2) != 2) {
        perror("I2C read");
        return -1;
    }

    /* BH1750 返回 MSB 在前 */
    raw = (buf[0] << 8) | buf[1];

    /* 转换为 lux: raw / 1.2 */
    *lux = (float)raw / 1.2f;

    return 0;
}

/**
 * 打开 I2C 设备
 */
static int bh1750_open(const char *device, unsigned char addr)
{
    int fd;

    fd = open(device, O_RDWR);
    if (fd < 0) {
        perror("open I2C device");
        return -1;
    }

    /* 设置 I2C 从设备地址 */
    if (ioctl(fd, I2C_SLAVE, addr) < 0) {
        perror("ioctl I2C_SLAVE");
        close(fd);
        return -1;
    }

    return fd;
}

/**
 * 初始化 BH1750
 */
static int bh1750_init(int fd, unsigned char mode)
{
    /* 上电 */
    if (bh1750_send_cmd(fd, CMD_POWER_ON) < 0) {
        fprintf(stderr, "Failed to power on\n");
        return -1;
    }
    usleep(10000);  /* 10ms */

    /* 设置测量模式 */
    if (bh1750_send_cmd(fd, mode) < 0) {
        fprintf(stderr, "Failed to set mode\n");
        return -1;
    }

    /* 等待首次测量完成 */
    if (mode == CMD_CONT_L_RES || mode == CMD_ONCE_L_RES) {
        usleep(20000);   /* 低分辨率 16ms, 取整 20ms */
    } else {
        usleep(180000);  /* 高分辨率 120ms, 稍大取 180ms */
    }

    return 0;
}

/**
 * 连续读取
 */
static int read_continuous(int fd, unsigned char mode, int interval_ms)
{
    float lux;

    if (bh1750_init(fd, mode) < 0) {
        return -1;
    }

    printf("模式: %s\n", mode_name(mode));
    printf("间隔: %d ms\n", interval_ms);
    printf("按 Ctrl+C 停止...\n\n");

    /* 表头 */
    printf("%-10s %10s %s\n", "时间", "光照(lx)", "柱状图");
    printf("----------------------------------------\n");

    while (g_running) {
        char time_str[16];
        int bar_len, i;

        if (bh1750_read(fd, &lux) < 0) {
            usleep(interval_ms * 1000);
            continue;
        }

        get_time_str(time_str, sizeof(time_str));

        /* 柱状图 (每字符约 50 lx, 最大 40 字符 = 2000 lx) */
        bar_len = (int)(lux / 50.0f);
        if (bar_len > 40) bar_len = 40;
        if (bar_len < 0)  bar_len = 0;

        printf("%-10s %9.1f  ", time_str, lux);
        for (i = 0; i < bar_len; i++) {
            putchar('#');
        }
        putchar('\n');

        fflush(stdout);
        usleep(interval_ms * 1000);
    }

    /* 恢复 power down */
    bh1750_send_cmd(fd, CMD_POWER_DOWN);

    return 0;
}

/**
 * 环境光等级判断
 */
static const char *lux_level(float lux)
{
    if (lux < 0.3)   return "极暗 (黑夜)";
    if (lux < 10)    return "暗 (黄昏室内)";
    if (lux < 50)    return "较暗 (阴天室内)";
    if (lux < 200)   return "适中 (办公室)";
    if (lux < 500)   return "较亮 (窗边)";
    if (lux < 2000)  return "明亮 (灯光直射)";
    return "很亮 (户外)";
}

/**
 * 打印使用说明
 */
static void print_usage(const char *prog)
{
    printf("Usage: %s [options] <i2c_device>\n\n", prog);
    printf("Options:\n");
    printf("  -v          详细模式\n");
    printf("  -a <addr>   I2C 地址 (默认: 0x%02X)\n", DEFAULT_I2C_ADDR);
    printf("  -m <mode>   测量模式 (默认: cont-h)\n");
    printf("                 cont-h   连续高分辨率 1 lx\n");
    printf("                 cont-h2  连续高分辨率 0.5 lx\n");
    printf("                 cont-l   连续低分辨率 4 lx\n");
    printf("                 once-h   单次高分辨率 1 lx\n");
    printf("                 once-h2  单次高分辨率 0.5 lx\n");
    printf("                 once-l   单次低分辨率 4 lx\n");
    printf("  -n <count>  读取次数 (单次模式, 默认: 1)\n");
    printf("  -i <ms>     连续读取间隔 (毫秒, 默认: %d)\n", DEFAULT_INTERVAL);
    printf("  -1          单次读取 (等同于 -m once-h)\n");
    printf("\nExamples:\n");
    printf("  %s -1 /dev/i2c-2              单次高精度读取\n", prog);
    printf("  %s /dev/i2c-2                  连续读取 (默认)\n", prog);
    printf("  %s -i 2000 /dev/i2c-2          每2秒读取一次\n", prog);
    printf("  %s -m once-h2 /dev/i2c-2       单次超高精度\n", prog);
}

int main(int argc, char *argv[])
{
    const char *device = NULL;
    unsigned char i2c_addr = DEFAULT_I2C_ADDR;
    unsigned char mode = DEFAULT_MODE;
    int interval_ms = DEFAULT_INTERVAL;
    int do_once = 0;
    int count = 1;
    int fd;
    int opt;

    /* 解析参数 */
    while ((opt = getopt(argc, argv, "va:m:n:i:1h?")) != -1) {
        switch (opt) {
            case 'v':
                g_verbose = 1;
                break;
            case 'a':
                i2c_addr = (unsigned char)strtol(optarg, NULL, 0);
                break;
            case 'm':
                if (strcmp(optarg, "cont-h") == 0) {
                    mode = CMD_CONT_H_RES;
                } else if (strcmp(optarg, "cont-h2") == 0) {
                    mode = CMD_CONT_H_RES2;
                } else if (strcmp(optarg, "cont-l") == 0) {
                    mode = CMD_CONT_L_RES;
                } else if (strcmp(optarg, "once-h") == 0) {
                    mode = CMD_ONCE_H_RES;
                    do_once = 1;
                } else if (strcmp(optarg, "once-h2") == 0) {
                    mode = CMD_ONCE_H_RES2;
                    do_once = 1;
                } else if (strcmp(optarg, "once-l") == 0) {
                    mode = CMD_ONCE_L_RES;
                    do_once = 1;
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
                if (interval_ms < 10) interval_ms = 10;
                break;
            case '1':
                mode = CMD_ONCE_H_RES;
                do_once = 1;
                break;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "Error: No I2C device specified\n\n");
        print_usage(argv[0]);
        return 1;
    }

    device = argv[optind];

    /* 打开 I2C 设备 */
    if (g_verbose) {
        printf("打开 %s, I2C 地址 0x%02X...\n", device, i2c_addr);
    }

    fd = bh1750_open(device, i2c_addr);
    if (fd < 0) {
        fprintf(stderr, "Failed to open %s at address 0x%02X\n",
                device, i2c_addr);
        fprintf(stderr, "检查: 1) 接线  2) 设备是否存在 (ls %s)\n", device);
        fprintf(stderr, "      3) i2cdetect -y -r <bus> 扫描地址\n");
        return 1;
    }

    if (g_verbose) {
        printf("I2C 设备已打开\n");
    }

    /* 设置信号处理 */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* 执行读取 */
    if (do_once) {
        int i;

        for (i = 0; i < count && g_running; i++) {
            float lux;

            if (bh1750_init(fd, mode) < 0) {
                close(fd);
                return 1;
            }

            if (bh1750_read(fd, &lux) < 0) {
                close(fd);
                return 1;
            }

            printf("光照强度: %.1f lx  [%s]\n", lux, lux_level(lux));

            if (i < count - 1 && g_running) {
                usleep(interval_ms * 1000);
            }
        }
    } else {
        read_continuous(fd, mode, interval_ms);
    }

    close(fd);
    printf("\nDone.\n");

    return 0;
}
