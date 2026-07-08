/**
 * MQTT 通信测试程序
 *
 * 功能:
 *   1. 连接 MQTT Broker
 *   2. 发布 bookshelf/status (上线通知 + LWT)
 *   3. 每 5 秒发布 bookshelf/sensors (模拟传感器)
 *   4. 发布一次 bookshelf/shelf (书架快照模拟)
 *   5. 订阅 bookshelf/cmd/find, bookshelf/cmd/scan
 *   6. Ctrl+C 退出
 *
 * 运行: ./mqtt_test [-h host] [-p port] [-u user] [-P password]
 */

#include "mqtt_client.h"
#include "json_builder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

/* 默认连接参数 */
#define BROKER_HOST   "example.com"
#define BROKER_PORT   1883
#define BROKER_USER   "board"
#define BROKER_PASS   "change-me"

static volatile int g_running = 1;

static void on_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

/* ── 消息回调: 收到小程序下发的命令 ──────────────── */

static void on_message(const char *topic, const char *payload, int payload_len,
                       void *user_data) {
    (void)user_data;
    printf("\n📩 [MQTT] 收到消息: %s\n", topic);
    printf("   Payload: %.*s\n", payload_len, payload);
}

/* ── 构建: bookshelf/status ──────────────────────── */

static void build_status_json(char *buf, int size, const char *ip) {
    json_builder_t jb;
    json_init(&jb, buf, size);
    json_obj_open(&jb, NULL);
        json_str(&jb, "status", "online");
        json_str(&jb, "version", "0.1.0");
        json_str(&jb, "ip", ip);
        json_int(&jb, "uptime", (int64_t)time(NULL));
    json_obj_close(&jb);
}

/* ── 构建: bookshelf/sensors ─────────────────────── */

static void build_sensors_json(char *buf, int size,
                               double temp, double hum, double lux,
                               const char *radar_state,
                               int move_cm, int still_cm) {
    char ts[32];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_info);

    json_builder_t jb;
    json_init(&jb, buf, size);
    json_obj_open(&jb, NULL);
        json_float(&jb, "temperature", temp);
        json_float(&jb, "humidity", hum);
        json_float(&jb, "lux", lux);
        json_obj_open(&jb, "radar");
            json_str(&jb, "state", radar_state);
            json_int(&jb, "move_dist_cm", move_cm);
            json_int(&jb, "still_dist_cm", still_cm);
        json_obj_close(&jb);
        json_str(&jb, "timestamp", ts);
    json_obj_close(&jb);
}

/* ── 构建: bookshelf/shelf (模拟 3 本书) ──────────── */

static void build_shelf_json(char *buf, int size) {
    char ts[32];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm_info);

    json_builder_t jb;
    json_init(&jb, buf, size);
    json_obj_open(&jb, NULL);
        json_str(&jb, "timestamp", ts);

        json_arr_open(&jb, "layers");

            /* ── 上层 ── */
            json_obj_open(&jb, NULL);
                json_str(&jb, "name", "上层");
                json_int(&jb, "index", 0);

                json_arr_open(&jb, "books");

                    json_obj_open(&jb, NULL);
                        json_int(&jb, "start_cm", 0);
                        json_int(&jb, "end_cm", 4);
                        json_str(&jb, "epc", "E280691500005029B7E1E88B");
                        json_str(&jb, "title", "复变函数");
                        json_str(&jb, "author", "刘太顺");
                        json_int(&jb, "rssi", 191);
                        json_bool(&jb, "present", 1);
                    json_obj_close(&jb);

                    json_obj_open(&jb, NULL);
                        json_int(&jb, "start_cm", 5);
                        json_int(&jb, "end_cm", 8);
                        json_str(&jb, "epc", "E280691500004029B7E1EC8B");
                        json_str(&jb, "title", "信号与系统");
                        json_str(&jb, "author", "奥本海姆");
                        json_int(&jb, "rssi", 185);
                        json_bool(&jb, "present", 1);
                    json_obj_close(&jb);

                    json_obj_open(&jb, NULL);
                        json_int(&jb, "start_cm", 9);
                        json_int(&jb, "end_cm", 38);
                        json_str(&jb, "epc", "");
                        json_str(&jb, "title", "");
                        json_str(&jb, "author", "");
                        json_int(&jb, "rssi", 0);
                        json_bool(&jb, "present", 0);
                    json_obj_close(&jb);

                json_arr_close(&jb);

                json_arr_open(&jb, "empty_zones");
                    json_obj_open(&jb, NULL);
                        json_int(&jb, "start_cm", 9);
                        json_int(&jb, "end_cm", 38);
                    json_obj_close(&jb);
                json_arr_close(&jb);

            json_obj_close(&jb);

        json_arr_close(&jb);  /* layers */

        json_int(&jb, "total_books", 2);
        json_int(&jb, "total_slots", 20);
    json_obj_close(&jb);
}

/* ================================================================ */

int main(int argc, char *argv[]) {
    const char *host = BROKER_HOST;
    int   port     = BROKER_PORT;
    const char *user = BROKER_USER;
    const char *pass = BROKER_PASS;

    /* 解析参数 */
    int opt;
    while ((opt = getopt(argc, argv, "h:p:u:P:")) != -1) {
        switch (opt) {
        case 'h': host = optarg; break;
        case 'p': port = atoi(optarg); break;
        case 'u': user = optarg; break;
        case 'P': pass = optarg; break;
        default:
            printf("Usage: %s [-h host] [-p port] [-u user] [-P password]\n", argv[0]);
            return 1;
        }
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    printf("╔══════════════════════════════════════════╗\n");
    printf("║  智能书架 MQTT 通信测试                 ║\n");
    printf("║  Broker: %s:%d                     ║\n", host, port);
    printf("║  User:   %s                            ║\n", user);
    printf("╚══════════════════════════════════════════╝\n\n");

    /* ── 初始化 MQTT ── */
    char url[256];
    snprintf(url, sizeof(url), "tcp://%s:%d", host, port);

    mqtt_ctx_t mqtt;
    if (mqtt_init(&mqtt, url, user, pass, on_message, NULL) < 0) {
        fprintf(stderr, "❌ mqtt_init 失败\n");
        return 1;
    }

    /* 设置 LWT 遗嘱: 异常断开时 Broker 自动发布 offline */
    mqtt_set_will(&mqtt,
        "bookshelf/status",
        "{\"status\":\"offline\"}",
        1, 0);

    /* 连接 */
    if (mqtt_connect(&mqtt) < 0) {
        fprintf(stderr, "❌ 连接失败\n");
        return 1;
    }

    /* 订阅命令 */
    mqtt_subscribe(&mqtt, "bookshelf/cmd/find", 1);
    mqtt_subscribe(&mqtt, "bookshelf/cmd/scan", 1);

    /* ── 1. 发布上线通知 ── */
    char json[4096];
    build_status_json(json, sizeof(json), "10.120.61.170");
    printf("\n📤 [bookshelf/status] %s\n", json);
    mqtt_publish(&mqtt, "bookshelf/status", json, strlen(json), 1, 0);

    /* ── 2. 发布书架快照 ── */
    build_shelf_json(json, sizeof(json));
    printf("\n📤 [bookshelf/shelf] %d bytes\n", (int)strlen(json));
    mqtt_publish(&mqtt, "bookshelf/shelf", json, strlen(json), 1, 0);

    /* ── 3. 定时发布传感器 + 心跳 ── */
    time_t last_sensor = 0;
    int sensor_seq = 0;

    printf("\n⏳ 运行中 (Ctrl+C 停止)...\n\n");

    while (g_running) {
        time_t now = time(NULL);

        /* 每 5 秒发布一次传感器 (实际部署改为 60s) */
        if (now - last_sensor >= 5) {
            last_sensor = now;
            sensor_seq++;

            /* 模拟传感器数据 */
            build_sensors_json(json, sizeof(json),
                               26.5 + (sensor_seq % 10) * 0.1,   /* 温度 */
                               55.0 + (sensor_seq % 5),          /* 湿度 */
                               120.0 + (sensor_seq % 20),        /* 光照 */
                               (sensor_seq % 3 == 0) ? "无人" :
                               (sensor_seq % 3 == 1) ? "静止" : "运动+静止",
                               (sensor_seq % 3 == 0) ? 0 : 120,
                               (sensor_seq % 3 == 0) ? 0 : 80);

            printf("📤 [bookshelf/sensors] #%d\n", sensor_seq);
            mqtt_publish(&mqtt, "bookshelf/sensors", json, strlen(json), 0, 0);
        }

        /* MQTT 轮询 (处理下发消息 + 保活 PING) */
        mqtt_poll(&mqtt, 200);
    }

    /* ── 清理 ── */
    printf("\n🛑 退出...\n");

    /* 发布 offline (可选，DISCONNECT 后 Broker 也会发 LWT) */
    mqtt_publish(&mqtt, "bookshelf/status",
                 "{\"status\":\"offline\"}", 21, 1, 0);
    usleep(200000);
    mqtt_disconnect(&mqtt);

    return 0;
}
