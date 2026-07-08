# UHF RFID 驱动 & 扫描器

CPH305F UHF RFID 模块，双层书架使用两个 USB CH341 串口模块。当前 FSM 使用稳定的 by-path 设备名，避免 `/dev/ttyUSB*` 顺序变化。

---

## 文件结构

| 文件 | 说明 |
|------|------|
| `uhf_driver.h` | 协议帧驱动 — init/deinit/power/start/stop/poll |
| `uhf_driver.c` | 实现 — 帧构建/TLV解析/标签缓存/非阻塞轮询 |
| `uhf_scanner.h` | 双层扫描器 — DB 驱动, 持续轮询 + last_seen |
| `uhf_scanner.c` | 实现 — 双模块管理/已知 EPC 匹配/取放事件/主读层与错放判断 |
| `uhf_cli.c` | 调试工具 — 设备信息/定时扫描/持续扫描 |
| `archive/` | 旧代码归档 |
| `Makefile` | 交叉编译 |

---

## 编译

```bash
make                  # 编译 uhf_cli
make uhf_scanner.o    # 编译扫描器库 (FSM 依赖)
make upload           # 上传到板子
```

---

## 工具使用

### uhf_cli — 调试工具

```bash
/root/test/uhf_cli -u /dev/ttyS1 -p 29 -i        # 设备信息
/root/test/uhf_cli -u /dev/ttyS1 -p 22 -t 5      # 扫 5 秒
/root/test/uhf_cli -u /dev/ttyS1 -p 22 -c        # 持续扫 (Ctrl+C 退出)
/root/test/uhf_cli -u /dev/ttyS1 -p 22 -n 10     # 扫到 10 个标签停止
```

---

## 库 API

### 驱动层 (`uhf_driver.h`)

```c
#include "uhf_driver.h"

uhf_t u;
uhf_init(&u, "/dev/ttyS1");
uhf_set_power(&u, 22);              // 10-33 dBm

uhf_inventory_start(&u);            // 开始盘存 (非阻塞)

while (running) {
    uhf_poll(&u, 20);               // 20ms 超时, 高频调用
    for (int i = 0; i < u.tag_count; i++)
        printf("%s RSSI=%u\n", u.tags[i].epc_str, u.tags[i].rssi);
}

uhf_inventory_stop(&u);
uhf_deinit(&u);
```

### 扫描器层 (`uhf_scanner.h`)

```c
#include "uhf_scanner.h"
#include "db.h"

db_ctx_t db;
db_init(&db, "/root/bookshelf.db");

scanner_t sc;
scanner_init(&sc, &db,
             "/dev/serial/by-path/platform-ff780000.usb-usb-0:1.2:1.0-port0",
             "/dev/serial/by-path/platform-ff780000.usb-usb-0:1.3:1.0-port0",
             18, 18);
sc.present_timeout_sec = 5;         // 5 秒未读到 = 取走

while (running) {
    scanner_poll(&sc);
    for (int e = 0; e < sc.event_count; e++) {
        if (sc.events[e].type == SCAN_EV_MISSING)
            printf("取走: %s\n", sc.events[e].title);
        if (sc.events[e].type == SCAN_EV_RETURNED)
            printf("放回/错放: %s actual=%d expected=%d\n",
                   sc.events[e].title,
                   sc.events[e].layer,
                   sc.events[e].expected_layer);
    }
    usleep(40000);
}

scanner_stop(&sc);
db_close(&db);
```

---

## 协议帧格式

```
| 52 46 | type | addr(2) | code | param_len(2) | [TLV...] | csum |
  header  type   addr     code   param_len      params    checksum

type: 0x00=命令  0x01=响应  0x02=通知
```

### 常用帧码

| 帧码 | 命令 | 说明 |
|------|------|------|
| 0x21 | 开始盘存 | 持续读取标签 |
| 0x23 | 停止盘存 | 停止持续盘存 |
| 0x40 | 查询设备信息 | FW版本/设备类型 |
| 0x48 | 设置单个参数 | TLV 0x26 |
| 0x49 | 查询单个参数 | TLV 0x26 |
| 0x80 | 标签通知 | type=0x02, 嵌套 TLV 0x50 |

---

## 当前书架参数

| 层 | FSM 设备 | 功率 | 说明 |
|----|------|:---:|------|
| 上层 | `/dev/serial/by-path/platform-ff780000.usb-usb-0:1.2:1.0-port0` | **18dBm** | 当前稳定读上层 10 本 |
| 下层 | `/dev/serial/by-path/platform-ff780000.usb-usb-0:1.3:1.0-port0` | **18dBm** | 当前稳定读下层 8 本 |

> 详见 [`docs/UHF-书架测试数据.md`](../../docs/UHF-%E4%B9%A6%E6%9E%B6%E6%B5%8B%E8%AF%95%E6%95%B0%E6%8D%AE.md)。目前不追求完全零串读，而是用软件判定主读层。

---

## 主读层与错放判断

scanner 维护每本已知书的 `expected_layer` 和 `actual_layer`：

- `expected_layer` 来自 `books.expected_layer`，表示这本书应该放在哪一层。
- `actual_layer` 来自双 UHF 读数判定，表示当前更可能在哪一层。
- 如果 `is_present=1` 且 `actual_layer != expected_layer`，FSM 写入 `shelf_state.layer=actual_layer`，UI 显示粉红色错放。

当前判层不是简单看单次 RSSI，而是三路证据：

- `read_count` 增量：本 tick 上下两个模块分别新增了多少次读取。
- 滚动分数 `layer_score[book][2]`：近期读数指数衰减后累计，避免单次抖动。
- 上下层 RSSI：只有差距足够明显时作为辅助证据。

旧逻辑在证据不够压倒性时会回退到 `expected_layer`，导致真实错放被吞掉。现在证据不足时保持上一轮 `actual_layer`；只有另一层连续稳定成为候选层，才触发一次 `SCAN_EV_RETURNED`，由 FSM 判断为 `returned` 或 `misplaced`。

关键参数：

| 参数 | 当前值 | 作用 |
|------|:---:|------|
| `return_confirm` | `10` | 取走后连续读到多少轮才确认放回，降低手持移动误判 |
| `missing_confirm` | `6` | 连续缺读多少轮确认取走 |
| `layer_change_confirm` | `8` | 已在架书主读层连续变化多少轮确认错放/归位 |
| 分数门槛 | `58%` 或领先 `>=3` | 判断某层读数是否明显占优 |
| RSSI 差值 | `>=6` | 两层同时读到时，RSSI 足够强才参与判层 |

日志中出现以下内容时，说明 scanner 已经判出错放：

```text
[scanner] misplaced return: 复变 expected=上 actual=下 score=... delta=... rssi=...
[FSM] 📥错放: 复变 L1 (层号不符) RSSI=...
```

如果移动书后仍没有错放，优先看 `/tmp/fsm_main.log` 中这几项：

- `score=上/下`：长期哪层读数更高。
- `delta=上/下`：当前 tick 哪层新增读数更多。
- `rssi=上/下`：两层信号是否接近。
- 如果三项都接近，说明物理串读仍然太强，软件只能保持上一轮层，避免误报。

---

## 关键注意事项

- **功率 1 字节直接 dBm** (10-33, CPH305F v5.3.5 实测)
- **RSSI 无符号数**，值越大信号越强
- **持续盘存 (0x21) 稳定**，不要用单次 (0x22)
- **不要反复 init/deinit** — 一次 init, 全程 poll
- **`cmd_send_recv` 跳过通知帧**，只接受 type=0x01 响应帧
- **SC16IS752 已废弃**，归档在 `archive/`
- **CH341 驱动**: 每次重启需 `insmod /root/ch341.ko`

---

## 当前状态

- 双模块通过 USB Hub + CH341 连接，FSM 使用 by-path 固定上下层。
- 驱动已加盲停+flush+重试，解决盘存残留导致 init 超时
- SC16IS752 方案已废弃，代码归档在 `archive/`

---

**最后更新**: 2026-06-20
