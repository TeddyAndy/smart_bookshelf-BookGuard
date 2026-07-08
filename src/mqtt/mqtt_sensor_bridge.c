/**
 * MQTT 传感器桥接 — 独立程序 (无需 FSM/RFID/LED)
 *
 * 当前连接: SHT30 温湿度 + BH1750 光照 (I2C2) + MIPI 屏幕
 * 功能: 读传感器 → JSON → MQTT publish → 小程序实时显示
 *
 * 编译: make mqtt_sensor_bridge && make upload_sensor
 * 运行: /root/mqtt_sensor_bridge [-v]
 *
 * 主题:
 *   publish: bookshelf/status, bookshelf/sensors, bookshelf/shelf, bookshelf/event, bookshelf/state
 *   subscribe: bookshelf/cmd/find, bookshelf/cmd/scan
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>

/* ── 我们的库 ── */
#include "mqtt_client.h"
#include "json_builder.h"
#include "../db/db.h"

/* ================================================================
 * 配置
 * ================================================================ */

#define BROKER_URL        "tcp://example.com:1883"
#define MQTT_USERNAME     "board"
#define MQTT_PASSWORD     "change-me"
#define SENSOR_PUB_SEC_DEF 60         /* 默认传感器发布间隔 */
#define STATUS_PUB_SEC     30         /* 心跳间隔 */
#define DB_PATH           "/root/bookshelf.db"
#define I2C_DEV           "/dev/i2c-2"

/* I2C 地址 */
#define BH1750_ADDR  0x23
#define SHT30_ADDR   0x44

/* BH1750 命令 */
#define BH1750_POWER_ON    0x01
#define BH1750_CONT_H_RES  0x10

/* SHT30 命令 */
#define SHT30_SINGLE_HIGH  0x2C06

/* ================================================================
 * 全局
 * ================================================================ */

static mqtt_ctx_t   g_mqtt;
static db_ctx_t     g_db;
static int          g_mqtt_connected = 0;
static int          g_db_ok = 0;
static int          g_running = 1;
static int          g_verbose = 0;
static int          g_i2c_fd = -1;
static time_t       g_start_time;

static float        g_temp = 25.0f;
static float        g_hum  = 60.0f;
static float        g_lux  = 300.0f;
static int          g_sensor_interval = SENSOR_PUB_SEC_DEF;  /* 可运行时切换 */

/* ================================================================
 * 工具
 * ================================================================ */

static void ts(char *buf, int size)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buf, size, "%Y-%m-%d %H:%M:%S", t);
}

static void get_ip(char *buf, int size)
{
    FILE *f = popen("ip addr show wlan0 2>/dev/null | grep 'inet ' | awk '{print $2}' | cut -d/ -f1", "r");
    if (f) {
        if (fgets(buf, size, f) && buf[0]) { buf[strcspn(buf, "\r\n")] = 0; pclose(f); return; }
        pclose(f);
    }
    strncpy(buf, "0.0.0.0", size);
}

static void on_signal(int s) { (void)s; g_running = 0; }

/* ================================================================
 * I2C 传感器 (内联实现, 不依赖 sensor_reader)
 * ================================================================ */

static int i2c_select(int fd, unsigned char addr)
{
    if (ioctl(fd, I2C_SLAVE, addr) < 0) { perror("I2C_SLAVE"); return -1; }
    return 0;
}

/* SHT30 CRC-8 */
static uint8_t sht30_crc8(const uint8_t *data, int len)
{
    const uint8_t POLY = 0x31;
    uint8_t crc = 0xFF;
    for (int j = 0; j < len; j++) {
        crc ^= data[j];
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x80) ? (crc << 1) ^ POLY : (crc << 1);
    }
    return crc;
}

static int sensor_init(void)
{
    g_i2c_fd = open(I2C_DEV, O_RDWR);
    if (g_i2c_fd < 0) {
        perror("open I2C");
        fprintf(stderr, "⚠️  传感器不可用, 使用默认值\n");
        return -1;
    }

    /* BH1750 上电 + 连续高分辨率模式 */
    if (i2c_select(g_i2c_fd, BH1750_ADDR) == 0) {
        unsigned char cmd = BH1750_POWER_ON;
        if (write(g_i2c_fd, &cmd, 1) < 0) { /* ignore */ }
        usleep(10000);
        cmd = BH1750_CONT_H_RES;
        if (write(g_i2c_fd, &cmd, 1) < 0) { /* ignore */ }
        usleep(180000);
        printf("[SENSOR] BH1750 就绪\n");
    } else {
        fprintf(stderr, "[SENSOR] BH1750 未检测到\n");
    }

    /* SHT30 快速验证 */
    if (i2c_select(g_i2c_fd, SHT30_ADDR) == 0) {
        printf("[SENSOR] SHT30 就绪\n");
    } else {
        fprintf(stderr, "[SENSOR] SHT30 未检测到\n");
    }

    return 0;
}

static int sensor_read(float *temp, float *hum, float *lux)
{
    if (g_i2c_fd < 0) return -1;

    /* ── SHT30 ── */
    if (i2c_select(g_i2c_fd, SHT30_ADDR) == 0) {
        unsigned char cmd[2] = { SHT30_SINGLE_HIGH >> 8, SHT30_SINGLE_HIGH & 0xFF };
        if (write(g_i2c_fd, cmd, 2) == 2) {
            usleep(25000);
            unsigned char buf[6];
            if (read(g_i2c_fd, buf, 6) == 6) {
                if (sht30_crc8(buf, 2) == buf[2] && sht30_crc8(buf + 3, 2) == buf[5]) {
                    unsigned short rt = (buf[0] << 8) | buf[1];
                    unsigned short rh = (buf[3] << 8) | buf[4];
                    *temp = -45.0f + 175.0f * ((float)rt / 65535.0f);
                    *hum  = 100.0f * ((float)rh / 65535.0f);
                    if (*hum > 100.0f) *hum = 100.0f;
                }
            }
        }
    }

    /* ── BH1750 ── */
    if (i2c_select(g_i2c_fd, BH1750_ADDR) == 0) {
        unsigned char buf[2];
        if (read(g_i2c_fd, buf, 2) == 2) {
            unsigned short raw = (buf[0] << 8) | buf[1];
            *lux = (float)raw / 1.2f;
        }
    }

    return 0;
}

/* ================================================================
 * MQTT 发布
 * ================================================================ */

static void pub_status(const char *status)
{
    if (!g_mqtt_connected) return;

    char ip[32]; get_ip(ip, sizeof(ip));
    char t[32]; ts(t, sizeof(t));

    char buf[512];
    json_builder_t jb;
    json_init(&jb, buf, sizeof(buf));
    json_obj_open(&jb, NULL);
        json_str(&jb, "status", status);
        json_str(&jb, "version", "0.4.1");
        json_str(&jb, "ip", ip);
        json_int(&jb, "uptime", (int64_t)(time(NULL) - g_start_time));
        json_str(&jb, "timestamp", t);
    json_obj_close(&jb);

    mqtt_publish(&g_mqtt, "bookshelf/status", buf, json_len(&jb), 1, 0);
    if (g_verbose) printf("📤 status: %s\n", status);
}

static void pub_sensors(void)
{
    if (!g_mqtt_connected) return;

    char t[32]; ts(t, sizeof(t));

    char buf[512];
    json_builder_t jb;
    json_init(&jb, buf, sizeof(buf));
    json_obj_open(&jb, NULL);
        json_float(&jb, "temperature", g_temp);
        json_float(&jb, "humidity", g_hum);
        json_float(&jb, "lux", g_lux);
        json_obj_open(&jb, "radar");
            json_str(&jb, "state", "未连接");
            json_int(&jb, "move_dist_cm", 0);
            json_int(&jb, "still_dist_cm", 0);
        json_obj_close(&jb);
        json_str(&jb, "timestamp", t);
    json_obj_close(&jb);

    mqtt_publish(&g_mqtt, "bookshelf/sensors", buf, json_len(&jb), 0, 0);
    if (g_verbose) printf("📤 sensors: %.1f°C %.1f%% %.0f lx\n", g_temp, g_hum, g_lux);
}

static void pub_shelf(void)
{
    if (!g_mqtt_connected || !g_db_ok) return;

    shelf_item_t items[64];
    int total = db_shelf_list_all(&g_db, items, 64);
    if (total < 0) total = 0;

    char t[32]; ts(t, sizeof(t));

    /* 极简版本 — 直接从 DB 读取, 不做位置计算 */
    char buf[4096];
    json_builder_t jb;
    json_init(&jb, buf, sizeof(buf));
    json_obj_open(&jb, NULL);
        json_str(&jb, "timestamp", t);
        json_arr_open(&jb, "layers");

        for (int l = 0; l < 2; l++) {
            json_obj_open(&jb, NULL);
                json_str(&jb, "name", l == 0 ? "上层" : "下层");
                json_int(&jb, "index", l);
                json_arr_open(&jb, "books");

                int layer_count = 0;
                for (int i = 0; i < total; i++) {
                    if (items[i].layer != l) continue;
                    layer_count++;
                    json_obj_open(&jb, NULL);
                        json_int(&jb, "start_cm", (int)items[i].start_cm);
                        json_int(&jb, "end_cm", (int)items[i].end_cm);
                        json_str(&jb, "epc", items[i].epc);
                        json_str(&jb, "title", items[i].title);
                        json_str(&jb, "author", items[i].author);
                        json_int(&jb, "rssi", items[i].rssi);
                        json_bool(&jb, "present", items[i].is_present);
                    json_obj_close(&jb);
                }

                json_arr_close(&jb);

                json_arr_open(&jb, "empty_zones");
                json_arr_close(&jb);

            json_obj_close(&jb);
        }

        json_arr_close(&jb);
        json_int(&jb, "total_books", total);
        json_int(&jb, "total_slots", 76);
    json_obj_close(&jb);

    mqtt_publish(&g_mqtt, "bookshelf/shelf", buf, json_len(&jb), 1, 1);  /* retain */
    if (g_verbose) printf("📤 shelf: %d 本书\n", total);
}

static void pub_event(const char *type, const char *epc,
                      const char *title, int layer, int rssi)
{
    if (!g_mqtt_connected) return;

    char t[32]; ts(t, sizeof(t));

    char buf[512];
    json_builder_t jb;
    json_init(&jb, buf, sizeof(buf));
    json_obj_open(&jb, NULL);
        json_str(&jb, "type", type);
        json_str(&jb, "epc", epc);
        json_str(&jb, "title", title);
        json_str(&jb, "author", "");
        json_int(&jb, "layer", layer);
        json_int(&jb, "start_cm", 0);
        json_int(&jb, "end_cm", 0);
        json_int(&jb, "rssi", rssi);
        json_str(&jb, "timestamp", t);
    json_obj_close(&jb);

    mqtt_publish(&g_mqtt, "bookshelf/event", buf, json_len(&jb), 1, 0);
    printf("📤 event: %s '%s'\n", type, title);
}

static void pub_state(const char *state_name)
{
    if (!g_mqtt_connected) return;

    char t[32]; ts(t, sizeof(t));

    char buf[256];
    json_builder_t jb;
    json_init(&jb, buf, sizeof(buf));
    json_obj_open(&jb, NULL);
        json_str(&jb, "state", state_name);
        json_str(&jb, "prev_state", "");
        json_str(&jb, "timestamp", t);
    json_obj_close(&jb);

    mqtt_publish(&g_mqtt, "bookshelf/state", buf, json_len(&jb), 1, 0);
    if (g_verbose) printf("📤 state: %s\n", state_name);
}

/* ================================================================
 * MQTT 消息回调 — 处理小程序命令
 * ================================================================ */

static void on_message(const char *topic, const char *payload,
                       int payload_len, void *user_data)
{
    (void)payload_len;
    (void)user_data;

    printf("📥 [%s]: %.200s\n", topic, payload ? payload : "");

    /* ── find: 查 DB 返回结果 ── */
    if (strcmp(topic, "bookshelf/cmd/find") == 0) {
        /* 解析 {"query":"xxx"} */
        const char *q = strstr(payload, "\"query\"");
        if (!q) q = strstr(payload, "query");
        if (q) {
            q = strchr(q, ':');
            if (q) {
                q++; while (*q == ' ' || *q == '"') q++;
                char query[128] = {0};
                int i = 0;
                while (*q && *q != '"' && *q != '}' && i < 127) query[i++] = *q++;

                if (query[0] && g_db_ok) {
                    printf("  🔍 查找: '%s'\n", query);
                    book_info_t all[64];
                    int count = db_book_list(&g_db, all, 64);
                    for (int j = 0; j < count; j++) {
                        if (strstr(all[j].title, query) ||
                            strstr(all[j].author, query) ||
                            strcasecmp(all[j].epc, query) == 0) {
                            /* 找到 — 检查是否在架 */
                            shelf_item_t item;
                            int present = 0;
                            if (db_shelf_find(&g_db, all[j].epc, &item) == 1)
                                present = item.is_present;
                            pub_event(present ? "found" : "borrowed",
                                      all[j].epc, all[j].title,
                                      all[j].expected_layer, 0);
                            printf("  ✅ %s (%s)\n", all[j].title,
                                   present ? "在架" : "已取出");
                            return;
                        }
                    }
                    printf("  ❌ 未找到\n");
                }
            }
        }
    }

    /* ── scan: 发布当前快照 ── */
    else if (strcmp(topic, "bookshelf/cmd/scan") == 0) {
        printf("  📋 远程盘点\n");
        pub_shelf();
    }

    /* ── config: 运行时切换参数 ── */
    else if (strcmp(topic, "bookshelf/cmd/config") == 0) {
        const char *p = strstr(payload, "\"sensor_interval\"");
        if (!p) p = strstr(payload, "sensor_interval");
        if (p) {
            p = strchr(p, ':');
            if (p) {
                int val = atoi(p + 1);
                if (val >= 1 && val <= 3600) {
                    g_sensor_interval = val;
                    printf("  ⚙️  传感器间隔 → %ds\n", val);
                }
            }
        }
    }
}

/* ================================================================
 * 主程序
 * ================================================================ */

int main(int argc, char *argv[])
{
    int verbose = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) verbose = 1;
        else if (!strcmp(argv[i], "-h")) {
            printf("Usage: mqtt_sensor_bridge [-v]\n");
            printf("  -v  打印每条发出的消息\n");
            return 0;
        }
    }
    g_verbose = verbose;

    printf("\n📡 智能书架 MQTT 传感器桥接 v0.4.1\n");
    printf("   Broker: %s\n", BROKER_URL);
    printf("   传感器: SHT30(0x44) + BH1750(0x23) @ %s\n", I2C_DEV);
    printf("   发布间隔: %ds (可运行时切换)\n\n", g_sensor_interval);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    g_start_time = time(NULL);

    /* ── 1. 传感器 ── */
    printf("[INIT] 传感器...\n");
    sensor_init();

    /* ── 2. 数据库 (可选) ── */
    printf("[INIT] 数据库...\n");
    if (db_init(&g_db, DB_PATH) == 0) {
        g_db_ok = 1;
        printf("[INIT] DB OK (%d 本书)\n", db_book_count(&g_db));
    } else {
        printf("[INIT] DB 不可用 (find/scan 命令受限)\n");
    }

    /* ── 3. MQTT ── */
    printf("[INIT] MQTT...\n");
    if (mqtt_init(&g_mqtt, BROKER_URL, MQTT_USERNAME, MQTT_PASSWORD,
                  on_message, NULL) < 0) {
        fprintf(stderr, "FATAL: mqtt_init\n");
        return 1;
    }

    /* LWT 遗嘱 */
    mqtt_set_will(&g_mqtt, "bookshelf/status",
                  "{\"status\":\"offline\"}", 1, 0);

    if (mqtt_connect(&g_mqtt) < 0) {
        fprintf(stderr, "FATAL: MQTT 连接失败 — 检查网络和 Broker\n");
        return 1;
    }
    g_mqtt_connected = 1;

    /* 订阅命令 */
    mqtt_subscribe(&g_mqtt, "bookshelf/cmd/find", 1);
    mqtt_subscribe(&g_mqtt, "bookshelf/cmd/scan", 1);
    mqtt_subscribe(&g_mqtt, "bookshelf/cmd/config", 1);

    /* 上线 */
    pub_status("online");
    pub_state("RUN");
    pub_sensors();
    if (g_db_ok) pub_shelf();

    printf("\n✅ 就绪, 开始循环 (Ctrl-C 退出)\n\n");

    /* ── 4. 主循环 ── */
    time_t last_sensor = 0;
    time_t last_status = 0;

    while (g_running) {
        time_t now = time(NULL);

        /* 读传感器 */
        sensor_read(&g_temp, &g_hum, &g_lux);

        /* ── 本地命令管道: echo "间隔N" > /tmp/sb_cmd ── */
        {
            static time_t last_cmd = 0;
            if (now - last_cmd >= 2) {
                last_cmd = now;
                FILE *f = fopen("/tmp/sb_cmd", "r");
                if (f) {
                    char cmd[64] = {0};
                    if (fgets(cmd, sizeof(cmd), f)) {
                        cmd[strcspn(cmd, "\r\n")] = 0;
                        int val = 0;
                        if (sscanf(cmd, "间隔%d", &val) == 1 || sscanf(cmd, "interval%d", &val) == 1) {
                            if (val >= 1 && val <= 3600) {
                                g_sensor_interval = val;
                                printf("⚙️  传感器间隔 → %ds (本地命令)\n", val);
                            }
                        } else if (strcmp(cmd, "now") == 0) {
                            pub_sensors();
                            printf("📤 即时发送 (本地命令)\n");
                        }
                    }
                    fclose(f);
                    f = fopen("/tmp/sb_cmd", "w"); if (f) fclose(f);
                }
            }
        }

        /* 定期发布传感器 (间隔可运行时切换) */
        if (now - last_sensor >= g_sensor_interval) {
            last_sensor = now;
            pub_sensors();
        }

        /* 定期心跳 */
        if (now - last_status >= STATUS_PUB_SEC) {
            last_status = now;
            pub_status("online");
        }

        /* MQTT poll */
        mqtt_poll(&g_mqtt, 200);

        /* 检测断线 */
        if (!g_mqtt.connected) {
            g_mqtt_connected = 0;
            printf("⚠️  MQTT 断开, %ds 后重试...\n", 10);
            sleep(10);
            if (mqtt_connect(&g_mqtt) == 0) {
                g_mqtt_connected = 1;
                mqtt_subscribe(&g_mqtt, "bookshelf/cmd/find", 1);
                mqtt_subscribe(&g_mqtt, "bookshelf/cmd/scan", 1);
                mqtt_subscribe(&g_mqtt, "bookshelf/cmd/config", 1);
                pub_status("online");
                pub_state("RUN");
                printf("✅ 重连成功\n");
            }
        }

        usleep(100000);  /* 100ms */
    }

    /* ── 5. 清理 ── */
    printf("\n[BYE] 正在下线...\n");
    pub_status("offline");
    usleep(200000);
    mqtt_disconnect(&g_mqtt);

    if (g_db_ok) db_close(&g_db);
    if (g_i2c_fd >= 0) close(g_i2c_fd);

    printf("Done.\n");
    return 0;
}
