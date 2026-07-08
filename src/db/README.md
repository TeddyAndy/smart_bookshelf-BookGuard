# 本地数据库模块

> SQLite3 (amalgamation) | NAND + SD 双库 | 断电安全 | 自动归档清理

---

## 一、架构

```
┌─────────────────────────────────────────────┐
│  fsm_main                                   │
│    │                                         │
│    ├─ db_init(&local, "/root/bookshelf.db")  │  NAND (SPI Flash, UBIFS)
│    │   ├─ books          永久                │  快, 永远可用
│    │   ├─ shelf_state    永久 (18条)          │
│    │   ├─ book_log       最近 3 天            │
│    │   └─ sensor_log     最近 24 小时         │
│    │                                         │
│    └─ db_archive_open(&arc,                  │  SD 卡
│           "/mnt/sdcard/bookshelf/history.db") │  大容量, 可插拔
│        ├─ book_log       最近 90 天           │  不存在时降级运行
│        └─ sensor_log     最近 90 天           │
│                                              │
│    每 10 分钟: db_cleanup(local, arc)         │
│        迁移旧数据 NAND → SD                   │
│        清理过期数据                           │
└─────────────────────────────────────────────┘
```

## 二、文件

| 文件 | 说明 |
|------|------|
| `db.h` | API — 上下文/数据结构/CRUD/归档/清理 |
| `db.c` | 实现 |
| `sqlite3.h` / `sqlite3.c` | SQLite amalgamation (9MB, 直接编译) |
| `db_test.c` | 测试程序 |
| `Makefile` | 编译 |

## 三、四张表

### 3.1 books — 书籍目录

| 字段 | 类型 | 说明 |
|------|------|------|
| epc | TEXT PK | RFID 标签 EPC (hex) |
| title | TEXT | 书名 |
| author | TEXT | 作者 |
| cover_path | TEXT | 封面图路径 |
| expected_layer | INT | 预期所在层 0=上 1=下 |
| expected_start | REAL | 预期位置起点 cm |
| expected_end | REAL | 预期位置终点 cm |
| pages | INT | 书页数 (估算厚度) |
| registered_at | TEXT | 注册时间 |

**维护:** 手动 INSERT / 导入脚本。不自动删。约 18 条，几 KB。

### 3.2 shelf_state — 当前在架状态

| 字段 | 类型 | 说明 |
|------|------|------|
| epc | TEXT PK | EPC |
| title | TEXT | 书名 (冗余, 加速) |
| author | TEXT | 作者 (冗余) |
| layer | INT | 实际所在层 |
| start_cm | REAL | 实测位置 cm |
| end_cm | REAL | 实测位置 cm |
| rssi | INT | 信号强度 |
| is_present | INT | 0=不在架 1=在架 |
| last_seen | TEXT | 最后盘点时间 |

**维护:** FSM 实时 UPSERT。

| 时机 | 操作 |
|------|------|
| 基线通过 | 18 本全量写 `is_present=1` |
| 取书确认 | 更新 `is_present=0` |
| 放回确认 | 更新 `is_present=1` + `rssi` 当前值 |

约 18 条，几 KB。

### 3.3 book_log — 借还事件

| 字段 | 类型 | 说明 |
|------|------|------|
| id | INT PK | 自增 |
| timestamp | TEXT | 事件时间 |
| epc | TEXT | EPC |
| title | TEXT | 书名 |
| action | TEXT | `borrowed` / `returned` |
| layer | INT | 所在层 |
| start_cm | REAL | 位置 |
| end_cm | REAL | 位置 |
| rssi | INT | 信号强度 |
| detail | TEXT | 备注 |

**维护:** FSM 事件触发时 INSERT。

### 3.4 sensor_log — 传感器历史

| 字段 | 类型 | 说明 |
|------|------|------|
| id | INT PK | 自增 |
| timestamp | TEXT | 记录时间 |
| temperature | REAL | 温度 °C |
| humidity | REAL | 湿度 %RH |
| lux | REAL | 光照 lx |
| radar_state | INT | 0=无人 1=运动 2=静止 3=都有 |

**维护:** (待 FSM 接入, 将来每分钟 INSERT 一条)

## 四、双库迁移策略

### 4.1 保留天数

| 数据 | NAND (本地) | SD (归档) | SD 不可用时 NAND 兜底 |
|------|------------|----------|---------------------|
| books | 永久 | — | 永久 |
| shelf_state | 永久 | — | 永久 |
| book_log | 3 天 | 90 天 | 7 天 |
| sensor_log | 1 天 | 90 天 | 3 天 |

### 4.2 迁移流程 (每 10 分钟, `db_cleanup`)

```
1. 检测 SD 卡
   ├─ 可用 → 正常迁移
   └─ 不可用 → 跳过, NAND 用兜底天数保留

2. 正常迁移 (事务包裹, 原子):
   BEGIN TRANSACTION
   ATTACH SD 库 AS arc
   INSERT INTO arc.book_log  SELECT * FROM local WHERE ts < now-3d
   INSERT INTO arc.sensor_log SELECT * FROM local WHERE ts < now-1d
   COMMIT
   DETACH

3. 清理 NAND 过期:
   DELETE FROM book_log   WHERE ts < now-3d (SD不在时用7d)
   DELETE FROM sensor_log WHERE ts < now-1d (SD不在时用3d)

4. 清理 SD 过期:
   DELETE FROM book_log   WHERE ts < now-90d
   DELETE FROM sensor_log WHERE ts < now-90d
```

### 4.3 时间计算

```sql
datetime('now','-90 days','localtime')
```

- 基于系统时钟 (RTC 恢复), 日历天数, 不是运行时间
- 断电 → RTC 恢复时间 → 计算依然正确
- 极端情况: 系统时间错乱到 1970 年 → 所有记录都"不到期", **不删 = 安全**
- 极端情况: 系统时间跳到 2030 年 → 所有记录瞬间"过期", 迁移+清理一次清空
  - 但 NAND 仍保留 book_log 3天/sensor 1天, 不会全部丢失

### 4.4 断电安全分析

```
场景 A: 断电发生在 BEGIN...COMMIT 之间
  → SQLite WAL 自动回滚, 事务内所有操作撤销
  → NAND 完整, SD 完整, 零数据丢失

场景 B: 断电发生在 COMMIT 之后, DELETE 之前
  → SD 已有新数据, NAND 旧数据还在
  → 两份都有, 重复但不丢, 下次 IGNORE 跳过重复

场景 C: 断电发生在 DELETE 中途
  → 部分旧记录已删, 剩余没删
  → 没丢数据, 下次迁移接手继续

场景 D: SD 卡在迁移中被拔出
  → ATTACH/写入失败 → 事务回滚 → NAND 不受影响
```

**结论: 任何时间点, 任何操作中途断电, 零数据丢失。**

### 4.5 时间同步

日历清理依赖系统时钟。板子无 RTC 电池，断电时间归零 (1970)。

**同步流程：**

```
WiFi 连上热点 → fsm_tick 首轮检测到 time < 2024
  → 依次尝试 rdate -s:
      time.nist.gov
      time.cloudflare.com
      (后续: 云服务器自定义时间接口)
  → 成功后 DB 切回日历清理模式
  → 失败则继续行数兜底, 每轮重试直到成功
```

**时间无效时的行为：** `[db] ⚠️ 系统时钟无效, 改用行数兜底清理`

## 五、断电保护 (多层)

| 层 | 机制 | 效果 |
|----|------|------|
| SQLite WAL | 写前日志, 崩溃自动恢复 | 写入中途断电 → 回滚到上一个检查点 |
| `PRAGMA synchronous=FULL` | 每次 commit 立刻 fsync | 确认写入 = 真正落到 NAND 闪存 |
| `PRAGMA journal_mode=WAL` | 写前日志模式 | 读写并发 + 崩溃恢复 |
| `PRAGMA wal_autocheckpoint=1000` | 每 1000 页合并 WAL | 控制 WAL 文件不无限变大 |
| `PRAGMA busy_timeout=3000` | 等锁 3 秒 | 多进程并发访问安全 |
| UBIFS 文件系统 | NAND 日志文件系统 | 文件系统层防掉电损坏 |
| 事务包裹迁移 | BEGIN/COMMIT 原子性 | 多步迁移要么全做要么全不做 |

### 手动恢复

```bash
# 完整性检查
sqlite3 /root/bookshelf.db "PRAGMA integrity_check;"

# 强制合并 WAL 到主文件
sqlite3 /root/bookshelf.db "PRAGMA wal_checkpoint(TRUNCATE);"

# 从 SD 归档恢复 (极端情况)
cp /mnt/sdcard/bookshelf/history.db /root/bookshelf.db.bak
```

## 六、空间管理

### 预估大小

| 数据 | 增长速度 | NAND 稳态 | SD 稳态 |
|------|----------|----------|---------|
| books | 0 | ~5 KB | — |
| shelf_state | 0 | ~5 KB | — |
| book_log | ~5 条/天 | ~100 KB | ~3 MB |
| sensor_log | ~1条/分钟(ACTIVE) | ~500 KB | ~40 MB |
| **合计** | | **~600 KB** | **~43 MB** |

### 大小监控

启动时 + 每次归档时调用 `db_check_size()`:
- NAND 库 > 10MB → stderr 告警
- SD 库 > 10MB → stderr 告警

```c
void db_check_size(const char *local_path, const char *archive_path);
```

### 紧急清理

```bash
# 手动清理所有过期数据
sqlite3 /root/bookshelf.db "DELETE FROM book_log WHERE timestamp < datetime('now','-3 days','localtime');"
sqlite3 /root/bookshelf.db "DELETE FROM sensor_log WHERE timestamp < datetime('now','-1 days','localtime');"
sqlite3 /root/bookshelf.db "VACUUM;"
```

## 七、SD 卡处理

| 场景 | 行为 |
|------|------|
| 启动时 SD 不存在 | ⚠️ 日志告警, 归档跳过, NAND 用兜底天数 |
| 启动时 SD 存在 | ✅ 自动建表, 正常归档 |
| 运行中 SD 被拔出 | 下个清理周期检测到 `archive->db==NULL`, 跳过 |
| SD 重新插入 | **不支持热插拔** — 需重启 FSM 重新打开 |
| SD 写满 | INSERT 失败, 日志告警, NAND 数据不丢 |

## 八、API 速查

```c
#include "db.h"

// ── 生命周期 ──
db_ctx_t local, archive;
db_init(&local, "/root/bookshelf.db");
db_archive_open(&archive, "/mnt/sdcard/bookshelf/history.db");  // 可失败
db_cleanup(&local, &archive);        // 迁移+清理
db_check_size(path, archive_path);   // 大小告警
db_close(&local);
db_close(&archive);

// ── 书籍目录 ──
db_book_upsert(&local, epc, title, author, layer, start, end);
db_book_find(&local, epc, &info);           // 返回 1=找到
db_book_list(&local, out, max_count);       // 列出全部
db_book_delete(&local, epc);
db_book_count(&local);

// ── 书架状态 ──
db_shelf_upsert(&local, epc, title, author, layer, start, end, rssi, present);
db_shelf_find(&local, epc, &item);
db_shelf_list_all(&local, out, max_count);
db_shelf_list_layer(&local, layer, out, max);
db_shelf_clear(&local);

// ── 操作日志 ──
db_log_add(&local, epc, title, "borrowed", layer, start, end, rssi, "");
db_log_recent(&local, 50, out, max);        // 最近 50 条

// ── 传感器 ──
db_sensor_add(&local, temp, hum, lux, radar_state);
db_sensor_recent(&local, 1440, out, 1440);  // 最近 1440 条 (~24h)
```

## 九、编译

```bash
cd src/db && make          # 编译 db_test
cd src/fsm && make         # FSM 自动链接 db.o + sqlite3.o
```

sqlite3.c 是 amalgamation (~9MB), Makefile 中单独编译不加 `-Wall` 避免海量第三方代码警告。

---

**最后更新**: 2026-06-17
