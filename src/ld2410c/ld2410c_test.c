/**
 * HLK-LD2410C 毫米波雷达 CLI 测试程序
 *
 * 调用 ld2410c 库，保留所有命令行参数。
 *
 * 编译: make
 * 运行: ./ld2410c_test [options] <uart_device>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include "ld2410c.h"

static int g_running = 1;
static int g_verbose = 0;
static int g_quiet = 0;

static void sig_handler(int sig)
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

/* ---- 显示 ---- */

static const char *energy_bar(uint8_t energy, int width)
{
    static char bar[64];
    int i, n = (int)(energy / 100.0f * width);
    if (n > width) n = width;
    if (n < 0) n = 0;
    for (i = 0; i < n; i++) bar[i] = '=';
    if (n < width) bar[n] = '>';
    for (i = n + 1; i < width; i++) bar[i] = ' ';
    bar[width] = '\0';
    return bar;
}

static void print_gate_energies(const ld2410c_data_t *data)
{
    int gate;
    printf("\n  ┌──────────┬───────┬───────┬───────┬───────┬───────┬───────┬───────┬───────┬───────┐\n");
    printf("  │ 距离门   │ Gate0 │ Gate1 │ Gate2 │ Gate3 │ Gate4 │ Gate5 │ Gate6 │ Gate7 │ Gate8 │\n");
    printf("  │ 范围(m)  │0~0.75 │0.75~1.5│1.5~2.25│2.25~3│3~3.75│3.75~4.5│4.5~5.25│5.25~6│6~6.75│\n");
    printf("  ├──────────┼───────┼───────┼───────┼───────┼───────┼───────┼───────┼───────┼───────┤\n");
    printf("  │ 运动能量 │");
    for (gate = 0; gate < LD2410C_MAX_GATES; gate++)
        printf(" %3d   ", data->moving_gate_energy[gate]);
    printf("│\n");
    printf("  │ 静止能量 │");
    for (gate = 0; gate < LD2410C_MAX_GATES; gate++)
        printf(" %3d   ", data->still_gate_energy[gate]);
    printf("│\n");
    printf("  └──────────┴───────┴───────┴───────┴───────┴───────┴───────┴───────┴───────┴───────┘\n");
}

static void print_frame(const ld2410c_data_t *data, unsigned long frame_count)
{
    char time_str[16];
    get_time_str(time_str, sizeof(time_str));

    if (data->is_engineering && g_verbose) {
        printf("\n╔══════════════════════════════════════════════════╗\n");
        printf("║  帧 #%lu (%s)                      ║\n", frame_count, time_str);
        printf("╠══════════════════════════════════════════════════╣\n");
        printf("║  状态: %-12s                              ║\n",
               ld2410c_state_name(data->target_state));
        printf("║  运动: %4d cm  能量: %3d                     ║\n",
               data->moving_distance_cm, data->moving_energy);
        printf("║  静止: %4d cm  能量: %3d                     ║\n",
               data->still_distance_cm, data->still_energy);
        printf("║  检测距离: %4d cm                             ║\n",
               data->detection_distance_cm);
        printf("║  光敏: %3d  OUT: %d                            ║\n",
               data->light_sensor, data->out_pin_state);
        printf("╚══════════════════════════════════════════════════╝\n");
        print_gate_energies(data);
    } else if (data->is_engineering) {
        printf("\r[%s] #%-6lu %s", time_str, frame_count,
               ld2410c_state_name(data->target_state));
        if (data->target_state != LD2410C_TARGET_NONE) {
            printf("  运动:%3dcm/%3d  静止:%3dcm/%3d  OUT:%d",
                   data->moving_distance_cm, data->moving_energy,
                   data->still_distance_cm, data->still_energy,
                   data->out_pin_state);
        }
        printf("\n  M:");
        for (int gate = 0; gate < LD2410C_MAX_GATES; gate++)
            printf(" %3d", data->moving_gate_energy[gate]);
        printf("\n  S:");
        for (int gate = 0; gate < LD2410C_MAX_GATES; gate++)
            printf(" %3d", data->still_gate_energy[gate]);
        printf("\n");
    } else {
        printf("\r[%s] #%-6lu  %-10s",
               time_str, frame_count,
               ld2410c_state_name(data->target_state));
        if (data->target_state != LD2410C_TARGET_NONE) {
            printf("  运动:%3dcm E=%-3s  静止:%3dcm E=%-3s",
                   data->moving_distance_cm,
                   energy_bar(data->moving_energy, 10),
                   data->still_distance_cm,
                   energy_bar(data->still_energy, 10));
        }
        fflush(stdout);
    }
}

static void print_stats(const ld2410c_stats_t *stats)
{
    printf("\n\n=== 统计 ===\n");
    printf("总帧数:      %lu\n", stats->total_frames);
    printf("有效帧:      %lu\n", stats->valid_frames);
    printf("帧头错误:    %lu\n", stats->err_header);
    printf("长度错误:    %lu\n", stats->err_length);
    printf("帧尾错误:    %lu\n", stats->err_footer);
    printf("内部标记错误: %lu\n", stats->err_inner);
    if (stats->acks_received > 0)
        printf("ACK 响应:    %lu\n", stats->acks_received);
    if (stats->total_frames > 0)
        printf("有效率:      %.1f%%\n",
               100.0f * stats->valid_frames / stats->total_frames);
}

/* ---- 打印回调 ---- */

static void on_data_cb(const ld2410c_data_t *data)
{
    /* 安静模式: 只在状态变化时打印 */
    if (g_quiet) return;
    /* 正常模式在 poll 循环中打印 */
}

static void on_state_change_cb(uint8_t old_state, uint8_t new_state)
{
    if (g_quiet) {
        char time_str[16];
        get_time_str(time_str, sizeof(time_str));
        printf("[%s] %s\n", time_str, ld2410c_state_name(new_state));
    }
}

/* ---- 用法 ---- */

static void print_usage(const char *prog)
{
    printf("Usage: %s [options] <uart_device>\n\n", prog);
    printf("Options:\n");
    printf("  -v              详细模式\n");
    printf("  -q              安静模式 (只显示状态变化)\n");
    printf("  -b <baud>       波特率 (默认: %d)\n", LD2410C_DEFAULT_BAUD);
    printf("  -e              开启工程模式\n");
    printf("  -n              关闭工程模式\n");
    printf("  -f <count>      读取指定帧数后退出\n");
    printf("  --version       读取固件版本\n");
    printf("  --sensitivity <n> 设置灵敏度 0~100 (统一全部距离门)\n");
    printf("  --factory       恢复出厂设置\n");
    printf("  --restart       重启模块\n");
    printf("  --max-gate <0-8> 最大检测距离门\n");
    printf("  --unmanned <s>   无人上报延时秒数\n");
    printf("  --noise         查询底噪状态\n");
    printf("\nExamples:\n");
    printf("  %s /dev/ttyS1                    基本模式\n", prog);
    printf("  %s -e /dev/ttyS1                 工程模式\n", prog);
    printf("  %s -v -e -f 10 /dev/ttyS1        详细+工程, 读10帧\n", prog);
}

/* ---- 主程序 ---- */

int main(int argc, char *argv[])
{
    const char *device = NULL;
    int baud_rate = LD2410C_DEFAULT_BAUD;
    int do_engineering = -1;
    int do_version = 0;
    int do_noise = 0;
    int do_sensitivity = -1;
    int do_factory = 0;
    int do_restart = 0;
    int do_max_gate = -1;
    int do_unmanned = -1;
    int frame_limit = 0;
    ld2410c_ctx_t ctx;
    int i;

    /* 参数解析 */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            do_version = 1;
        } else if (strcmp(argv[i], "--sensitivity") == 0 && i + 1 < argc) {
            do_sensitivity = atoi(argv[++i]);
            if (do_sensitivity < 0) do_sensitivity = 0;
            if (do_sensitivity > 100) do_sensitivity = 100;
        } else if (strcmp(argv[i], "--noise") == 0) {
            do_noise = 1;
        } else if (strcmp(argv[i], "--factory") == 0) {
            do_factory = 1;
        } else if (strcmp(argv[i], "--restart") == 0) {
            do_restart = 1;
        } else if (strcmp(argv[i], "-v") == 0) {
            g_verbose = 1; g_quiet = 0;
        } else if (strcmp(argv[i], "-q") == 0) {
            g_quiet = 1; g_verbose = 0;
        } else if (strcmp(argv[i], "-e") == 0) {
            do_engineering = 1;
        } else if (strcmp(argv[i], "-n") == 0) {
            do_engineering = 0;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "-?") == 0) {
            print_usage(argv[0]); return 0;
        } else if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            baud_rate = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            frame_limit = atoi(argv[++i]);
            if (frame_limit < 1) frame_limit = 1;
        } else if (strcmp(argv[i], "--max-gate") == 0 && i + 1 < argc) {
            do_max_gate = atoi(argv[++i]);
            if (do_max_gate < 0) do_max_gate = 0;
            if (do_max_gate > 8) do_max_gate = 8;
        } else if (strcmp(argv[i], "--unmanned") == 0 && i + 1 < argc) {
            do_unmanned = atoi(argv[++i]);
            if (do_unmanned < 0) do_unmanned = 0;
        } else if (argv[i][0] != '-') {
            device = argv[i];
        } else {
            fprintf(stderr, "未知选项: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (device == NULL) {
        fprintf(stderr, "Error: 未指定串口设备\n\n");
        print_usage(argv[0]);
        return 1;
    }

    /* ---- 初始化 ---- */
    if (ld2410c_init(&ctx, device, baud_rate) != 0) {
        fprintf(stderr, "无法打开 %s\n", device);
        return 1;
    }
    ld2410c_set_verbose(&ctx, g_verbose);
    ctx.on_data = on_data_cb;
    ctx.on_state_change = on_state_change_cb;

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* ---- 执行命令 ---- */
    if (do_version) {
        uint16_t type = 0, ver = 0;
        if (ld2410c_read_version(&ctx, &type, &ver) == 0)
            printf("固件类型: 0x%04X, 版本: V%d.%02d\n", type, ver / 100, ver % 100);
        else
            fprintf(stderr, "读取固件版本失败\n");
    }

    if (do_noise) {
        ld2410c_read_noise_status(&ctx);
    }

    if (do_sensitivity >= 0) {
        ld2410c_set_sensitivity(&ctx, (uint8_t)do_sensitivity,
                                (uint8_t)do_sensitivity);
        usleep(100000);
    }

    if (do_factory) {
        ld2410c_restart(&ctx);
        printf("已发送恢复出厂设置命令\n");
    }

    if (do_restart) {
        ld2410c_restart(&ctx);
        printf("已发送重启命令\n");
    }

    if (do_max_gate >= 0) {
        int unmanned = (do_unmanned >= 0) ? do_unmanned : 5;
        ld2410c_set_max_gate(&ctx, do_max_gate, do_max_gate, unmanned);
        usleep(100000);
    }

    if (do_engineering == 1) {
        ld2410c_set_engineering_mode(&ctx, 1);
        usleep(100000);
    } else if (do_engineering == 0) {
        ld2410c_set_engineering_mode(&ctx, 0);
        usleep(100000);
    }

    /* 如果只执行了命令，退出 */
    if ((do_version || do_noise || do_factory || do_restart ||
         do_max_gate >= 0 || do_sensitivity >= 0) &&
        do_engineering == -1 && frame_limit == 0) {
        ld2410c_close(&ctx);
        return 0;
    }

    /* ---- 连续读取 ---- */
    if (!g_quiet) {
        printf("等待雷达数据... (Ctrl+C 停止)\n");
        if (!g_verbose && !do_engineering)
            printf("(使用 -v 查看详细，-e 开启工程模式)\n");
        printf("\n");
    }

    while (g_running) {
        int ret = ld2410c_poll(&ctx, 200);  /* 200ms 超时 */
        if (ret < 0) continue;

        if (ret > 0 && !g_quiet) {
            const ld2410c_data_t *data = ld2410c_get_data(&ctx);
            if (data) print_frame(data, ctx.stats.valid_frames);
        }

        if (frame_limit > 0 && (int)ctx.stats.valid_frames >= frame_limit)
            break;
    }

    /* ---- 统计 ---- */
    if (!g_quiet && ctx.stats.total_frames > 0)
        print_stats(ld2410c_get_stats(&ctx));

    /* 退出前关工程模式 */
    if (do_engineering == 1 && ctx.is_engineering) {
        printf("\n关闭工程模式...\n");
        ld2410c_set_engineering_mode(&ctx, 0);
    }

    ld2410c_close(&ctx);
    printf("\nDone.\n");
    return 0;
}
