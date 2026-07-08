# WS2812B 书架灯带驱动

## 文件结构

```
src/ws2812b/
├── ws2812b.h/c        ← 底层 SPI 驱动 (6.4MHz, 单线协议)
├── shelf_led.h/c      ← 书架灯带库 (物理布局 + 场景 API)
├── ws2812b_test.c     ← 底层测试程序 (纯色/追逐/彩虹/呼吸/进度条)
├── shelf_led_test.c   ← 书架库测试程序 (7 步验证物理映射)
└── Makefile
```

## 物理布局 (78 灯)

```
LED #0     ← 上层状态指示灯 (左上角起点)
LED #1~37  ← 上层位置导引灯 (37灯, 左→右, ~1灯/cm)
LED #38    ← 下层状态指示灯
LED #39~77 ← 下层位置导引灯 (39灯, 左→右, ~1灯/cm) (左下角终点)

走线: SPI1 MOSI Pin38 → DIN → 绕书架一圈 → LED #77 DOUT
```

## 接线

```
ELF3506 P9                     WS2812B 灯带
═══════════                    ════════════
Pin 2 或 4  (5V)  ──────────→  VCC (红线)
Pin 6        (GND) ──────────→  GND (白线)
Pin 38 (SPI1_MOSI) ──────────→  DIN (绿线)
                [建议加 3.3V→5V 电平转换]
```

> WS2812B 是 5V 逻辑电平，ELF3506 GPIO 输出 3.3V。短距离可用，偶尔头部爆闪加 74HCT125。

## 编译 & 上传

```bash
cd src/ws2812b
make          # 交叉编译 (ws2812b_test + shelf_led_test)
make upload   # 上传到板子 /root/test/
```

## shelf_led 书架库 API

### 初始化

```c
#include "shelf_led.h"

shelf_led_init("/dev/spidev1.0", 78);   // 初始化, 默认亮度 10
shelf_led_set_brightness(10);           // 调亮度 (0~255)
shelf_led_close();                      // 全熄 + 释放
```

### 灯珠控制 (颜色自动乘全局亮度)

```c
shelf_led_set(20, 0, 0, 255);           // LED #20 亮蓝色
shelf_led_set(60, 255, 0, 0);           // LED #60 亮红色
shelf_led_fill(255, 255, 255);          // 全白
shelf_led_clear();                       // 全熄
shelf_led_show();                        // 发送到灯带
```

### 层状态指示

```c
shelf_led_status(0, 0, 255, 0);         // 上层状态灯(LED 0) 绿色
shelf_led_status(1, 255, 0, 0);         // 下层状态灯(LED 38) 红色
shelf_led_clear_status(0);              // 熄灭上层状态灯
```

### 位置导引

```c
shelf_led_position(0, 10, 20, 0, 0, 255);  // 上层 10~20cm 亮蓝
shelf_led_position(1, 5, 8, 255, 0, 0);    // 下层 5~8cm 亮红
shelf_led_position_all(0, 0, 255, 0);       // 上层全部位置灯绿色
shelf_led_clear_position(1);                 // 熄灭下层所有位置灯
```

### 一键场景

```c
shelf_led_scene_sleep();                     // 休眠: 全熄
shelf_led_scene_aware();                     // 感知: 状态绿 + 暖白底光
shelf_led_scene_borrowed(0, 10, 15);        // 取出: 蓝
shelf_led_scene_returned(1, 20, 25);        // 放回: 绿
shelf_led_scene_misplaced(1, 5, 10);        // 错放: 红
shelf_led_scene_find(0, 25, 30);            // 查找: 金
shelf_led_scene_error();                     // 故障: 全红
```

## 测试程序

```bash
# 底层测试 (78灯, 原始亮度参数)
/root/test/ws2812b_test

# 书架库测试 (默认亮度 10)
/root/test/shelf_led_test
/root/test/shelf_led_test -b 5      # 更暗
/root/test/shelf_led_test -b 20     # 稍亮
/root/test/shelf_led_test -b 0      # 静默 (不亮灯, 只打印)

# 测试步骤:
#  1. 物理顺序验证 (LED 0→77 逐个亮)
#  2. 状态指示灯 (红绿蓝金)
#  3. 上层位置灯 (逐 cm + 区间)
#  4. 下层位置灯 (逐 cm + 区间)
#  5. 全部场景模式 (休眠/感知/取出/放回/错放/查找/故障)
#  6. 两本书同时显示
#  7. 彩虹扫光
```

## 全局亮度

所有 `shelf_led_*` 函数传入的颜色值自动乘以 `brightness/255`：

| brightness | 缩放 | 255→实际 | 说明 |
|------------|------|----------|------|
| 10 | ~4% | 10 | 默认, 微光不刺眼 |
| 5 | ~2% | 5 | 暗光 |
| 20 | ~8% | 20 | 稍亮 |
| 0 | 0% | 0 | 静默模式 |
| 255 | 100% | 255 | 全功率 (注意功耗!) |

已设置的颜色不变，需重新设置场景后再 `shelf_led_show()`。

## SPI 时序原理

```
SPI 时钟: 6.4 MHz (1 bit = 156.25 ns)
1 个 SPI 字节 = 8 个 SPI bit = 1.25 µs = 1 个 WS2812B bit

WS2812B bit 0: SPI 0xC0 (11000000)
  ┌──┐        312ns 高
──┘  └──────  937ns 低

WS2812B bit 1: SPI 0xF8 (11111000)
  ┌─────┐     781ns 高
──┘     └───  469ns 低

颜色顺序: GRB (绿 → 红 → 蓝), MSB 优先
RESET: >50µs 低电平 (SPI MODE0 空闲低 + usleep 100µs)
```

## 功耗参考

| 亮度 | 单灯电流 | 78 灯总电流 | 功率 (5V) |
|------|----------|-------------|-----------|
| 100% 白色 | ~60mA | ~4.7A | ~23W |
| 50% 白色 | ~30mA | ~2.3A | ~11.5W |
| 10% 白色 | ~6mA | ~0.47A | ~2.3W |
| 默认亮度(~4%) | ~2.4mA | ~0.19A | ~0.9W |

> 默认亮度 10/255 下 78 灯全亮约 0.19A/0.9W，板载 5V 引脚可安全供电。
> 高亮度建议外接 5V 电源。

---

**最后更新**: 2026-06-17 — FSM v8 集成
