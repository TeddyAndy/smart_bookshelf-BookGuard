# 红外阵列独立测试程序

本目录是红外反射阵列 PCB 的单板测试程序，用来验证 `ITR8307 + 74HC595 + TCA9555` 的分时扫描是否正常。

测试程序：`ir_monitor.c`

- 通过 3 个 GPIO 控制 `74HC595`，每次只点亮一组红外发射管。
- 通过 I2C 读取 `TCA9555` 的 16 路输入。
- 终端里用 `#` 表示检测到遮挡/反射，用 `.` 表示未检测到。
- TCA9555 输入按低电平有效处理：`0 = detected`，`1 = open`。

## 当前书架接线

当前整机 FSM 使用传感器 I2C2 和 3 根 GPIO 扫描红外阵列。红外板上的信号名如果和 PCB 丝印一致，按这一列接。

| 红外板信号 | ELF3506 接口 | ELF3506 引脚/功能 | Linux GPIO | 程序参数 |
|---|---|---|---:|---|
| `SER` / `DS` / `DATA` | P14 | Pin 33, `GPIO1_A0` | `gpio32` | `--ser 32` |
| `RCLK` / `STCP` / `LATCH` | P14 | Pin 32, `GPIO1_A1` | `gpio33` | `--rclk 33` |
| `SRCLK` / `SHCP` / `CLOCK` | P14 | Pin 31, `GPIO1_A2` | `gpio34` | `--srclk 34` |
| `SDA` | P9 | Pin 3, `I2C2_SDA` | - | `-b 2` |
| `SCL` | P9 | Pin 5, `I2C2_SCL` | - | `-b 2` |
| `GND` | P14 或 P9 | 任意 `GND` | - | - |
| `VCC` | P14 或 P9 | 按红外板电源设计接 `3.3V` 或 `5V` | - | - |

已测当前 I2C：

- 设备节点：`/dev/i2c-2`
- TCA9555 7-bit 地址：`0x20`

注意：

- `SER/RCLK/SRCLK` 是 ELF3506 直接输出的 3.3V GPIO，红外板逻辑电平需要兼容 3.3V。
- `SDA/SCL` 当前接 I2C2，也就是 P9 Pin 3/5，和 SHT30/BH1750 共用总线。
- 必须共地，否则 GPIO 和 I2C 都可能表现异常。

## 四块红外板地址和覆盖范围

一层书架长度按 `38 cm` 处理。每块红外板 16 路，覆盖半层约 `19 cm`，单点间距约 `19 / 16 = 1.1875 cm`。

当前 FSM 里的预留配置在 `src/fsm/fsm.c` 的 `g_ir_boards[]`：

| 板位 | TCA9555 地址 | 层 | 覆盖范围 | 当前状态 |
|---|---:|---:|---:|---|
| `upper_left` | `0x21` | 上层 `0` | `0.0-19.0 cm` | 预留 |
| `upper_right` | `0x20` | 上层 `0` | `19.0-38.0 cm` | 已接入 |
| `lower_left` | `0x22` | 下层 `1` | `0.0-19.0 cm` | 预留 |
| `lower_right` | `0x24` | 下层 `1` | `19.0-38.0 cm` | 预留 |

同一 I2C2 上已有传感器地址：

| 设备 | I2C 地址 | 备注 |
|---|---:|---|
| `SHT30` 温湿度 | `0x44` | `ADDR` 拉高时为 `0x45` |
| `BH1750` 光照 | `0x23` | 默认地址 |

TCA9555 常用地址范围是 `0x20` 到 `0x27`，不要和已有传感器冲突。4 块红外板可以先按下面分配：

单块板测试示例：

```bash
/root/test/ir_live -i 20 -f 1
```

`ir_live` 会直接按当前 FSM 使用的 I2C2/GPIO/TCA9555 扫描逻辑实时显示红外点，适合测试灵敏度和书是否推到足够靠里。

如果要测试其他预留地址，可以用 `ir_monitor`：

```bash
/root/test/ir_monitor -b 2 -a 0x21 --ser 32 --rclk 33 --srclk 34
```

## 编译和上传

在开发机上交叉编译：

```bash
make -C src/infrared
```

上传到板子：

```bash
scp src/infrared/ir_monitor root@192.168.0.232:/root/test/
```

也可以直接用 Makefile 的上传目标：

```bash
make -C src/infrared upload
```

默认上传地址：

- 板子 IP：`192.168.0.232`
- 用户：`root`
- 目录：`/root/test/`

如需改地址：

```bash
make -C src/infrared upload BOARD_IP=192.168.0.123
```

## 运行

在 ELF3506 板子上运行：

```bash
/root/test/ir_monitor
```

默认会一直刷新显示：

```text
Infrared PCB monitor  scan=0  I2C=/dev/i2c-2 addr=0x20
GPIO: SER=32 RCLK=33 SRCLK=34 | #=blocked/reflection .=open

S00-S07: S00=. S01=. S02=# S03=. S04=. S05=. S06=. S07=.
S08-S15: S08=. S09=. S10=. S11=. S12=. S13=. S14=. S15=.

Bitmap: ..#.............
```

含义：

- `S00` 到 `S15` 是 TCA9555 读到的 16 路接收通道。
- `#` 表示该通道输入为低电平，程序认为检测到遮挡/反射。
- `.` 表示该通道输入为高电平，程序认为未检测到。
- `Bitmap` 是 16 路状态的紧凑显示，方便连续观察。

## 常用参数

```bash
/root/test/ir_monitor -n 1
```

扫描 1 次后退出，适合快速确认。

```bash
/root/test/ir_monitor -r
```

显示每一组点亮时 TCA9555 的原始输入字节，排查通道映射时很有用。

```bash
/root/test/ir_monitor -d 200
```

刷新间隔改为 200 ms，默认是 100 ms。

```bash
/root/test/ir_monitor -s 5000
```

点亮红外组后等待 5000 us 再读取，默认是 3000 us。

```bash
/root/test/ir_monitor -b 2 -a 0x20
```

指定 I2C bus 和 TCA9555 地址。

```bash
/root/test/ir_monitor --ser 32 --rclk 33 --srclk 34
```

手动指定 74HC595 三根控制线的 Linux GPIO 编号。

## 扫描逻辑

程序每轮扫描 8 个发射组：

1. `74HC595` 输出 `1 << group`，只打开当前红外发射组。
2. 等待 `settle_us`，默认 `3000 us`。
3. 读取 TCA9555 的 `INPUT0` 和 `INPUT1`。
4. 关闭所有红外发射组。
5. 根据当前 group 取两路结果：
   - `INPUT0` 的 bit `group` 映射到 `S00-S07`
   - `INPUT1` 的 bit `group` 映射到 `S08-S15`

因此第 `group` 组点亮时，会更新两个显示通道：`Sgroup` 和 `S(group + 8)`。

## 排查

如果运行时报 `/dev/i2c-2` 打不开：

- 确认系统里有 `/dev/i2c-2`。
- 确认红外板 `SDA/SCL` 接的是 P9 Pin 3/5。
- 如果实际接到其他 I2C bus，用 `-b` 改 bus。

如果报 `I2C_SLAVE` 或读写失败：

- 确认 TCA9555 地址是 `0x20`。
- 用 `i2cdetect -y 2` 看地址是否出现。
- 确认 `VCC/GND/SDA/SCL` 都接好。

如果所有通道一直是 `.`：

- 确认 `SER/RCLK/SRCLK` 三根线没有接反。
- 用 `-r` 看原始输入是否一直为 `0xff`。
- 确认红外发射端供电正常。

如果所有通道一直是 `#`：

- 用 `-r` 看原始输入是否一直为 `0x00`。
- 检查 TCA9555 输入上拉、接收管方向、短路或焊接问题。

清理编译产物：

```bash
make -C src/infrared clean
```
