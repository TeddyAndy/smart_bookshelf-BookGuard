#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define TCA_REG_INPUT0 0x00
#define TCA_REG_INPUT1 0x01
#define TCA_REG_CONFIG0 0x06
#define TCA_REG_CONFIG1 0x07

typedef struct {
    int i2c_bus;
    int i2c_addr;
    int gpio_ser;
    int gpio_rclk;
    int gpio_srclk;
    int settle_us;
    int refresh_ms;
    int loops;
    int raw;
    int no_clear;
} config_t;

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "Options:\n"
            "  -b BUS       I2C bus number, default 0 (/dev/i2c-0)\n"
            "  -a ADDR      TCA9555 7-bit address, default 0x20\n"
            "  -n LOOPS     Number of scans, default 0 means forever\n"
            "  -d MS        Refresh interval, default 100 ms\n"
            "  -s US        Settle time after LED group on, default 3000 us\n"
            "  -r           Show raw TCA9555 bytes per group\n"
            "  -C           Do not clear terminal between scans\n"
            "  --ser N      SER gpio number, default 32\n"
            "  --rclk N     RCLK gpio number, default 33\n"
            "  --srclk N    SRCLK gpio number, default 34\n",
            argv0);
}

static int write_text(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0) return -1;
    ssize_t len = (ssize_t)strlen(text);
    ssize_t n = write(fd, text, len);
    close(fd);
    return n == len ? 0 : -1;
}

static int gpio_path(int gpio, const char *name, char *buf, size_t len)
{
    int n = snprintf(buf, len, "/sys/class/gpio/gpio%d/%s", gpio, name);
    return (n > 0 && (size_t)n < len) ? 0 : -1;
}

static int gpio_export(int gpio)
{
    char path[128];
    if (gpio_path(gpio, "value", path, sizeof(path)) == 0 && access(path, F_OK) == 0)
        return 0;

    char num[32];
    snprintf(num, sizeof(num), "%d", gpio);
    if (write_text("/sys/class/gpio/export", num) < 0 && errno != EBUSY) {
        perror("gpio export");
        return -1;
    }

    for (int i = 0; i < 20; i++) {
        if (access(path, F_OK) == 0) return 0;
        usleep(10000);
    }
    fprintf(stderr, "gpio%d did not appear in sysfs\n", gpio);
    return -1;
}

static int gpio_set_direction(int gpio, const char *direction)
{
    char path[128];
    if (gpio_path(gpio, "direction", path, sizeof(path)) < 0) return -1;
    if (write_text(path, direction) < 0) {
        fprintf(stderr, "gpio%d direction %s failed: %s\n", gpio, direction, strerror(errno));
        return -1;
    }
    return 0;
}

static int gpio_write(int gpio, int value)
{
    char path[128];
    if (gpio_path(gpio, "value", path, sizeof(path)) < 0) return -1;
    if (write_text(path, value ? "1" : "0") < 0) {
        fprintf(stderr, "gpio%d write failed: %s\n", gpio, strerror(errno));
        return -1;
    }
    return 0;
}

static void pulse(int gpio)
{
    gpio_write(gpio, 1);
    usleep(500);
    gpio_write(gpio, 0);
    usleep(500);
}

static void shift_byte(const config_t *cfg, uint8_t value)
{
    for (int bit = 7; bit >= 0; bit--) {
        gpio_write(cfg->gpio_ser, (value >> bit) & 1);
        pulse(cfg->gpio_srclk);
    }
    pulse(cfg->gpio_rclk);
}

static int i2c_write_reg(int fd, uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return write(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf) ? 0 : -1;
}

static int i2c_read_reg(int fd, uint8_t reg, uint8_t *value)
{
    if (write(fd, &reg, 1) != 1) return -1;
    if (read(fd, value, 1) != 1) return -1;
    return 0;
}

static int scan_once(const config_t *cfg, int fd, int detected[16], uint8_t raw[8][2])
{
    for (int i = 0; i < 16; i++) detected[i] = 0;

    for (int group = 0; group < 8; group++) {
        uint8_t in0 = 0xff;
        uint8_t in1 = 0xff;

        shift_byte(cfg, (uint8_t)(1u << group));
        usleep(cfg->settle_us);

        if (i2c_read_reg(fd, TCA_REG_INPUT0, &in0) < 0 ||
            i2c_read_reg(fd, TCA_REG_INPUT1, &in1) < 0) {
            shift_byte(cfg, 0x00);
            return -1;
        }

        raw[group][0] = in0;
        raw[group][1] = in1;
        detected[group] = ((in0 >> group) & 1) ? 0 : 1;
        detected[group + 8] = ((in1 >> group) & 1) ? 0 : 1;

        shift_byte(cfg, 0x00);
        usleep(1000);
    }
    return 0;
}

static void print_scan(const config_t *cfg, int scan_no, const int detected[16], uint8_t raw[8][2])
{
    if (!cfg->no_clear) printf("\033[2J\033[H");

    printf("Infrared PCB monitor  scan=%d  I2C=/dev/i2c-%d addr=0x%02x\n",
           scan_no, cfg->i2c_bus, cfg->i2c_addr);
    printf("GPIO: SER=%d RCLK=%d SRCLK=%d | #=blocked/reflection .=open\n\n",
           cfg->gpio_ser, cfg->gpio_rclk, cfg->gpio_srclk);

    for (int row = 0; row < 2; row++) {
        int start = row ? 8 : 0;
        printf("S%02d-S%02d: ", start, start + 7);
        for (int i = 0; i < 8; i++) {
            int ch = start + i;
            printf("S%02d=%c ", ch, detected[ch] ? '#' : '.');
        }
        printf("\n");
    }

    printf("\nBitmap: ");
    for (int i = 0; i < 16; i++) putchar(detected[i] ? '#' : '.');
    printf("\n");

    if (cfg->raw) {
        printf("\nRaw per active group:\n");
        for (int g = 0; g < 8; g++) {
            printf("  grp%d: in0=0x%02x in1=0x%02x  D%d=%d D%d=%d\n",
                   g, raw[g][0], raw[g][1],
                   g, detected[g], g + 8, detected[g + 8]);
        }
    }

    fflush(stdout);
}

static int parse_int(const char *text, int *out)
{
    char *end = NULL;
    long value = strtol(text, &end, 0);
    if (!text[0] || (end && *end)) return -1;
    *out = (int)value;
    return 0;
}

int main(int argc, char **argv)
{
    config_t cfg = {
        .i2c_bus = 0,
        .i2c_addr = 0x20,
        .gpio_ser = 32,
        .gpio_rclk = 33,
        .gpio_srclk = 34,
        .settle_us = 3000,
        .refresh_ms = 100,
        .loops = 0,
        .raw = 0,
        .no_clear = 0,
    };

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-b") && i + 1 < argc) parse_int(argv[++i], &cfg.i2c_bus);
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) parse_int(argv[++i], &cfg.i2c_addr);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) parse_int(argv[++i], &cfg.loops);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) parse_int(argv[++i], &cfg.refresh_ms);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) parse_int(argv[++i], &cfg.settle_us);
        else if (!strcmp(argv[i], "-r")) cfg.raw = 1;
        else if (!strcmp(argv[i], "-C")) cfg.no_clear = 1;
        else if (!strcmp(argv[i], "--ser") && i + 1 < argc) parse_int(argv[++i], &cfg.gpio_ser);
        else if (!strcmp(argv[i], "--rclk") && i + 1 < argc) parse_int(argv[++i], &cfg.gpio_rclk);
        else if (!strcmp(argv[i], "--srclk") && i + 1 < argc) parse_int(argv[++i], &cfg.gpio_srclk);
        else {
            usage(argv[0]);
            return 2;
        }
    }

    if (gpio_export(cfg.gpio_ser) < 0 || gpio_export(cfg.gpio_rclk) < 0 ||
        gpio_export(cfg.gpio_srclk) < 0)
        return 1;

    if (gpio_set_direction(cfg.gpio_ser, "out") < 0 ||
        gpio_set_direction(cfg.gpio_rclk, "out") < 0 ||
        gpio_set_direction(cfg.gpio_srclk, "out") < 0)
        return 1;

    gpio_write(cfg.gpio_ser, 0);
    gpio_write(cfg.gpio_rclk, 0);
    gpio_write(cfg.gpio_srclk, 0);
    shift_byte(&cfg, 0x00);

    char dev[64];
    snprintf(dev, sizeof(dev), "/dev/i2c-%d", cfg.i2c_bus);
    int fd = open(dev, O_RDWR);
    if (fd < 0) {
        perror(dev);
        return 1;
    }

    if (ioctl(fd, I2C_SLAVE, cfg.i2c_addr) < 0) {
        perror("I2C_SLAVE");
        close(fd);
        return 1;
    }

    if (i2c_write_reg(fd, TCA_REG_CONFIG0, 0xff) < 0 ||
        i2c_write_reg(fd, TCA_REG_CONFIG1, 0xff) < 0) {
        perror("configure TCA9555 inputs");
        close(fd);
        return 1;
    }

    int detected[16];
    uint8_t raw[8][2];
    int scan_no = 0;

    while (cfg.loops == 0 || scan_no < cfg.loops) {
        if (scan_once(&cfg, fd, detected, raw) < 0) {
            perror("scan");
            shift_byte(&cfg, 0x00);
            close(fd);
            return 1;
        }

        print_scan(&cfg, scan_no, detected, raw);
        scan_no++;
        usleep((useconds_t)cfg.refresh_ms * 1000u);
    }

    shift_byte(&cfg, 0x00);
    close(fd);
    return 0;
}
