/**
 * 快速测试: 分别盘点上下层 UHF, 确认物理连接未接反
 * 用法: ./test_uhf_check
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "uhf_driver.h"

#define UP_DEV   "/dev/serial/by-path/platform-ff780000.usb-usb-0:1.3:1.0-port0"
#define LO_DEV   "/dev/serial/by-path/platform-ff780000.usb-usb-0:1.2:1.0-port0"

static int test_one(const char *label, const char *device, int power_dbm)
{
    uhf_t u;
    memset(&u, 0, sizeof(u));

    printf("\n=== %s (%s, %ddBm) ===\n", label, device, power_dbm);

    if (uhf_init(&u, device) != 0) {
        printf("  [FAIL] uhf_init 失败\n");
        return -1;
    }
    printf("  FW: %d.%d.%d\n", u.fw_ver[0], u.fw_ver[1], u.fw_ver[2]);

    if (uhf_set_power(&u, power_dbm) != 0) {
        printf("  [FAIL] 设置功率失败\n");
        uhf_deinit(&u);
        return -1;
    }

    /* 单次盘点: 等 3 秒 */
    int n = uhf_inventory_once(&u, 3000);
    printf("  读到 %d 个标签:\n", n);

    for (int i = 0; i < u.tag_count; i++) {
        printf("    [%d] EPC=%s  RSSI=%d  ant=%d\n",
               i, u.tags[i].epc_str, u.tags[i].rssi, u.tags[i].antenna);
    }

    uhf_deinit(&u);
    printf("  [OK] 测试完成\n");
    return n;
}

int main(void)
{
    printf("UHF 模块连接验证测试\n");
    printf("=====================\n");

    /* 上层: 20dBm, 下层: 18dBm (甜区中心) */
    test_one("上层 UHF (1.3)", UP_DEV, 20);
    test_one("下层 UHF (1.2)", LO_DEV, 18);

    printf("\n=== 判断方法 ===\n");
    printf("上层应读到: 复变/马原/标日上/数电/信息论/概统/dsp/电磁场/fsf3/fsf4\n");
    printf("下层应读到: 数理方程/毛概/线代/标日下/大物4/大物3/模电/通信原理\n");
    printf("如果读到相反的, 说明 USB 接反了。\n");

    return 0;
}
