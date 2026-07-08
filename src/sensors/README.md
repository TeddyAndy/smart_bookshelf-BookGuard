# 传感器模块

> BH1750 光照 + SHT30 温湿度 | I2C2 (P9 Pin3/5)

---

## 硬件

| 传感器 | 型号 | 地址 | 参数 |
|--------|------|:---:|------|
| 光照 | BH1750FVI | 0x23 | 1-65535 lx, 16-bit ADC |
| 温湿度 | SHT30-DIS | 0x44 | -40~125°C, 0~100%RH |

接线：两个传感器并联在 I2C2 总线上（VCC=3.3V, GND, SDA=Pin3, SCL=Pin5）。

## 文件

| 文件 | 说明 |
|------|------|
| `i2c_sensors.c` | BH1750 + SHT30 合并读取程序 |
| `Makefile` | 编译 `i2c_sensors` |
| `bh1750_standalone/` | BH1750 独立测试 + README |
| `sht30_standalone/` | SHT30 独立测试 + README |

## FSM 集成

传感器已集成到 FSM（`fsm.c`），自检时探测 0x23 和 0x44 地址，运行时每分钟读取一次写入 `sensor_log` 表，并通过 MQTT 推送。

---

**最后更新**: 2026-06-17
