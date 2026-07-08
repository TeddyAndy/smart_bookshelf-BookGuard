/**
 * MQTT 3.1.1 客户端实现
 *
 * 自包含: 仅依赖 POSIX socket + 标准 C 库。
 * 参考: MQTT Version 3.1.1 OASIS Standard
 */

#include "mqtt_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

/* ================================================================
 * MQTT 协议常量
 * ================================================================ */

#define MQTT_CONNECT       1
#define MQTT_CONNACK       2
#define MQTT_PUBLISH       3
#define MQTT_PUBACK        4
#define MQTT_SUBSCRIBE     8
#define MQTT_SUBACK        9
#define MQTT_PINGREQ      12
#define MQTT_PINGRESP     13
#define MQTT_DISCONNECT   14

#define MQTT_PROTO_NAME    "MQTT"
#define MQTT_PROTO_LEVEL   4        /* 3.1.1 */

/* ── 内部: 写入大端 uint16 ─────────────────────── */

static inline void w16(uint8_t *buf, uint16_t v) {
    buf[0] = (v >> 8) & 0xFF;
    buf[1] = v & 0xFF;
}

/* ── 内部: MQTT 剩余长度编码 ───────────────────── */

static int encode_remaining_length(uint8_t *buf, int length) {
    int pos = 0;
    do {
        uint8_t b = length % 128;
        length /= 128;
        if (length > 0) b |= 0x80;
        buf[pos++] = b;
    } while (length > 0);
    return pos;
}

/* ── 内部: MQTT 剩余长度解码 ───────────────────── */

static int decode_remaining_length(const uint8_t *buf, int max_len,
                                   int *out_len, int *consumed) {
    int value = 0, multiplier = 1;
    int i;
    for (i = 0; i < 4 && i < max_len; i++) {
        value += (buf[i] & 0x7F) * multiplier;
        multiplier *= 128;
        if (!(buf[i] & 0x80)) break;
    }
    if (i >= 4) return -1;  /* 格式错误 */
    if (consumed) *consumed = i + 1;
    if (out_len) *out_len = value;
    return 0;
}

/* ── 内部: 写 MQTT UTF-8 字符串 (2字节长度前缀) ── */

static int write_utf8_str(uint8_t *buf, const char *s) {
    int len = (int)strlen(s);
    w16(buf, (uint16_t)len);
    memcpy(buf + 2, s, len);
    return 2 + len;
}

/* ── 内部: 构建 CONNECT 包 ─────────────────────── */

static int build_connect(mqtt_ctx_t *ctx, uint8_t *buf, int buf_size) {
    int pos = 0;

    /* ── 固定头 ── */
    buf[pos++] = (MQTT_CONNECT << 4);  /* 无标志 */

    /* ── 可变头: 协议名 */
    uint8_t var[256];
    int vpos = 0;
    vpos += write_utf8_str(var + vpos, MQTT_PROTO_NAME);   /* "MQTT" */
    var[vpos++] = MQTT_PROTO_LEVEL;                         /* level 4 */

    /* ── 连接标志 ── */
    uint8_t flags = 0;
    flags |= (!!ctx->username[0]) << 7;   /* Username */
    flags |= (!!ctx->password[0]) << 6;   /* Password */
    flags |= (ctx->lwt_retain & 1) << 5;  /* Will Retain */
    flags |= (ctx->lwt_qos & 3) << 3;     /* Will QoS */
    flags |= (!!ctx->lwt_topic[0]) << 2;  /* Will Flag */
    flags |= (1 << 1);                     /* Clean Session */
    var[vpos++] = flags;

    /* Keep Alive */
    w16(var + vpos, (uint16_t)ctx->keepalive_sec);
    vpos += 2;

    /* Client ID */
    vpos += write_utf8_str(var + vpos, ctx->client_id);

    /* Will Topic + Message */
    if (ctx->lwt_topic[0]) {
        vpos += write_utf8_str(var + vpos, ctx->lwt_topic);
        vpos += write_utf8_str(var + vpos, ctx->lwt_payload);
    }

    /* Username */
    if (ctx->username[0])
        vpos += write_utf8_str(var + vpos, ctx->username);

    /* Password */
    if (ctx->password[0])
        vpos += write_utf8_str(var + vpos, ctx->password);

    /* ── 剩余长度 + 可变头 ── */
    pos += encode_remaining_length(buf + pos, vpos);
    if (pos + vpos > buf_size) return -1;
    memcpy(buf + pos, var, vpos);
    pos += vpos;

    return pos;
}

/* ── 内部: 构建 PUBLISH 包 ─────────────────────── */

static int build_publish(const char *topic, const void *payload, int payload_len,
                         uint8_t qos, uint8_t retain, uint16_t packet_id,
                         uint8_t *buf, int buf_size) {
    (void)buf_size;
    int pos = 0;
    uint8_t flags = 0;
    if (retain) flags |= 0x01;
    flags |= (qos & 3) << 1;

    buf[pos++] = (MQTT_PUBLISH << 4) | flags;

    int var_len = 0;
    var_len += 2 + (int)strlen(topic);  /* UTF-8 topic */
    if (qos > 0) var_len += 2;           /* Packet Identifier */
    var_len += payload_len;

    pos += encode_remaining_length(buf + pos, var_len);
    pos += write_utf8_str(buf + pos, topic);
    if (qos > 0) {
        w16(buf + pos, packet_id);
        pos += 2;
    }
    if (payload && payload_len > 0) {
        memcpy(buf + pos, payload, payload_len);
        pos += payload_len;
    }
    return pos;
}

/* ── 内部: 构建 SUBSCRIBE 包 ───────────────────── */

static int build_subscribe(uint16_t packet_id, const char *topic, uint8_t qos,
                           uint8_t *buf, int buf_size) {
    (void)buf_size;
    int pos = 0;
    buf[pos++] = (MQTT_SUBSCRIBE << 4) | 0x02;

    int var_len = 2;                            /* Packet Identifier */
    var_len += 2 + (int)strlen(topic);           /* Topic Filter */
    var_len += 1;                                /* Requested QoS */

    pos += encode_remaining_length(buf + pos, var_len);
    w16(buf + pos, packet_id); pos += 2;
    pos += write_utf8_str(buf + pos, topic);
    buf[pos++] = qos & 3;

    return pos;
}

/* ── 内部: 构建 PINGREQ ────────────────────────── */

static int build_pingreq(uint8_t *buf) {
    buf[0] = (MQTT_PINGREQ << 4);
    buf[1] = 0;
    return 2;
}

/* ── 内部: 构建 DISCONNECT ─────────────────────── */

static int build_disconnect(uint8_t *buf) {
    buf[0] = (MQTT_DISCONNECT << 4);
    buf[1] = 0;
    return 2;
}

/* ── 内部: 构建 PUBACK ─────────────────────────── */

static int build_puback(uint16_t packet_id, uint8_t *buf) {
    buf[0] = (MQTT_PUBACK << 4);
    buf[1] = 2;
    w16(buf + 2, packet_id);
    return 4;
}

/* ── 内部: 发送原始数据 ────────────────────────── */

static int raw_send(mqtt_ctx_t *ctx, const uint8_t *buf, int len) {
    if (ctx->fd < 0) return -1;
    int sent = 0;
    while (sent < len) {
        int n = (int)send(ctx->fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(10000);
                continue;
            }
            return -1;
        }
        sent += n;
    }
    return 0;
}

/* ── 内部: 解析并处理一条 MQTT 包 ──────────────── */

static int handle_packet(mqtt_ctx_t *ctx, const uint8_t *buf, int len) {
    if (len < 2) return -1;

    uint8_t pkt_type = buf[0] >> 4;

    int rem_len, consumed;
    if (decode_remaining_length(buf + 1, len - 1, &rem_len, &consumed) < 0)
        return -1;

    int hdr_len = 1 + consumed;
    int total = hdr_len + rem_len;
    if (total > len) return 0;  /* 需要更多数据 */

    switch (pkt_type) {

    case MQTT_CONNACK:
        if (rem_len >= 2 && buf[hdr_len + 1] == 0) {
            ctx->connected = 1;
            ctx->last_ping = time(NULL);
            printf("[MQTT] ✅ Broker 确认连接 (session_present=%d)\n",
                   buf[hdr_len]);
        } else {
            int code = rem_len >= 2 ? buf[hdr_len + 1] : -1;
            fprintf(stderr, "[MQTT] ❌ 连接被拒绝, 返回码=%d\n", code);
            ctx->connected = 0;
        }
        break;

    case MQTT_SUBACK:
        if (rem_len >= 2) {
            // uint16_t pid = (buf[hdr_len] << 8) | buf[hdr_len+1];
            int rc = rem_len >= 3 ? buf[hdr_len + 2] : -1;
            if (rc == 0 || rc == 1) {
                printf("[MQTT] ✅ 订阅成功 (QoS=%d)\n", rc);
            } else {
                fprintf(stderr, "[MQTT] ❌ 订阅失败, 返回码=0x%02X\n", rc);
            }
        }
        break;

    case MQTT_PUBLISH: {
        /* 解析 topic (UTF-8) */
        int tlen = (buf[hdr_len] << 8) | buf[hdr_len + 1];
        if (hdr_len + 2 + tlen > total) break;
        const char *topic = (const char *)(buf + hdr_len + 2);
        int qos = (buf[0] >> 1) & 0x03;
        int pid_offset = (qos > 0) ? 2 : 0;
        int payload_offset = hdr_len + 2 + tlen + pid_offset;
        int payload_len = total - payload_offset;
        if (payload_len < 0) payload_len = 0;

        const char *pload = payload_len > 0
                            ? (const char *)(buf + payload_offset) : "";

        /* QoS 1: 回复 PUBACK */
        if (qos == 1) {
            uint16_t pid = (buf[hdr_len + 2 + tlen] << 8)
                         |  buf[hdr_len + 2 + tlen + 1];
            uint8_t ack[4];
            int alen = build_puback(pid, ack);
            raw_send(ctx, ack, alen);
        }

        /* 触发回调 */
        if (ctx->on_message) {
            ctx->on_message(topic, pload, payload_len, ctx->user_data);
        }
        break;
    }

    case MQTT_PUBACK:
        /* QoS 1 publish 被 Broker 确认 */
        break;

    case MQTT_PINGRESP:
        /* Broker 响应 PINGREQ，无需处理 */
        break;

    default:
        break;
    }

    return total;  /* 返回消费的字节数 */
}

/* ================================================================
 * 公开 API
 * ================================================================ */

int mqtt_init(mqtt_ctx_t *ctx, const char *broker_url,
              const char *username, const char *password,
              mqtt_msg_cb cb, void *user_data) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;
    ctx->keepalive_sec = 60;
    ctx->on_message = cb;
    ctx->user_data = user_data;

    /* 解析 broker_url: "tcp://host:port" */
    const char *p = broker_url;
    if (strncmp(p, "tcp://", 6) == 0) p += 6;
    else if (strncmp(p, "mqtt://", 7) == 0) p += 7;

    const char *colon = strrchr(p, ':');
    if (colon) {
        size_t host_len = colon - p;
        if (host_len >= sizeof(ctx->broker_host)) host_len = sizeof(ctx->broker_host) - 1;
        memcpy(ctx->broker_host, p, host_len);
        ctx->broker_host[host_len] = '\0';
        ctx->broker_port = atoi(colon + 1);
    } else {
        snprintf(ctx->broker_host, sizeof(ctx->broker_host), "%s", p);
        ctx->broker_port = 1883;
    }
    if (ctx->broker_port <= 0 || ctx->broker_port > 65535)
        ctx->broker_port = 1883;

    /* 用户名 / 密码 */
    if (username) {
        snprintf(ctx->username, sizeof(ctx->username), "%s", username);
    }
    if (password) {
        snprintf(ctx->password, sizeof(ctx->password), "%s", password);
    }

    /* 生成 client_id: elf3506_<pid>_<rand> */
    snprintf(ctx->client_id, sizeof(ctx->client_id),
             "elf3506_%d_%04x", getpid(),
             (unsigned)(time(NULL) & 0xFFFF));

    return 0;
}

void mqtt_set_will(mqtt_ctx_t *ctx,
                   const char *topic, const char *payload,
                   uint8_t qos, uint8_t retain) {
    if (topic) {
        snprintf(ctx->lwt_topic, sizeof(ctx->lwt_topic), "%s", topic);
    }
    if (payload) {
        snprintf(ctx->lwt_payload, sizeof(ctx->lwt_payload), "%s", payload);
    }
    ctx->lwt_qos = qos;
    ctx->lwt_retain = retain;
}

int mqtt_connect(mqtt_ctx_t *ctx) {
    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }
    ctx->connected = 0;

    /* ── TCP connect ── */
    ctx->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->fd < 0) {
        perror("[MQTT] socket");
        return -1;
    }

    /* 设置 TCP_NODELAY (小包即时发送) */
    int one = 1;
    setsockopt(ctx->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    /* 设置非阻塞 */
    int flags = fcntl(ctx->fd, F_GETFL, 0);
    fcntl(ctx->fd, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)ctx->broker_port);

    /* DNS 解析 — 先检查是否已是 IP 地址, 跳过 gethostbyname 防阻塞 */
    addr.sin_addr.s_addr = inet_addr(ctx->broker_host);
    if (addr.sin_addr.s_addr == INADDR_NONE) {
        struct hostent *he = gethostbyname(ctx->broker_host);
        if (he && he->h_addrtype == AF_INET)
            memcpy(&addr.sin_addr, he->h_addr, he->h_length);
    }

    printf("[MQTT] 连接 %s:%d ...\n", ctx->broker_host, ctx->broker_port);

    int ret = connect(ctx->fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        perror("[MQTT] connect");
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }

    /* 等待连接完成 (最多 5 秒) */
    if (ret < 0 && errno == EINPROGRESS) {
        fd_set wfds;
        struct timeval tv = {2, 0};
        FD_ZERO(&wfds);
        FD_SET(ctx->fd, &wfds);
        ret = select(ctx->fd + 1, NULL, &wfds, NULL, &tv);
        if (ret <= 0) {
            fprintf(stderr, "[MQTT] 连接超时\n");
            close(ctx->fd);
            ctx->fd = -1;
            return -1;
        }
        /* 检查 socket 错误 */
        int so_err = 0;
        socklen_t slen = sizeof(so_err);
        getsockopt(ctx->fd, SOL_SOCKET, SO_ERROR, &so_err, &slen);
        if (so_err != 0) {
            fprintf(stderr, "[MQTT] 连接失败: %s\n", strerror(so_err));
            close(ctx->fd);
            ctx->fd = -1;
            return -1;
        }
    }

    printf("[MQTT] TCP 已连接\n");

    /* ── 发送 MQTT CONNECT ── */
    uint8_t pkt[512];
    int pkt_len = build_connect(ctx, pkt, sizeof(pkt));
    if (pkt_len < 0 || raw_send(ctx, pkt, pkt_len) < 0) {
        fprintf(stderr, "[MQTT] 发送 CONNECT 失败\n");
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }

    /* ── 等待 CONNACK (最多 5 秒) ── */
    time_t start = time(NULL);
    while (!ctx->connected && (time(NULL) - start) < 5) {
        fd_set rfds;
        struct timeval tv = {0, 200000};  /* 200ms */
        FD_ZERO(&rfds);
        FD_SET(ctx->fd, &rfds);
        int n = select(ctx->fd + 1, &rfds, NULL, NULL, &tv);
        if (n > 0) {
            uint8_t tmp[256];
            int nr = (int)recv(ctx->fd, tmp, sizeof(tmp), 0);
            if (nr > 0) {
                /* 追加到 rx_buf */
                if (ctx->rx_len + nr < (int)sizeof(ctx->rx_buf)) {
                    memcpy(ctx->rx_buf + ctx->rx_len, tmp, nr);
                    ctx->rx_len += nr;
                }
                /* 尝试解析 */
                int consumed = handle_packet(ctx, ctx->rx_buf, ctx->rx_len);
                if (consumed > 0) {
                    memmove(ctx->rx_buf, ctx->rx_buf + consumed,
                            ctx->rx_len - consumed);
                    ctx->rx_len -= consumed;
                }
            }
        }
    }

    if (!ctx->connected) {
        fprintf(stderr, "[MQTT] 等待 CONNACK 超时\n");
        close(ctx->fd);
        ctx->fd = -1;
        return -1;
    }

    ctx->last_ping = time(NULL);
    printf("[MQTT] ✅ 已连接 (client_id=%s)\n", ctx->client_id);
    return 0;
}

int mqtt_subscribe(mqtt_ctx_t *ctx, const char *topic, uint8_t qos) {
    if (!ctx->connected) return -1;
    uint16_t pid = ++ctx->next_packet_id;
    if (pid == 0) pid = ++ctx->next_packet_id;

    uint8_t pkt[256];
    int len = build_subscribe(pid, topic, qos, pkt, sizeof(pkt));
    if (len < 0 || raw_send(ctx, pkt, len) < 0) return -1;

    printf("[MQTT] 订阅: %s (QoS=%d)\n", topic, qos);
    return 0;
}

int mqtt_publish(mqtt_ctx_t *ctx, const char *topic,
                 const void *payload, int payload_len,
                 uint8_t qos, uint8_t retain) {
    if (!ctx->connected) return -1;
    if (payload_len < 0) payload_len = (int)strlen((const char *)payload);

    uint16_t pid = 0;
    if (qos > 0) {
        pid = ++ctx->next_packet_id;
        if (pid == 0) pid = ++ctx->next_packet_id;
    }

    uint8_t pkt[MQTT_MAX_PAYLOAD_LEN + 256];
    int len = build_publish(topic, payload, payload_len,
                            qos, retain, pid, pkt, sizeof(pkt));
    if (len < 0 || raw_send(ctx, pkt, len) < 0) return -1;
    return 0;
}

void mqtt_poll(mqtt_ctx_t *ctx, int timeout_ms) {
    if (ctx->fd < 0) return;

    /* ── 读取 ── */
    fd_set rfds;
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    FD_ZERO(&rfds);
    FD_SET(ctx->fd, &rfds);

    int n = select(ctx->fd + 1, &rfds, NULL, NULL, &tv);
    if (n > 0 && FD_ISSET(ctx->fd, &rfds)) {
        uint8_t tmp[512];
        int nr = (int)recv(ctx->fd, tmp, sizeof(tmp), 0);
        if (nr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            return;  /* 瞬态, 不是断连 */
        }
        if (nr <= 0) {
            /* 连接断开 */
            printf("[MQTT] ⚠️ 连接断开\n");
            ctx->connected = 0;
            close(ctx->fd);
            ctx->fd = -1;
            return;
        }

        /* 追加到缓冲区 */
        if (ctx->rx_len + nr < (int)sizeof(ctx->rx_buf)) {
            memcpy(ctx->rx_buf + ctx->rx_len, tmp, nr);
            ctx->rx_len += nr;
        }

        /* 循环处理所有完整包 */
        int max_loops = 10;
        while (ctx->rx_len > 1 && max_loops-- > 0) {
            int consumed = handle_packet(ctx, ctx->rx_buf, ctx->rx_len);
            if (consumed > 0) {
                memmove(ctx->rx_buf, ctx->rx_buf + consumed,
                        ctx->rx_len - consumed);
                ctx->rx_len -= consumed;
            } else {
                break;  /* 包不完整或解析错误 */
            }
        }
    }

    /* ── PING 保活 ── */
    time_t now = time(NULL);
    if (ctx->connected && (now - ctx->last_ping) >= ctx->keepalive_sec) {
        uint8_t ping[2];
        int plen = build_pingreq(ping);
        if (raw_send(ctx, ping, plen) == 0) {
            ctx->last_ping = now;
        }
    }
}

void mqtt_disconnect(mqtt_ctx_t *ctx) {
    if (ctx->fd >= 0) {
        if (ctx->connected) {
            uint8_t pkt[2];
            int len = build_disconnect(pkt);
            raw_send(ctx, pkt, len);
        }
        close(ctx->fd);
        ctx->fd = -1;
    }
    ctx->connected = 0;
    printf("[MQTT] 已断开\n");
}
