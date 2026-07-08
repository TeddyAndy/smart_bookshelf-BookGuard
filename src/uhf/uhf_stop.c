/**
 * 安全关闭两个 UHF 模块：发送 STOP + 关闭串口
 */
#include <stdio.h>
#include "uhf_driver.h"

#define UP_DEV  "/dev/serial/by-path/platform-ff780000.usb-usb-0:1.3:1.0-port0"
#define LO_DEV  "/dev/serial/by-path/platform-ff780000.usb-usb-0:1.2:1.0-port0"

int main(void) {
    uhf_t u;

    /* 上层 */
    if (uhf_init(&u, UP_DEV) == 0) {
        printf("上层: 已停止盘存 + 关闭串口\n");
        uhf_deinit(&u);
    } else {
        printf("上层: 无需操作 (已关闭)\n");
    }

    /* 下层 */
    if (uhf_init(&u, LO_DEV) == 0) {
        printf("下层: 已停止盘存 + 关闭串口\n");
        uhf_deinit(&u);
    } else {
        printf("下层: 无需操作 (已关闭)\n");
    }

    printf("UHF 模块已安全关闭\n");
    return 0;
}
