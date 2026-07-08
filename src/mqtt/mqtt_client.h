/**
 * MQTT 3.1.1 客户端 — 自包含实现
 *
 * 零外部依赖，基于 POSIX socket + select。
 * 支持：CONNECT(用户+密码+LWT)、PUBLISH(QoS0/1)、SUBSCRIBE、PING 保活、自动重连。
 *
 * 用法:
 *   mqtt_ctx_t ctx;
 *   mqtt_init(&ctx, "tcp://example.com:1883", "board", "password", on_msg);
 *   mqtt_connect(&ctx);
 *   mqtt_subscribe(&ctx, "bookshelf/cmd/find", 1);
 *   mqtt_publish(&ctx, "bookshelf/status", json_str, 1, 0);
 *   while (1) { mqtt_poll(&ctx, 100); }
 *   mqtt_disconnect(&ctx);
 */

#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MQTT_MAX_TOPIC_LEN   128
#define MQTT_MAX_PAYLOAD_LEN 4096
#define MQTT_MAX_CLIENT_ID   64

/* ── 消息回调 ──────────────────────────────────── */

typedef void (*mqtt_msg_cb)(const char *topic, const char *payload, int payload_len,
                            void *user_data);

/* ── 客户端上下文 ──────────────────────────────── */

typedef struct {
    int         fd;                         /* TCP socket */
    char        broker_host[128];
    int         broker_port;
    char        client_id[MQTT_MAX_CLIENT_ID];
    char        username[64];
    char        password[64];

    /* LWT 遗嘱消息 */
    char        lwt_topic[MQTT_MAX_TOPIC_LEN];
    char        lwt_payload[MQTT_MAX_PAYLOAD_LEN];
    uint8_t     lwt_qos;
    uint8_t     lwt_retain;

    int         connected;
    uint16_t    next_packet_id;
    time_t      last_ping;
    int         keepalive_sec;

    /* 回调 */
    mqtt_msg_cb on_message;
    void       *user_data;

    /* 内部缓冲 */
    uint8_t     rx_buf[MQTT_MAX_PAYLOAD_LEN + 256];
    int         rx_len;
} mqtt_ctx_t;

/* ── API ───────────────────────────────────────── */

/**
 * 初始化上下文 (不建立连接)
 *
 * @param ctx       上下文
 * @param broker_url  "tcp://host:port" 格式
 * @param username   认证用户名
 * @param password   认证密码
 * @param cb         收到消息时的回调 (可为 NULL)
 * @param user_data  回调透传参数
 * @return 0=成功, -1=参数错误
 */
int  mqtt_init(mqtt_ctx_t *ctx, const char *broker_url,
               const char *username, const char *password,
               mqtt_msg_cb cb, void *user_data);

/**
 * 设置 LWT 遗嘱消息
 *
 * 必须在 mqtt_connect() 之前调用。
 * 当客户端异常断开时，Broker 自动发布此消息给订阅者。
 */
void mqtt_set_will(mqtt_ctx_t *ctx,
                   const char *topic, const char *payload,
                   uint8_t qos, uint8_t retain);

/**
 * 建立 TCP + MQTT CONNECT
 * @return 0=成功, -1=失败
 */
int  mqtt_connect(mqtt_ctx_t *ctx);

/**
 * 订阅主题
 * @param qos  0 或 1
 */
int  mqtt_subscribe(mqtt_ctx_t *ctx, const char *topic, uint8_t qos);

/**
 * 发布消息
 * @param qos     0=最多一次, 1=至少一次
 * @param retain  0=不保留, 1=Broker保留最后一条
 */
int  mqtt_publish(mqtt_ctx_t *ctx, const char *topic,
                  const void *payload, int payload_len,
                  uint8_t qos, uint8_t retain);

/**
 * 轮询 — 主循环中调用
 *
 * 处理: 接收 Broker 下发的消息 + 发送 PINGREQ 保活 + 触发回调
 * @param timeout_ms  每次 select 超时 (毫秒), 建议 50~200
 */
void mqtt_poll(mqtt_ctx_t *ctx, int timeout_ms);

/**
 * 断开连接 (发送 DISCONNECT + 关闭 socket)
 */
void mqtt_disconnect(mqtt_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MQTT_CLIENT_H */
