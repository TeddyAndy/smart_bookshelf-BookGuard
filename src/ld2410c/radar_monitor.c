/**
 * 雷达实时监测程序
 *
 * 运行: ./radar_monitor /dev/ttyS1
 * 退出: Ctrl+C
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include "ld2410c.h"

static int g_running = 1;

static void sig_handler(int sig) { (void)sig; g_running = 0; }

int main(int argc, char *argv[])
{
    ld2410c_ctx_t ctx;
    const char *device = (argc > 1) ? argv[1] : "/dev/ttyS1";
    int baud = (argc > 2) ? atoi(argv[2]) : 115200;
    unsigned long frame_count = 0;

    if (ld2410c_init(&ctx, device, baud) != 0) {
        fprintf(stderr, "无法打开雷达 %s\n", device);
        return 1;
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("\033[2J\033[H"); /* 清屏 */
    printf("┌─────────────────────────────────────┐\n");
    printf("│       LD2410C 雷达实时监测          │\n");
    printf("├─────────────────────────────────────┤\n");
    printf("│                                     │\n");
    printf("│       等待数据...                   │\n");
    printf("│                                     │\n");
    printf("└─────────────────────────────────────┘\n");

    while (g_running) {
        int ret = ld2410c_poll(&ctx, 0);
        if (ret <= 0) continue;

        frame_count++;
        const ld2410c_data_t *d = ld2410c_get_data(&ctx);
        if (!d) continue;

        /* 状态对应的图标和颜色提示 */
        const char *icon, *hint;
        switch (d->target_state) {
            case LD2410C_TARGET_NONE:       icon = "○"; hint = "(无人)";         break;
            case LD2410C_TARGET_MOVING:     icon = "◎"; hint = "有物体在移动";   break;
            case LD2410C_TARGET_STATIONARY: icon = "●"; hint = "有人静止站立";   break;
            case LD2410C_TARGET_BOTH:       icon = "◉"; hint = "有人在移动中";   break;
            default:                        icon = "?"; hint = "底噪检测中...";  break;
        }

        /* 光标上移 5 行，覆盖刷新 */
        printf("\033[5A");
        printf("│                                     │\n");
        printf("│  状态: %-4s %-20s     │\n", icon, ld2410c_state_name(d->target_state));
        printf("│  %-35s │\n", hint);
        printf("│  运动: %3d cm  (能量 %3d)            │\n",
               d->moving_distance_cm, d->moving_energy);
        printf("│  静止: %3d cm  (能量 %3d)            │\n",
               d->still_distance_cm, d->still_energy);
        printf("│  OUT: %d  帧数: %lu                   │\n",
               d->out_pin_state, frame_count);
        printf("└─────────────────────────────────────┘\n");
    }

    /* 恢复光标 */
    printf("\n\n退出。共 %lu 帧\n", frame_count);
    ld2410c_close(&ctx);
    return 0;
}
