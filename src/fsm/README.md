# 智能书架 FSM

FSM 是板端主控进程，负责 UHF 双层盘点、书籍取放事件、错放判断、查找/加书闭环、传感器入库、MQTT 上报、灯带控制，并通过本地文件和屏幕 UI 联动。

---

## 当前硬件配置

| 模块 | 配置 |
|------|------|
| 上层 UHF | `/dev/serial/by-path/platform-ff780000.usb-usb-0:1.2:1.0-port0` |
| 下层 UHF | `/dev/serial/by-path/platform-ff780000.usb-usb-0:1.3:1.0-port0` |
| 上层功率 | `18dBm` |
| 下层功率 | `18dBm` |
| 雷达 | `/dev/ttyS1`, LD2410C |
| 温湿度/光照 | `/dev/i2c-2`, SHT30 + BH1750 |
| 红外阵列 | `/dev/i2c-2`, TCA9555 + 74HC595，当前接入上层右半边 |
| 灯带 | `/dev/spidev1.0`, 78 颗 |
| 本地数据库 | `/root/bookshelf.db` |

2026-06-20 实测确认：`1.2` 稳定读上层 10 本，`1.3` 稳定读下层 8 本。15-18dBm 是当前最稳窗口；19-20dBm 开始出现“线代”上下层读数接近。

---

## 状态流

```text
BOOT
  自检 LED / 雷达 / WiFi / 传感器 / MQTT / 屏幕 / UHF
  UHF 基线扫描
    ↓
ACTIVE  ←→  SLEEP
  ↓          ↑
FIND ────────┘

任意状态自检或运行失败 → FAULT
```

| 状态 | 说明 |
|------|------|
| `BOOT` | 模块自检，UHF 基线扫描，建立初始在架状态 |
| `ACTIVE` | 正常盘点，处理取放、错放、加书、查找命令 |
| `SLEEP` | 无人时停 UHF 省电，雷达唤醒后回 ACTIVE，屏幕由 UI 关背光 |
| `FIND` | 查找书籍，黄灯指示目标，目标取走后屏幕收到反馈 |
| `FAULT` | 致命模块异常，停 UHF 并红灯提示 |

---

## 雷达休眠保护

LD2410C 如果串口在线但一直上报“无人”，旧逻辑会在 `FSM_SLEEP_TIMEOUT` 后误进入休眠。现在 FSM 增加了雷达可信度门槛：

- 运动检测距离门为 `2`，静止检测距离门为 `2`，约覆盖 1.5m 内的人体。
- 启动时下发运动灵敏度 `70`、静止灵敏度 `75`。
- 本次启动后雷达至少识别过一次人体，才允许它触发自动休眠。
- 雷达有效帧超过 `FSM_RADAR_FRAME_STALE_SEC` 未更新，也视为不可信。
- 雷达不可信时 FSM 保持 `ACTIVE`，避免人在书架前仍然停 UHF。
- `/tmp/fsm_state` 会写出 `radar_seen`、`radar_sleep_reliable`、`radar_last_person`，用于排查硬件或摆放问题。

这只是软件保护；如果雷达长期不识别人，仍应检查供电、串口、朝向和灵敏度配置。

---

## UHF 判层逻辑

UHF 串读是正常现象，FSM 不要求完全零串读。当前 scanner 使用“三路证据”判断主读层：

- 每个 tick 分别读取同一 EPC 在上层模块和下层模块的 `read_count` 增量。
- 对上下层分数做衰减累计，避免历史读数永久影响判断。
- 读数增量、滚动分数、上下层 RSSI 都会参与判层；其中任一路连续稳定偏向另一层，就会更新实际层。
- 证据不足时保持上一轮实际层，不再直接回退到预期层，避免真实错放被“预期层兜底”吞掉。
- 分数主读层门槛约为 `58%` 或主分数领先另一层 `>= 3`，比旧的 `70%` 更适合有物理隔离但仍会串读的书架。
- 未知 EPC 只在加书模式处理，正常盘点不会触发登记。
- 已在架书籍如果主读层连续稳定变化，会触发一次 `returned/misplaced` 事件，更新 `shelf_state.layer`。
- 错放书在灯带上显示为原本预期位置的粉红色，方便放回正确位置。

这套逻辑对应当前实测：15-18dBm 下 18 本书都能稳定判到预期层；19-20dBm 线代会变成不确定，因此默认不使用。

---

## 红外阵列和位置融合

当前系统可以理解为：

```text
UHF 负责身份
红外负责位置变化
数据库历史状态负责约束和排序
```

红外不是直接识别哪本书，而是给出哪些厘米位置被遮挡。FSM 用它来判断取走、放回、移动、整理和位置漂移。

### 当前红外板配置

FSM 的红外配置在 `src/fsm/fsm.c` 的 `g_ir_boards[]`：

| 板位 | TCA9555 地址 | 层 | 覆盖范围 | 当前状态 |
|---|---:|---:|---:|---|
| `upper_left` | `0x21` | 上层 `0` | `0.0-19.0 cm` | 预留 |
| `upper_right` | `0x20` | 上层 `0` | `19.0-38.0 cm` | 已接入 |
| `lower_left` | `0x22` | 下层 `1` | `0.0-19.0 cm` | 预留 |
| `lower_right` | `0x24` | 下层 `1` | `19.0-38.0 cm` | 预留 |

一层书架按 `38 cm` 映射。单块红外板 16 点覆盖 `19 cm`，约 `1.1875 cm/点`。后续接入其他三块板时，只需要接好对应 TCA9555 地址并把 `enabled` 从 `0` 改成 `1`。

三根 74HC595 控制线当前共用：

| 信号 | Linux GPIO |
|---|---:|
| `SER` / `DATA` | `32` |
| `RCLK` / `LATCH` | `33` |
| `SRCLK` / `CLOCK` | `34` |

I2C 使用 `/dev/i2c-2`，和 SHT30/BH1750 共用。

### 操作窗口

红外读数很快，但数据库更新不会直接使用单帧：

```text
红外稳定帧: IR_STABLE_SCANS = 4
FSM tick: 40 ms
操作提交: 最后一次红外变化后 FSM_OP_SETTLE_SEC = 1 秒
```

因此红外变化约 `160 ms` 级别就会进入 FSM，书籍状态提交在最后一次红外变化稳定 `1 秒` 后进行。

屏幕红外点阵是另一条快路径：

```text
FSM 写 /tmp/fsm_state 的 ir*_bitmap
UI 每 150 ms 局部刷新红外点
```

它只用于人工观察，不等待 UHF、不等待数据库提交。

### 操作窗口内红外历史

取走书时常见现象是：

```text
书刚抽走 -> 原位置红外马上空出来
旁边书倒下/合拢 -> 又把红外挡住
UHF 稍后才确认缺读
```

所以 FSM 不只看提交瞬间的红外状态，还记录整个操作窗口内曾经发生过的变化：

| 字段 | 说明 |
|---|---|
| `op_seen_removed_ir[]` | 本次操作中曾经空出的红外点 |
| `op_seen_added_ir[]` | 本次操作中曾经新增遮挡的红外点 |

提交时使用：

```text
最终红外差异 OR 操作窗口历史差异
```

这样即使书倒下后重新遮挡，FSM 仍然能知道原位置曾经空出过。

### UHF 缺读保护

UHF 在拿书、放书、手遮挡时会短时漏读。FSM 的原则是：

```text
如果 UHF 说某本书不见了，
但这本书原位置没有红外空出证据，
先按 UHF 临时漏读处理，保留原位置。
```

当前策略：

- 单本缺读且原位置红外无空出：12 秒内保留原位置。
- 多本同时缺读且红外无空出：全部按临时漏读处理，不触发复杂取放。
- 红外有空出区域：按红外区域选择匹配的书标记为取走，可支持多本同时取走。
- 红外有新增区域：按固定书厚和历史顺序确定放回位置。

### 红外整理和夹缝过滤

红外单个灭点可能是书与书之间的夹缝，不一定是真空档。FSM 会把“左右都是亮点的单个灭点”补成亮点，用于推理。屏幕上仍显示原始点，便于人工观察。

如果红外亮区和数据库位置差太大，ACTIVE 状态下会周期性修正 `shelf_state`。但如果红外亮点明显太少，说明书可能没有推到足够靠里，FSM 不强行改数据库，只保留红外点给人工观察。

---

## 自检策略

| 模块 | 失败处理 |
|------|----------|
| UHF | 致命，进入 FAULT |
| 雷达 | 致命，进入 FAULT |
| 传感器 | 致命，进入 FAULT |
| 屏幕设备 | 默认致命；联调时可用 `--no-screen` 跳过 |
| LED | 致命前置检查失败会影响灯效，通常需处理 |
| WiFi | 非致命，本地模式继续运行 |
| MQTT | 非致命，本地模式继续运行 |

---

## 网络联动

FSM 通过 `src/wifi/wifi_manager.c` 管理 WiFi。屏幕网络页只写命令，不直接执行系统网络命令。

状态字段写入 `/tmp/fsm_state`：

| 字段 | 说明 |
|------|------|
| `wifi_hw` | 是否检测到 `wlan0` |
| `wifi_connected` | 是否已连接 |
| `wifi_ssid` | 当前 SSID |
| `wifi_ip` | 当前 IP |
| `wifi_signal` | RSSI |
| `wifi_error` | 最近错误/提示 |
| `mqtt_connected` | MQTT 是否在线 |

网络命令：

| 命令 | 说明 |
|------|------|
| `NET_SCAN` | 调用 `wpa_cli scan`，结果写 `/tmp/wifi_scan` |
| `NET_CONNECT ssid\|password` | 写 `/root/wpa.conf`，启动/重载 `wpa_supplicant`，并尝试 DHCP |
| `NET_DISCONNECT` | 断开当前 WiFi |

---

## 屏幕 IPC

屏幕进程不直接打开 UHF、雷达、I2C 或灯带。FSM 与屏幕通过两个文件通信：

| 文件 | 方向 | 说明 |
|------|------|------|
| `/tmp/fsm_state` | FSM → UI | 当前状态、查找状态、加书状态、待登记 EPC |
| `/tmp/sb_cmd` | UI → FSM | 查找、取消查找、开始加书、取消加书、登记新书 |

支持命令：

| 命令 | 说明 |
|------|------|
| `FIND <query>` | 按书名或 EPC 查找 |
| `FIND_CANCEL` | 取消查找 |
| `FORCE_STATE <BOOT|ACTIVE|SLEEP|FIND|FAULT>` | 强制切换 FSM 状态，供调试用 |
| `FORCE_WAKE` | 强制恢复到 ACTIVE |
| `ADD_START` | 进入加书模式，开始识别未知 EPC |
| `ADD_CANCEL` | 退出加书模式 |
| `REGISTER <layer> <start_cm> <end_cm> <title>` | 把待登记 EPC 写入 books 表 |

---

## 查找闭环

1. 屏幕写入 `FIND <query>`。
2. FSM 进入 `FIND`，匹配书籍并点亮黄灯。
3. 目标书被取走时，scanner 发出 `SCAN_EV_MISSING`。
4. FSM 写 `find_status=2` 到 `/tmp/fsm_state`。
5. 屏幕显示“已取走/已找到”反馈。
6. 用户取消或超时后返回进入 FIND 前的状态。

---

## 加书闭环

1. 屏幕写入 `ADD_START`。
2. FSM 只在此状态开启未知 EPC 检测。
3. 读到未登记 EPC 后写入 `/tmp/fsm_state` 的 `pending_epc` 和 `pending_layer`。
4. 屏幕填写书名、层、位置后写入 `REGISTER ...`。
5. FSM 写入 `books` 表，重新加载 scanner 书目，退出加书模式。

正常 ACTIVE/FIND 下读到未知 EPC 不处理，避免现场杂标签或串读造成误登记。

---

## 编译部署

```bash
cd /home/elf/elf3506-smart-bookshelf/src/uhf
make uhf_scanner.o

cd ../fsm
make -B fsm_main
scp fsm_main root@192.168.0.232:/root/fsm_main
```

板端启动：

```bash
ssh root@192.168.0.232 "nohup /root/fsm_main -v --no-screen > /tmp/fsm_main.log 2>&1 &"
ssh root@192.168.0.232 "nohup /root/SmartBookshelf > /tmp/SmartBookshelf.log 2>&1 &"
```

安全停止：

```bash
ssh root@192.168.0.232 "killall SmartBookshelf 2>/dev/null || true"
ssh root@192.168.0.232 "sh /root/stop_fsm.sh"
```

不要直接 `kill -9` FSM。正常退出会向 UHF 模块发送 STOP，降低 CH341 卡死概率。

---

## 常用调试

查看状态：

```bash
ssh root@192.168.0.232 "cat /tmp/fsm_state"
```

查看日志：

```bash
ssh root@192.168.0.232 "tail -n 80 /tmp/fsm_main.log"
ssh root@192.168.0.232 "tail -n 80 /tmp/SmartBookshelf.log"
```

手动发屏幕命令：

```bash
ssh root@192.168.0.232 "printf 'FIND 线代\n' > /tmp/sb_cmd"
ssh root@192.168.0.232 "printf 'FIND_CANCEL\n' > /tmp/sb_cmd"
ssh root@192.168.0.232 "printf 'ADD_START\n' > /tmp/sb_cmd"
```

---

## 关键文件

| 文件 | 说明 |
|------|------|
| `fsm.h` | 硬件路径、功率、状态结构、公共 API |
| `fsm.c` | 自检、状态流、屏幕 IPC、传感器、MQTT、事件处理 |
| `main.c` | 入口、参数解析、主循环 |
| `Makefile` | 交叉编译和部署 |
| `../uhf/uhf_scanner.c` | 双层 UHF 持续扫描和串读判层 |

---

*最后更新：2026-06-20*
