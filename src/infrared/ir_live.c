#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define TCA_INPUT0 0x00
#define TCA_INPUT1 0x01
#define TCA_CONFIG0 0x06
#define TCA_CONFIG1 0x07

typedef struct {
    int bus;
    int addr;
    int ser;
    int rclk;
    int srclk;
    int interval_ms;
    int settle_us;
    int stable_frames;
    int loops;
    int raw;
    double cm_start;
    double cm_end;
} cfg_t;

static volatile sig_atomic_t running = 1;

static void on_signal(int sig)
{
    (void)sig;
    running = 0;
}

static uint64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ull + (uint64_t)tv.tv_usec / 1000ull;
}

static void usage(const char *name)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  -b BUS       I2C bus, default 2 (/dev/i2c-2)\n"
            "  -a ADDR      TCA9555 address, default 0x20\n"
            "  -i MS        scan interval, default 50 ms\n"
            "  -s US        LED settle time, default 3000 us\n"
            "  -f N         stable frames threshold, default 3\n"
            "  -n N         number of scans, default 0 forever\n"
            "  -r           print raw TCA9555 bytes for each group\n"
            "  --cm0 CM     board start cm, default 19.0\n"
            "  --cm1 CM     board end cm, default 38.0\n"
            "  --ser N      SER gpio, default 32\n"
            "  --rclk N     RCLK gpio, default 33\n"
            "  --srclk N    SRCLK gpio, default 34\n",
            name);
}

static int parse_int(const char *s, int *out)
{
    char *end = NULL;
    long v = strtol(s, &end, 0);
    if (!s[0] || (end && *end)) return -1;
    *out = (int)v;
    return 0;
}

static int parse_double(const char *s, double *out)
{
    char *end = NULL;
    double v = strtod(s, &end);
    if (!s[0] || (end && *end)) return -1;
    *out = v;
    return 0;
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

static int gpio_path(int gpio, const char *leaf, char *buf, size_t len)
{
    int n = snprintf(buf, len, "/sys/class/gpio/gpio%d/%s", gpio, leaf);
    return n > 0 && (size_t)n < len ? 0 : -1;
}

static int gpio_export_out(int gpio)
{
    char path[128];
    char num[32];
    if (gpio_path(gpio, "value", path, sizeof(path)) < 0) return -1;
    if (access(path, F_OK) != 0) {
        snprintf(num, sizeof(num), "%d", gpio);
        if (write_text("/sys/class/gpio/export", num) < 0 && errno != EBUSY)
            return -1;
        for (int i = 0; i < 40 && access(path, F_OK) != 0; i++)
            usleep(10000);
    }
    if (gpio_path(gpio, "direction", path, sizeof(path)) < 0) return -1;
    return write_text(path, "out");
}

static int gpio_write(int gpio, int value)
{
    char path[128];
    if (gpio_path(gpio, "value", path, sizeof(path)) < 0) return -1;
    return write_text(path, value ? "1" : "0");
}

static void pulse(int gpio)
{
    gpio_write(gpio, 1);
    usleep(50);
    gpio_write(gpio, 0);
    usleep(50);
}

static void shift_byte(const cfg_t *cfg, uint8_t value)
{
    for (int bit = 7; bit >= 0; bit--) {
        gpio_write(cfg->ser, (value >> bit) & 1);
        pulse(cfg->srclk);
    }
    pulse(cfg->rclk);
}

static int i2c_write_reg(int fd, uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return write(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf) ? 0 : -1;
}

static int i2c_read_reg(int fd, uint8_t reg, uint8_t *value)
{
    if (write(fd, &reg, 1) != 1) return -1;
    return read(fd, value, 1) == 1 ? 0 : -1;
}

static int scan_once(const cfg_t *cfg, int fd, uint16_t *bitmap, uint8_t raw[8][2])
{
    uint16_t out = 0;
    for (int group = 0; group < 8; group++) {
        uint8_t in0 = 0xff, in1 = 0xff;
        shift_byte(cfg, (uint8_t)(1u << group));
        usleep((useconds_t)cfg->settle_us);

        if (i2c_read_reg(fd, TCA_INPUT0, &in0) < 0 ||
            i2c_read_reg(fd, TCA_INPUT1, &in1) < 0) {
            shift_byte(cfg, 0x00);
            return -1;
        }

        raw[group][0] = in0;
        raw[group][1] = in1;
        if (((in0 >> group) & 1) == 0) out |= (uint16_t)(1u << group);
        if (((in1 >> group) & 1) == 0) out |= (uint16_t)(1u << (group + 8));

        shift_byte(cfg, 0x00);
        usleep(500);
    }
    *bitmap = out;
    return 0;
}

static void bitmap_text(uint16_t bm, char out[17])
{
    for (int i = 0; i < 16; i++) out[i] = (bm & (1u << i)) ? '#' : '.';
    out[16] = '\0';
}

static void print_ranges(const cfg_t *cfg, uint16_t bits)
{
    double step = (cfg->cm_end - cfg->cm_start) / 16.0;
    int open = 0;
    int start = 0;
    for (int ch = 0; ch <= 16; ch++) {
        int set = ch < 16 && (bits & (1u << ch));
        if (set && !open) {
            open = 1;
            start = ch;
        } else if (!set && open) {
            double s = cfg->cm_start + step * start;
            double e = cfg->cm_start + step * ch;
            printf(" ch%02d-%02d %.1f-%.1fcm", start, ch - 1, s, e);
            open = 0;
        }
    }
}

int main(int argc, char **argv)
{
    cfg_t cfg = {
        .bus = 2,
        .addr = 0x20,
        .ser = 32,
        .rclk = 33,
        .srclk = 34,
        .interval_ms = 50,
        .settle_us = 3000,
        .stable_frames = 3,
        .loops = 0,
        .raw = 0,
        .cm_start = 19.0,
        .cm_end = 38.0,
    };

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-b") && i + 1 < argc) parse_int(argv[++i], &cfg.bus);
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) parse_int(argv[++i], &cfg.addr);
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) parse_int(argv[++i], &cfg.interval_ms);
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) parse_int(argv[++i], &cfg.settle_us);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) parse_int(argv[++i], &cfg.stable_frames);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) parse_int(argv[++i], &cfg.loops);
        else if (!strcmp(argv[i], "-r")) cfg.raw = 1;
        else if (!strcmp(argv[i], "--cm0") && i + 1 < argc) parse_double(argv[++i], &cfg.cm_start);
        else if (!strcmp(argv[i], "--cm1") && i + 1 < argc) parse_double(argv[++i], &cfg.cm_end);
        else if (!strcmp(argv[i], "--ser") && i + 1 < argc) parse_int(argv[++i], &cfg.ser);
        else if (!strcmp(argv[i], "--rclk") && i + 1 < argc) parse_int(argv[++i], &cfg.rclk);
        else if (!strcmp(argv[i], "--srclk") && i + 1 < argc) parse_int(argv[++i], &cfg.srclk);
        else {
            usage(argv[0]);
            return 2;
        }
    }

    if (cfg.interval_ms < 10) cfg.interval_ms = 10;
    if (cfg.stable_frames < 1) cfg.stable_frames = 1;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (gpio_export_out(cfg.ser) < 0 || gpio_export_out(cfg.rclk) < 0 ||
        gpio_export_out(cfg.srclk) < 0) {
        fprintf(stderr, "GPIO init failed: %s\n", strerror(errno));
        return 1;
    }
    gpio_write(cfg.ser, 0);
    gpio_write(cfg.rclk, 0);
    gpio_write(cfg.srclk, 0);
    shift_byte(&cfg, 0x00);

    char dev[64];
    snprintf(dev, sizeof(dev), "/dev/i2c-%d", cfg.bus);
    int fd = open(dev, O_RDWR);
    if (fd < 0) {
        perror(dev);
        return 1;
    }
    if (ioctl(fd, I2C_SLAVE, cfg.addr) < 0) {
        perror("I2C_SLAVE");
        close(fd);
        return 1;
    }
    if (i2c_write_reg(fd, TCA_CONFIG0, 0xff) < 0 ||
        i2c_write_reg(fd, TCA_CONFIG1, 0xff) < 0) {
        perror("configure TCA9555");
        close(fd);
        return 1;
    }

    printf("IR live monitor: /dev/i2c-%d addr=0x%02x interval=%dms settle=%dus stable=%d frames\n",
           cfg.bus, cfg.addr, cfg.interval_ms, cfg.settle_us, cfg.stable_frames);
    printf("GPIO SER=%d RCLK=%d SRCLK=%d, cm %.1f-%.1f, #=blocked/reflection .=open\n",
           cfg.ser, cfg.rclk, cfg.srclk, cfg.cm_start, cfg.cm_end);
    printf("Columns: t_ms scan raw_hex raw_bitmap stable_hex stable_bitmap streak event\n\n");
    fflush(stdout);

    uint16_t raw_bm = 0, last_raw = 0xffff, stable = 0xffff;
    int streak = 0;
    int scan = 0;
    uint64_t start_ms = now_ms();
    uint8_t raw[8][2];

    while (running && (cfg.loops == 0 || scan < cfg.loops)) {
        if (scan_once(&cfg, fd, &raw_bm, raw) < 0) {
            perror("scan");
            break;
        }

        if (raw_bm == last_raw) streak++;
        else {
            last_raw = raw_bm;
            streak = 1;
        }

        uint16_t old_stable = stable;
        if (streak >= cfg.stable_frames) stable = raw_bm;

        char raw_txt[17], stable_txt[17];
        bitmap_text(raw_bm, raw_txt);
        bitmap_text(stable, stable_txt);
        printf("%6llu %5d raw=%04x %-16s stable=%04x %-16s streak=%d",
               (unsigned long long)(now_ms() - start_ms), scan, raw_bm, raw_txt,
               stable, stable_txt, streak);

        if (stable != old_stable) {
            uint16_t removed = old_stable & (uint16_t)~stable;
            uint16_t added = stable & (uint16_t)~old_stable;
            if (removed) {
                printf("  REMOVED");
                print_ranges(&cfg, removed);
            }
            if (added) {
                printf("  ADDED");
                print_ranges(&cfg, added);
            }
        }
        putchar('\n');

        if (cfg.raw) {
            for (int g = 0; g < 8; g++)
                printf("       grp%d in0=%02x in1=%02x\n", g, raw[g][0], raw[g][1]);
        }
        fflush(stdout);

        scan++;
        usleep((useconds_t)cfg.interval_ms * 1000u);
    }

    shift_byte(&cfg, 0x00);
    close(fd);
    return 0;
}
