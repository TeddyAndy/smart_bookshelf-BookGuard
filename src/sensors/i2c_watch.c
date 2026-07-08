#include <errno.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_BUS 2
#define DEFAULT_INTERVAL_MS 1000
#define DEFAULT_TIMEOUT_US 200000

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

static void on_alarm(int sig)
{
    (void)sig;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "Options:\n"
            "  -b BUS       I2C bus number, default %d (/dev/i2c-%d)\n"
            "  -i MS        scan interval, default %d ms\n"
            "  -t US        per-address timeout alarm, default %d us\n"
            "  -1           scan once and exit\n"
            "  -q           quiet: only print found devices and errors\n"
            "\n"
            "Known addresses in this project:\n"
            "  0x20  TCA9555 infrared board\n"
            "  0x23  BH1750 light sensor\n"
            "  0x44  SHT30 temp/humidity sensor\n"
            "  0x50/0x58 board/system devices often seen on i2c-2\n",
            argv0, DEFAULT_BUS, DEFAULT_BUS, DEFAULT_INTERVAL_MS, DEFAULT_TIMEOUT_US);
}

static const char *known_name(int addr)
{
    switch (addr) {
    case 0x20: return "TCA9555/infrared";
    case 0x23: return "BH1750/light";
    case 0x44: return "SHT30/temp-humidity";
    case 0x50: return "board/system";
    case 0x58: return "board/system";
    default: return "";
    }
}

static void print_time_prefix(void)
{
    time_t now = time(NULL);
    struct tm tm_now;
    char buf[32];

    localtime_r(&now, &tm_now);
    strftime(buf, sizeof(buf), "%H:%M:%S", &tm_now);
    printf("[%s] ", buf);
}

static int probe_addr(int fd, int addr, int timeout_us, int *err_out)
{
    uint8_t byte = 0;

    if (ioctl(fd, I2C_SLAVE, addr) < 0) {
        *err_out = errno;
        return -1;
    }

    /*
     * i2cdetect -r style probe: read one byte. It avoids device-specific
     * commands, but some devices may still NACK; that is reported clearly.
     */
    ualarm((useconds_t)timeout_us, 0);
    errno = 0;
    if (read(fd, &byte, 1) == 1) {
        ualarm(0, 0);
        *err_out = 0;
        return 1;
    }
    ualarm(0, 0);

    *err_out = errno ? errno : EIO;
    return 0;
}

static int probe_tca9555(int fd, int addr, int timeout_us, int *err_out)
{
    uint8_t reg = 0x00;
    uint8_t byte = 0;

    if (ioctl(fd, I2C_SLAVE, addr) < 0) {
        *err_out = errno;
        return -1;
    }

    ualarm((useconds_t)timeout_us, 0);
    errno = 0;
    if (write(fd, &reg, 1) != 1) {
        ualarm(0, 0);
        *err_out = errno ? errno : EIO;
        return 0;
    }
    if (read(fd, &byte, 1) != 1) {
        ualarm(0, 0);
        *err_out = errno ? errno : EIO;
        return 0;
    }
    ualarm(0, 0);

    *err_out = 0;
    return 1;
}

static int scan_once(const char *dev, int quiet, int timeout_us)
{
    int fd = open(dev, O_RDWR);
    int found = 0;
    int errors = 0;

    if (fd < 0) {
        print_time_prefix();
        printf("OPEN_FAIL %s: %s\n", dev, strerror(errno));
        return -1;
    }

    if (!quiet) {
        print_time_prefix();
        printf("scan %s\n", dev);
    }

    for (int addr = 0x03; addr <= 0x77 && g_running; addr++) {
        int err = 0;
        int rc;
        const char *name = known_name(addr);

        if (addr == 0x20) {
            rc = probe_tca9555(fd, addr, timeout_us, &err);
        } else {
            rc = probe_addr(fd, addr, timeout_us, &err);
        }

        if (rc == 1) {
            found++;
            print_time_prefix();
            printf("FOUND 0x%02x", addr);
            if (name[0]) printf("  %s", name);
            putchar('\n');
        } else if (err == ENXIO || err == ENODEV || err == EREMOTEIO || err == EIO) {
            /*
             * These usually mean NACK/no device. Only print them for project
             * addresses in verbose mode, so the live output stays readable.
             */
            if (!quiet && name[0]) {
                print_time_prefix();
                printf("MISS  0x%02x  %s  %s\n", addr, name, strerror(err));
            }
        } else if (err) {
            errors++;
            print_time_prefix();
            printf("ERR   0x%02x", addr);
            if (name[0]) printf("  %s", name);
            printf("  %s\n", strerror(err));
        }
    }

    print_time_prefix();
    printf("summary: found=%d errors=%d\n", found, errors);
    close(fd);
    return errors ? 1 : 0;
}

int main(int argc, char **argv)
{
    int bus = DEFAULT_BUS;
    int interval_ms = DEFAULT_INTERVAL_MS;
    int timeout_us = DEFAULT_TIMEOUT_US;
    int once = 0;
    int quiet = 0;
    int opt;
    char dev[64];

    while ((opt = getopt(argc, argv, "b:i:t:1qh")) != -1) {
        switch (opt) {
        case 'b':
            bus = atoi(optarg);
            break;
        case 'i':
            interval_ms = atoi(optarg);
            if (interval_ms < 100) interval_ms = 100;
            break;
        case 't':
            timeout_us = atoi(optarg);
            if (timeout_us < 10000) timeout_us = 10000;
            break;
        case '1':
            once = 1;
            break;
        case 'q':
            quiet = 1;
            break;
        case 'h':
        default:
            usage(argv[0]);
            return opt == 'h' ? 0 : 1;
        }
    }

    snprintf(dev, sizeof(dev), "/dev/i2c-%d", bus);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGALRM, on_alarm);

    do {
        scan_once(dev, quiet, timeout_us);
        if (!once && g_running) {
            fflush(stdout);
            usleep((useconds_t)interval_ms * 1000);
        }
    } while (!once && g_running);

    return 0;
}
