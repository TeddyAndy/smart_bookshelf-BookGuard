# HLK-LD2410C 毫米波雷达驱动

基于海凌科 HLK-LD2410C 24GHz FMCW 毫米波雷达，通过 UART 读取人体存在检测数据。

---

## 📁 文件

| 文件 | 说明 |
|------|------|
| `ld2410c.h` | 库 API — context/init/poll/命令/回调 |
| `ld2410c.c` | 库实现 — UART/帧解析/协议 |
| `ld2410c_test.c` | CLI 测试程序（调用库） |
| `radar_monitor.c` | 实时监测仪表盘 |
| `Makefile` | 编译脚本 |

---

## 🔧 编译

```bash
make                    # 编译 ld2410c_test + radar_monitor
make clean              # 清理
make upload             # 编译并上传到板子
```

---

## 🔌 硬件连接

| LD2410C | P9 引脚 | 说明 |
|---------|---------|------|
| VCC | Pin 4 (VCC_5V) | **5V 供电** |
| GND | Pin 6 (GND) | 地 |
| TX | Pin 10 (UART1_RX) | 雷达发送 → 板子接收 |
| RX | Pin 8 (UART1_TX) | 板子发送 → 雷达接收 |
| OUT | (可选) | 3.3V, 有人=HIGH |

- 串口设备: `/dev/ttyS1`
- 波特率: **115200** bps 8N1（本系统已统一）

---

## 🚀 使用方法

### 实时监测仪表盘
```bash
/root/test/radar_monitor /dev/ttyS1
```

### CLI 测试
```bash
/root/test/ld2410c_test [options] /dev/ttyS1
```

| 参数 | 说明 |
|------|------|
| `-e` | 开启工程模式 |
| `-v` | 详细模式 |
| `-q` | 安静模式（仅输出状态变化） |
| `-b <baud>` | 波特率（默认 115200） |
| `-f <count>` | 读取指定帧数后退出 |
| `--version` | 读取固件版本 |
| `--sensitivity <n>` | 设置灵敏度 0~100 |
| `--max-gate <0-8>` | 最大检测距离门 |
| `--restart` | 重启模块 |

---

## 📚 库 API

```c
#include "ld2410c.h"

ld2410c_ctx_t ctx;

// 初始化
ld2410c_init(&ctx, "/dev/ttyS1", 115200);

// 设置回调 (可选)
ctx.on_data = my_data_cb;
ctx.on_state_change = my_state_cb;  // 状态变化时触发

// 主循环中轮询 (非阻塞)
while (1) {
    ld2410c_poll(&ctx, 0);
    // ...
}

// 命令
ld2410c_read_version(&ctx, &type, &ver);
ld2410c_set_sensitivity(&ctx, 30, 30);
ld2410c_set_max_gate(&ctx, 3, 3, 5);

// 关闭
ld2410c_close(&ctx);
```

---

## 🎯 距离门说明

| 门号 | 范围 | 典型场景 |
|------|------|----------|
| Gate 0 | 0 ~ 0.75m | 桌面/书桌近距离 |
| Gate 1 | 0.75 ~ 1.5m | **书架前方** ← 主要使用 |
| Gate 2 | 1.5 ~ 2.25m | 房间通道 |
| Gate 3+ | 2.25m+ | 远距离 |

---

## 🛠️ 技术规格

| 参数 | 规格 |
|------|------|
| 芯片 | HLK-LD2410C (S3KM1110) |
| 频率 | 24GHz ~ 24.25GHz FMCW |
| 检测范围 | 0.2 ~ 6m |
| 供电 | 5V (≥200mA) |
| 接口 | UART 115200bps 8N1 |

---

## FSM 集成

已集成到 FSM v8: `check_radar` 自检 → 设门限 (运动1.5m/静止0.75m) → 每 tick `ld2410c_poll(1ms)` → SLEEP/ACTIVE 切换。

---

**最后更新**: 2026-06-17 — FSM 集成
