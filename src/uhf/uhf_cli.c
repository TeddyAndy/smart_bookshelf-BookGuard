/**
 * UHF RFID 调试工具
 *
 * 用法:
 *   uhf_cli -u /dev/ttyS1 -p 29 -i         设备信息
 *   uhf_cli -u /dev/ttyS1 -p 29 -t 5       扫 5 秒
 *   uhf_cli -u /dev/ttyS1 -p 29 -c         持续扫 (Ctrl+C 退出)
 *   uhf_cli -u /dev/ttyS1 -p 29 -n 10      扫到 10 个标签停止
 */
#include "uhf_driver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/time.h>

static volatile int running = 1;
static void on_signal(int s) { (void)s; running = 0; }

static double now_sec(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

static void usage(const char *p) {
    printf("Usage: %s [options] <device>\n", p);
    printf("  -u <dev>  设备路径 (/dev/ttyS1)\n");
    printf("  -p <dBm>  功率 (10-33, default 26)\n");
    printf("  -i         查询设备信息\n");
    printf("  -t <sec>   定时扫描 (秒)\n");
    printf("  -n <num>   扫到 N 个标签后停止\n");
    printf("  -c         持续扫描\n");
    printf("  -v         verbose\n");
}

static void print_tags(uhf_t *u) {
    for (int i = 0; i < u->tag_count; i++) {
        uhf_tag_t *t = &u->tags[i];
        printf("  %-36s RSSI=%-3u reads=%-4d age=%lds\n",
               t->epc_str, t->rssi, t->read_count,
               (long)(time(NULL) - t->last_seen));
    }
}

int main(int argc, char *argv[]) {
    const char *dev = "/dev/ttyS1";
    int power = 26, do_info = 0, do_timed = 0, do_count = 0, do_cont = 0;
    int timed_sec = 0, count_n = 0, verbose = 0;
    int opt;

    while ((opt = getopt(argc, argv, "u:p:t:n:civh")) != -1) {
        switch (opt) {
        case 'u': dev = optarg; break;
        case 'p': power = atoi(optarg); break;
        case 't': do_timed = 1; timed_sec = atoi(optarg); break;
        case 'n': do_count = 1; count_n = atoi(optarg); break;
        case 'c': do_cont = 1; break;
        case 'i': do_info = 1; break;
        case 'v': verbose = 1; break;
        default: usage(argv[0]); return 1;
        }
    }

    /* init */
    uhf_t u;
    if (uhf_init(&u, dev) < 0) {
        fprintf(stderr, "FAIL: uhf_init %s\n", dev);
        return 1;
    }
    if (verbose) uhf_verbose(&u, 1);

    printf("FW v%d.%d.%d  Power=%ddBm\n",
           u.fw_ver[0], u.fw_ver[1], u.fw_ver[2], u.power);

    if (do_info) {
        uhf_deinit(&u);
        return 0;
    }

    /* 设功率 */
    if (power != (int)u.power) {
        if (uhf_set_power(&u, (uint8_t)power) < 0) {
            fprintf(stderr, "FAIL: set_power\n");
            uhf_deinit(&u); return 1;
        }
    }
    printf("Power: %d dBm\n", power);

    /* 启动盘存 */
    if (uhf_inventory_start(&u) < 0) {
        fprintf(stderr, "FAIL: inventory_start\n");
        uhf_deinit(&u); return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    double t0 = now_sec();

    /* ── 持续扫描 ── */
    if (do_cont) {
        printf("Continuous scan (Ctrl+C to stop)...\n\n");
        int tick = 0;
        while (running) {
            uhf_poll(&u, 50);
            tick++;
            if (tick % 40 == 0 && u.tag_count > 0) {  /* 每 2s */
                printf("── %d tags (%.1fs) ──\n", u.tag_count, now_sec() - t0);
                print_tags(&u);
            }
        }
    }
    /* ── 定时扫描 ── */
    else if (do_timed) {
        printf("Scanning for %ds...\n", timed_sec);
        while (running && (now_sec() - t0) < timed_sec) {
            uhf_poll(&u, 20);
        }
    }
    /* ── 计数扫描 ── */
    else if (do_count) {
        printf("Scanning until %d tags...\n", count_n);
        while (running && u.tag_count < count_n) {
            uhf_poll(&u, 20);
        }
    }
    /* ── 默认: 3 秒 ── */
    else {
        printf("Scanning for 3s...\n");
        while (running && (now_sec() - t0) < 3.0) {
            uhf_poll(&u, 20);
        }
    }

    double elapsed = now_sec() - t0;

    /* 停止 + 清理 */
    uhf_inventory_stop(&u);
    uhf_deinit(&u);

    /* 结果 */
    printf("\n══ %d unique tags in %.2fs ══\n", u.tag_count, elapsed);
    print_tags(&u);

    if (u.tag_count > 0) {
        printf("\nEPC list:\n");
        for (int i = 0; i < u.tag_count; i++)
            printf("%s\n", u.tags[i].epc_str);
    }

    return 0;
}
