# WiFi 管理器

> wpa_supplicant 封装 — 扫描、连接、保存、状态查询

---

## 用途

为 LVGL UI 提供 WiFi 管理 API，实现类似手机的 WiFi 连接界面：
- 扫描附近热点
- 选择热点输入密码
- 连接并保存配置
- 查看当前连接状态

## 文件

| 文件 | 说明 |
|------|------|
| `wifi_manager.h` | API — 扫描/连接/状态/配置 |
| `wifi_manager.c` | 实现 — wpa_cli + wpa.conf |
| `README.md` | 本文档 |

## 依赖

- **wpa_supplicant** — 需运行中, 通过 wpa_cli 控制
- **wpa.conf** — 配置文件路径 `/root/wpa.conf`
- **wlan0** — 无线网卡接口

## API

```c
#include "wifi_manager.h"

// 状态
int wifi_is_running(void);                    // wpa_supplicant 是否运行
int wifi_is_connected(void);                  // 是否已关联热点
int wifi_get_status(wifi_status_t *out);      // 连接详情 (SSID/IP/信号)

// 扫描
int wifi_scan(wifi_ap_t *aps, int max);       // 扫描附近 AP (阻塞 3s)

// 连接
int wifi_connect(ssid, psk, save);            // 连接热点 (save=1 持久化)
int wifi_disconnect(void);                    // 断开

// 配置
int wifi_config_load(wifi_config_t *out);     // 读已保存配置
int wifi_config_save(wifi_config_t *cfg);     // 写配置
```

## 数据结构

```c
// 扫描结果
typedef struct {
    char ssid[64];
    char bssid[20];
    int  signal;       // dBm, 负数
    int  freq;         // MHz
    char flags[128];   // WPA-PSK-CCMP ...
} wifi_ap_t;

// 当前状态
typedef struct {
    int  connected;
    char ssid[64];
    char ip[32];
    int  signal;
} wifi_status_t;

// 配置
typedef struct {
    char ssid[64];
    char psk[64];
} wifi_config_t;
```

## 实现细节

### 扫描

```bash
wpa_cli -i wlan0 scan       # 触发扫描
sleep 3                      # 等待完成
wpa_cli -i wlan0 scan_results # 获取结果
```

### 临时连接 (不保存)

```bash
wpa_cli -i wlan0 add_network         # → 返回 network id
wpa_cli -i wlan0 set_network N ssid '"MyWiFi"'
wpa_cli -i wlan0 set_network N psk '"password"'
wpa_cli -i wlan0 select_network N
```

### 持久化连接 (保存)

写入 `/root/wpa.conf`:
```
ctrl_interface=/var/run/wpa_supplicant
network={
    scan_ssid=1
    ssid="MyWiFi"
    psk="password"
}
```

然后 `wpa_cli reconfigure` 重载。

### 状态查询

- 连接状态: 读 `/sys/class/net/wlan0/operstate`
- 详情: `wpa_cli status` + `wpa_cli signal_poll`

## FSM 集成

当前 FSM 的 `check_wifi` 已包含基础检测：
- 硬件存在 → 致命 (wlan0 接口是否存在)
- 热点连接 → 非致命 (已连则自动 rdate 对时)

完整的 WiFi 管理界面待 LVGL 屏幕接入后实现，届时直接调用本模块 API。

---

**最后更新**: 2026-06-17
