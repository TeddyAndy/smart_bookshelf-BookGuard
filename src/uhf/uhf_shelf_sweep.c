/**
 * UHF 书架分层测试工具
 *
 * 目标不是证明零串读，而是统计同一 EPC 在上下层模块的读数差异，
 * 用“哪一层读数明显更多”来判断主读层、串读和未知标签。
 */
#include "uhf_driver.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_UP_DEV "/dev/serial/by-path/platform-ff780000.usb-usb-0:1.2:1.0-port0"
#define DEFAULT_LO_DEV "/dev/serial/by-path/platform-ff780000.usb-usb-0:1.3:1.0-port0"

#define MAX_OBS 256

typedef struct {
    const char *title;
    const char *epc;
    int layer; /* 0=上, 1=下 */
} known_book_t;

typedef struct {
    char epc[UHF_EPC_STR];
    const char *title;
    int expected_layer;
    int up_reads;
    int lo_reads;
    int up_seen_rounds;
    int lo_seen_rounds;
    int up_rssi_sum;
    int lo_rssi_sum;
    int up_rssi_max;
    int lo_rssi_max;
} obs_t;

static const known_book_t KNOWN_BOOKS[] = {
    {"fsf3", "E280691500005029B7E1E88B", 0},
    {"fsf4", "E280691500004029B7E1EC8B", 0},
    {"马原", "E280691500005029B7E1F08B", 0},
    {"复变", "E280691500005029B7E2008B", 0},
    {"标日上", "E280691500004029B7E1F88B", 0},
    {"数电", "E280691500005029B7E1FC8B", 0},
    {"信息论", "E280691500004029B7E1F48B", 0},
    {"概统", "E280691500004029B7E1E08B", 0},
    {"dsp", "E280691500005029B7E1E48B", 0},
    {"电磁场", "E280691500004029B7E1D08B", 0},
    {"线代", "E280691500005029B7E1CC8B", 1},
    {"大物4", "E280691500005029B7E1D48B", 1},
    {"大物3", "E280691500005029B7E1C08B", 1},
    {"模电", "E280691500005029B7E1D88B", 1},
    {"通信原理", "E280691500004029B7E1BC8B", 1},
    {"标日下", "E280691500004029B7E1DC8B", 1},
    {"毛概", "E280691500004029B7E1C88B", 1},
    {"数理方程", "E280691500004029B7E1C48B", 1},
};

static volatile int running = 1;

static void on_signal(int sig)
{
    (void)sig;
    running = 0;
}

static double now_sec(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

static const known_book_t *find_known(const char *epc)
{
    size_t n = sizeof(KNOWN_BOOKS) / sizeof(KNOWN_BOOKS[0]);
    for (size_t i = 0; i < n; i++) {
        if (strcasecmp(KNOWN_BOOKS[i].epc, epc) == 0) return &KNOWN_BOOKS[i];
    }
    return NULL;
}

static const char *layer_name(int layer)
{
    if (layer == 0) return "上";
    if (layer == 1) return "下";
    return "未知";
}

static obs_t *get_obs(obs_t *obs, int *count, const char *epc)
{
    for (int i = 0; i < *count; i++) {
        if (strcasecmp(obs[i].epc, epc) == 0) return &obs[i];
    }
    if (*count >= MAX_OBS) return NULL;

    obs_t *o = &obs[*count];
    memset(o, 0, sizeof(*o));
    snprintf(o->epc, sizeof(o->epc), "%s", epc);

    const known_book_t *k = find_known(epc);
    if (k) {
        o->title = k->title;
        o->expected_layer = k->layer;
    } else {
        o->title = "未知";
        o->expected_layer = -1;
    }

    (*count)++;
    return o;
}

static void init_known_obs(obs_t *obs, int *count)
{
    size_t n = sizeof(KNOWN_BOOKS) / sizeof(KNOWN_BOOKS[0]);
    for (size_t i = 0; i < n; i++) {
        (void)get_obs(obs, count, KNOWN_BOOKS[i].epc);
    }
}

static void add_tag(obs_t *obs, int *count, const uhf_tag_t *tag, int side)
{
    obs_t *o = get_obs(obs, count, tag->epc_str);
    if (!o) return;

    if (side == 0) {
        o->up_reads += tag->read_count;
        o->up_seen_rounds++;
        o->up_rssi_sum += tag->rssi;
        if (tag->rssi > o->up_rssi_max) o->up_rssi_max = tag->rssi;
    } else {
        o->lo_reads += tag->read_count;
        o->lo_seen_rounds++;
        o->lo_rssi_sum += tag->rssi;
        if (tag->rssi > o->lo_rssi_max) o->lo_rssi_max = tag->rssi;
    }
}

static int scan_one(const char *label, const char *dev, int side, int power,
                    int seconds, obs_t *obs, int *obs_count, FILE *log)
{
    uhf_t u;
    memset(&u, 0, sizeof(u));

    printf("[%s] %s %ddBm %ds\n", label, dev, power, seconds);
    if (log) fprintf(log, "- %s: `%s`, %ddBm, %ds\n", label, dev, power, seconds);

    if (uhf_init(&u, dev) < 0) {
        fprintf(stderr, "[FAIL] %s init failed: %s\n", label, dev);
        if (log) fprintf(log, "- **FAIL** %s init failed: `%s`\n", label, dev);
        return -1;
    }

    if (uhf_set_power(&u, (uint8_t)power) < 0) {
        fprintf(stderr, "[FAIL] %s set power failed\n", label);
        if (log) fprintf(log, "- **FAIL** %s set power failed\n", label);
        uhf_deinit(&u);
        return -1;
    }

    uhf_tags_clear(&u);
    if (uhf_inventory_start(&u) < 0) {
        fprintf(stderr, "[FAIL] %s inventory_start failed\n", label);
        if (log) fprintf(log, "- **FAIL** %s inventory_start failed\n", label);
        uhf_deinit(&u);
        return -1;
    }

    double t0 = now_sec();
    while (running && now_sec() - t0 < seconds) {
        uhf_poll(&u, 20);
    }

    uhf_inventory_stop(&u);

    for (int i = 0; i < u.tag_count; i++) {
        add_tag(obs, obs_count, &u.tags[i], side);
    }

    printf("  -> %d EPC\n", u.tag_count);
    uhf_deinit(&u);
    return 0;
}

static void usage(const char *prog)
{
    printf("Usage: %s [options]\n", prog);
    printf("  -U <dev>   上层 UHF 设备, default %s\n", DEFAULT_UP_DEV);
    printf("  -D <dev>   下层 UHF 设备, default %s\n", DEFAULT_LO_DEV);
    printf("  -a <dBm>   起始功率, default 10\n");
    printf("  -b <dBm>   结束功率, default 24\n");
    printf("  -s <step>  功率步进, default 1\n");
    printf("  -r <n>     每档轮数, default 2\n");
    printf("  -t <sec>   每轮扫描秒数, default 3\n");
    printf("  -o <file>  追加 Markdown 结果\n");
    printf("  -h         帮助\n");
}

static void print_power_report(FILE *out, int power, int rounds, int seconds,
                               const obs_t *obs, int count)
{
    int up_ok = 0, lo_ok = 0, unknown = 0, uncertain = 0, cross = 0;

    fprintf(out, "\n### %ddBm\n\n", power);
    fprintf(out, "- 轮数: %d\n", rounds);
    fprintf(out, "- 每轮: 上层 %ds + 下层 %ds\n\n", seconds, seconds);
    fprintf(out, "| 书名 | 预期 | 上读数 | 下读数 | 上轮数 | 下轮数 | 判定 | 置信 | 说明 |\n");
    fprintf(out, "|------|------|-------:|-------:|------:|------:|------|------:|------|\n");

    for (int i = 0; i < count; i++) {
        const obs_t *o = &obs[i];
        int total = o->up_reads + o->lo_reads;
        int main_layer = -1;
        int main_reads = 0;
        int other_reads = 0;
        int confidence = 0;
        const char *note = "";

        if (total > 0) {
            if (o->up_reads >= o->lo_reads) {
                main_layer = 0;
                main_reads = o->up_reads;
                other_reads = o->lo_reads;
            } else {
                main_layer = 1;
                main_reads = o->lo_reads;
                other_reads = o->up_reads;
            }
            confidence = (main_reads * 100) / total;
        }

        if (o->expected_layer < 0) {
            unknown++;
            note = "未知EPC";
        } else if (total == 0) {
            note = "未读到";
        } else if (confidence < 70 || main_reads < 3) {
            uncertain++;
            note = "两层接近/读数太少";
        } else if (main_layer == o->expected_layer) {
            if (other_reads > 0) {
                cross++;
                note = "可判本层, 但有串读";
            } else {
                note = "稳定本层";
            }
            if (main_layer == 0) up_ok++;
            else lo_ok++;
        } else {
            cross++;
            note = "主读层与预期相反";
        }

        fprintf(out, "| %s | %s | %d | %d | %d | %d | %s | %d%% | %s |\n",
                o->title, layer_name(o->expected_layer), o->up_reads, o->lo_reads,
                o->up_seen_rounds, o->lo_seen_rounds, layer_name(main_layer),
                confidence, note);
    }

    fprintf(out, "\n");
    fprintf(out, "汇总: 上层稳定 %d, 下层稳定 %d, 有串读/反向 %d, 不确定 %d, 未知 %d\n\n",
            up_ok, lo_ok, cross, uncertain, unknown);
}

int main(int argc, char **argv)
{
    const char *up_dev = DEFAULT_UP_DEV;
    const char *lo_dev = DEFAULT_LO_DEV;
    const char *out_path = NULL;
    int start_power = 10;
    int end_power = 24;
    int step = 1;
    int rounds = 2;
    int seconds = 3;
    int opt;

    while ((opt = getopt(argc, argv, "U:D:a:b:s:r:t:o:h")) != -1) {
        switch (opt) {
        case 'U': up_dev = optarg; break;
        case 'D': lo_dev = optarg; break;
        case 'a': start_power = atoi(optarg); break;
        case 'b': end_power = atoi(optarg); break;
        case 's': step = atoi(optarg); break;
        case 'r': rounds = atoi(optarg); break;
        case 't': seconds = atoi(optarg); break;
        case 'o': out_path = optarg; break;
        default: usage(argv[0]); return opt == 'h' ? 0 : 1;
        }
    }

    if (step <= 0 || rounds <= 0 || seconds <= 0 || start_power > end_power) {
        usage(argv[0]);
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    FILE *out = stdout;
    if (out_path) {
        out = fopen(out_path, "a");
        if (!out) {
            perror("fopen");
            return 1;
        }
    }

    time_t ts = time(NULL);
    char tbuf[64];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", localtime(&ts));

    fprintf(out, "\n---\n\n");
    fprintf(out, "## 自动分层功率扫描记录\n\n");
    fprintf(out, "- 时间: %s\n", tbuf);
    fprintf(out, "- 上层设备: `%s`\n", up_dev);
    fprintf(out, "- 下层设备: `%s`\n", lo_dev);
    fprintf(out, "- 判层规则: 同一 EPC 上/下读数占比 >= 70%% 且主读数 >= 3，判为主读层；否则不确定。\n");
    fprintf(out, "- 说明: 有串读不等于失败，只要主读层明显高于另一层，软件即可稳定判层。\n\n");

    for (int p = start_power; running && p <= end_power; p += step) {
        obs_t obs[MAX_OBS];
        int obs_count = 0;
        memset(obs, 0, sizeof(obs));
        init_known_obs(obs, &obs_count);

        fprintf(out, "#### 原始扫描动作 %ddBm\n\n", p);
        for (int r = 0; running && r < rounds; r++) {
            fprintf(out, "- round %d/%d\n", r + 1, rounds);
            if (scan_one("上层", up_dev, 0, p, seconds, obs, &obs_count, out) < 0) break;
            usleep(200000);
            if (scan_one("下层", lo_dev, 1, p, seconds, obs, &obs_count, out) < 0) break;
            usleep(200000);
        }

        print_power_report(out, p, rounds, seconds, obs, obs_count);
        fflush(out);
    }

    if (out_path) fclose(out);
    return running ? 0 : 130;
}
