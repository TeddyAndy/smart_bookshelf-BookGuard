/**
 * HLK-LD2410C 毫米波雷达驱动库 — 实现
 *
 * 协议 V1.09, 基于 ld2410c_test.c 重构
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <time.h>
#include "ld2410c.h"

/* Linux termios2 支持任意波特率 */
#ifndef TCGETS2
#define TCGETS2  _IOR('T', 0x2A, struct termios2)
#endif
#ifndef TCSETS2
#define TCSETS2  _IOC(_IOC_WRITE, 'T', 0x2B, sizeof(struct termios2))
#endif
#ifndef BOTHER
#define BOTHER   0010000
#endif
#ifndef CBAUD
#define CBAUD    0010017
#endif

struct termios2 {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_line;
    cc_t     c_cc[19];
    speed_t  c_ispeed;
    speed_t  c_ospeed;
};

/* ================================================================
 * 协议标记
 * ================================================================ */

#define FRAME_HEADER_0  0xF4
#define FRAME_HEADER_1  0xF3
#define FRAME_HEADER_2  0xF2
#define FRAME_HEADER_3  0xF1
#define FRAME_FOOTER_0  0xF8
#define FRAME_FOOTER_1  0xF7
#define FRAME_FOOTER_2  0xF6
#define FRAME_FOOTER_3  0xF5

#define CMD_HEADER_0    0xFD
#define CMD_HEADER_1    0xFC
#define CMD_HEADER_2    0xFB
#define CMD_HEADER_3    0xFA
#define CMD_FOOTER_0    0x04
#define CMD_FOOTER_1    0x03
#define CMD_FOOTER_2    0x02
#define CMD_FOOTER_3    0x01

#define INNER_HEADER    0xAA
#define INNER_TAIL      0x55
#define INNER_CHECK     0x00
#define TYPE_ENGINEERING 0x01
#define TYPE_BASIC       0x02
#define MAX_FRAME_SIZE   128

/* 命令码 */
#define CMD_ENABLE_CONFIG  0x00FF
#define CMD_END_CONFIG     0x00FE
#define CMD_ENGINEER_ON    0x0062
#define CMD_ENGINEER_OFF   0x0063
#define CMD_READ_VERSION   0x00A0
#define CMD_FACTORY_RESET  0x00A2
#define CMD_RESTART        0x00A3
#define CMD_MAX_DISTANCE   0x0060
#define CMD_SENSITIVITY    0x0064
#define CMD_NOISE_STATUS   0x001B

#define CLEAR(p) memset((p), 0, sizeof(*(p)))

/* ================================================================
 * 辅助
 * ================================================================ */

static inline void le16(uint8_t *buf, uint16_t val)
{
    buf[0] = val & 0xFF;
    buf[1] = (val >> 8) & 0xFF;
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

const char *ld2410c_state_name(uint8_t state)
{
    switch (state) {
        case LD2410C_TARGET_NONE:            return "无人";
        case LD2410C_TARGET_MOVING:          return "运动";
        case LD2410C_TARGET_STATIONARY:      return "静止";
        case LD2410C_TARGET_BOTH:            return "运动+静止";
        case LD2410C_TARGET_NOISE_DETECTING: return "底噪检测中";
        case LD2410C_TARGET_NOISE_SUCCESS:   return "底噪完成";
        case LD2410C_TARGET_NOISE_FAILED:    return "底噪失败";
        default:                             return "未知";
    }
}

void ld2410c_set_verbose(ld2410c_ctx_t *ctx, int verbose)
{
    if (ctx) ctx->verbose = verbose;
}

/* ================================================================
 * UART 操作
 * ================================================================ */

static int uart_open(const char *device, int baud_rate)
{
    int fd;
    struct termios2 tio;

    fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        perror("open UART");
        return -1;
    }

    if (ioctl(fd, TCGETS2, &tio) != 0) {
        perror("TCGETS2");
        close(fd);
        return -1;
    }

    tio.c_cflag &= ~CBAUD;
    tio.c_cflag |= BOTHER;
    tio.c_ispeed = baud_rate;
    tio.c_ospeed = baud_rate;

    tio.c_cflag &= ~PARENB;
    tio.c_cflag &= ~CSTOPB;
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;
    tio.c_cflag &= ~CRTSCTS;
    tio.c_cflag |= CLOCAL | CREAD;

    tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);
    tio.c_iflag &= ~(ICRNL | INLCR);
    tio.c_oflag &= ~OPOST;

    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 5;

    if (ioctl(fd, TCSETS2, &tio) != 0) {
        perror("TCSETS2");
        close(fd);
        return -1;
    }

    tcflush(fd, TCIOFLUSH);
    return fd;
}

static int uart_write_all(int fd, const uint8_t *data, int len)
{
    int total = 0;
    while (total < len) {
        int n = write(fd, data + total, len - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += n;
    }
    tcdrain(fd);
    return total;
}

/* ================================================================
 * 命令帧操作
 * ================================================================ */

static int send_command(int fd, uint16_t cmd_word,
                        const uint8_t *value, uint16_t value_len)
{
    uint8_t buf[MAX_FRAME_SIZE];
    int total_len = 4 + 2 + 2 + value_len + 4;

    buf[0] = CMD_HEADER_0; buf[1] = CMD_HEADER_1;
    buf[2] = CMD_HEADER_2; buf[3] = CMD_HEADER_3;
    le16(&buf[4], 2 + value_len);
    le16(&buf[6], cmd_word);
    if (value && value_len > 0) memcpy(&buf[8], value, value_len);
    buf[8 + value_len + 0] = CMD_FOOTER_0;
    buf[8 + value_len + 1] = CMD_FOOTER_1;
    buf[8 + value_len + 2] = CMD_FOOTER_2;
    buf[8 + value_len + 3] = CMD_FOOTER_3;

    return uart_write_all(fd, buf, total_len);
}

static int parse_ack(const uint8_t *buf, int len,
                     uint16_t *cmd_word, uint8_t *ack_value, int *ack_value_len)
{
    int data_len;
    if (len < 12) return -2;
    if (buf[0] != CMD_HEADER_0 || buf[1] != CMD_HEADER_1 ||
        buf[2] != CMD_HEADER_2 || buf[3] != CMD_HEADER_3)
        return -1;

    data_len = buf[4] | (buf[5] << 8);
    if (len < 4 + 2 + data_len + 4) return -2;
    if (buf[4 + 2 + data_len + 0] != CMD_FOOTER_0 ||
        buf[4 + 2 + data_len + 1] != CMD_FOOTER_1 ||
        buf[4 + 2 + data_len + 2] != CMD_FOOTER_2 ||
        buf[4 + 2 + data_len + 3] != CMD_FOOTER_3)
        return -2;

    *cmd_word = buf[6] | (buf[7] << 8);
    if (data_len > 4) {
        *ack_value_len = data_len - 4;
        if (*ack_value_len > 0 && ack_value)
            memcpy(ack_value, &buf[10], *ack_value_len);
    } else {
        *ack_value_len = 0;
    }
    return 0;
}

static int wait_for_ack(int fd, int timeout_ms, uint16_t *cmd_word,
                        uint8_t *value, int *value_len)
{
    uint8_t buf[MAX_FRAME_SIZE];
    int pos = 0;
    long long deadline = now_ms() + timeout_ms;

    while (now_ms() < deadline) {
        uint8_t ch;
        int n = read(fd, &ch, 1);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) { usleep(5000); continue; }

        if (pos == 0 && ch != CMD_HEADER_0) continue;
        if (pos == 1 && ch != CMD_HEADER_1) { pos = 0; continue; }
        if (pos == 2 && ch != CMD_HEADER_2) { pos = 0; continue; }
        if (pos == 3 && ch != CMD_HEADER_3) { pos = 0; continue; }

        buf[pos++] = ch;
        if (pos == 6) {
            int data_len = buf[4] | (buf[5] << 8);
            if (data_len > MAX_FRAME_SIZE - 12) { pos = 0; continue; }
        }
        if (pos >= 12) {
            int data_len = buf[4] | (buf[5] << 8);
            int total = 4 + 2 + data_len + 4;
            if (pos >= total)
                return parse_ack(buf, pos, cmd_word, value, value_len);
        }
    }
    return -2; /* 超时 */
}

static int cmd_with_ack(int fd, uint16_t cmd_word,
                        const uint8_t *value, uint16_t value_len,
                        uint8_t *ack_value, int *ack_value_len)
{
    uint16_t ack_cmd = 0;
    tcflush(fd, TCIFLUSH);
    if (send_command(fd, cmd_word, value, value_len) < 0) return -1;
    return wait_for_ack(fd, 2000, &ack_cmd, ack_value, ack_value_len);
}

static int enable_config(int fd)
{
    uint8_t buf[64]; int len = 0;
    return cmd_with_ack(fd, CMD_ENABLE_CONFIG,
                        (uint8_t[]){0x01, 0x00}, 2, buf, &len);
}

static int end_config(int fd)
{
    uint8_t buf[64]; int len = 0;
    return cmd_with_ack(fd, CMD_END_CONFIG, NULL, 0, buf, &len);
}

/* ================================================================
 * 数据帧解析
 * ================================================================ */

static int parse_data_payload(const uint8_t *payload, int payload_len,
                              ld2410c_data_t *data)
{
    int basic_len = 9;
    if (payload_len < 5) return -1;

    if (payload[0] != TYPE_BASIC && payload[0] != TYPE_ENGINEERING) return -1;
    if (payload[1] != INNER_HEADER) return -1;
    if (payload_len < basic_len + 4) return -1;

    data->target_state          = payload[2];
    data->moving_distance_cm    = payload[3] | (payload[4] << 8);
    data->moving_energy         = payload[5];
    data->still_distance_cm     = payload[6] | (payload[7] << 8);
    data->still_energy          = payload[8];
    data->detection_distance_cm = payload[9] | (payload[10] << 8);
    data->is_engineering        = (payload[0] == TYPE_ENGINEERING);

    if (data->is_engineering) {
        int eng_offset = 11;
        int i;
        if (payload_len < eng_offset + 2 + 9 + 9 + 1 + 1 + 2) {
            data->is_engineering = 0;
            return 0;
        }
        data->max_moving_gate = payload[eng_offset];
        data->max_still_gate  = payload[eng_offset + 1];
        for (i = 0; i < LD2410C_MAX_GATES; i++)
            data->moving_gate_energy[i] = payload[eng_offset + 2 + i];
        for (i = 0; i < LD2410C_MAX_GATES; i++)
            data->still_gate_energy[i] = payload[eng_offset + 2 + 9 + i];
        data->light_sensor  = payload[eng_offset + 2 + 9 + 9];
        data->out_pin_state = payload[eng_offset + 2 + 9 + 9 + 1];
    }
    return 0;
}

/**
 * 逐个字节喂入，尝试找到并解析一个完整数据帧
 * 返回: 1=解析到有效帧, 0=需要更多数据, -1=格式错误
 */
static int find_and_parse_frame(ld2410c_ctx_t *ctx, int fd)
{
    uint8_t ch;
    int n;

    while (1) {
        n = read(fd, &ch, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        if (n == 0) return 0; /* 无数据 */

        /* 寻找帧头 F4 F3 F2 F1 */
        if (ctx->rx_pos == 0 && ch != FRAME_HEADER_0) continue;
        if (ctx->rx_pos == 1 && ch != FRAME_HEADER_1) { ctx->rx_pos = 0; continue; }
        if (ctx->rx_pos == 2 && ch != FRAME_HEADER_2) { ctx->rx_pos = 0; continue; }
        if (ctx->rx_pos == 3 && ch != FRAME_HEADER_3) { ctx->rx_pos = 0; continue; }

        ctx->rx_buf[ctx->rx_pos++] = ch;

        /* 拿到长度域 */
        if (ctx->rx_pos == 6) {
            int data_len = ctx->rx_buf[4] | (ctx->rx_buf[5] << 8);
            if (data_len > MAX_FRAME_SIZE - 10) {
                ctx->stats.err_length++;
                ctx->rx_pos = 0;
                continue;
            }
        }

        /* 帧头4 + 长度2 + 数据N + 帧尾4 */
        if (ctx->rx_pos >= 12) {
            int data_len = ctx->rx_buf[4] | (ctx->rx_buf[5] << 8);
            int total = 4 + 2 + data_len + 4;

            if (ctx->rx_pos >= total) {
                int footer_ok =
                    (ctx->rx_buf[4 + 2 + data_len + 0] == FRAME_FOOTER_0 &&
                     ctx->rx_buf[4 + 2 + data_len + 1] == FRAME_FOOTER_1 &&
                     ctx->rx_buf[4 + 2 + data_len + 2] == FRAME_FOOTER_2 &&
                     ctx->rx_buf[4 + 2 + data_len + 3] == FRAME_FOOTER_3);

                ctx->stats.total_frames++;

                if (footer_ok) {
                    ld2410c_data_t tmp;
                    CLEAR(&tmp);
                    if (parse_data_payload(&ctx->rx_buf[6], data_len, &tmp) == 0) {
                        int check_pos = 6 + data_len - 2;
                        if (ctx->rx_buf[check_pos] == INNER_TAIL &&
                            ctx->rx_buf[check_pos + 1] == INNER_CHECK) {
                            ctx->stats.valid_frames++;

                            /* 检测状态变化 */
                            if (ctx->on_state_change &&
                                tmp.target_state != ctx->last_target_state) {
                                ctx->on_state_change(ctx->last_target_state,
                                                     tmp.target_state);
                            }
                            ctx->last_target_state = tmp.target_state;

                            /* 更新最新数据 */
                            ctx->latest = tmp;

                            /* 回调 */
                            if (ctx->on_data) ctx->on_data(&ctx->latest);

                            ctx->rx_pos = 0;
                            return 1;
                        } else {
                            ctx->stats.err_inner++;
                        }
                    }
                } else {
                    ctx->stats.err_footer++;
                }
                ctx->rx_pos = 0;
                return -1;
            }
        }

        if (ctx->rx_pos >= MAX_FRAME_SIZE) {
            ctx->stats.err_length++;
            ctx->rx_pos = 0;
        }
    }
}

/* ================================================================
 * 公共 API
 * ================================================================ */

int ld2410c_init(ld2410c_ctx_t *ctx, const char *device, int baud_rate)
{
    if (!ctx || !device) return -1;
    CLEAR(ctx);

    ctx->fd = -1;
    snprintf(ctx->device, sizeof(ctx->device), "%s", device);
    ctx->baud_rate = baud_rate > 0 ? baud_rate : LD2410C_DEFAULT_BAUD;

    ctx->fd = uart_open(ctx->device, ctx->baud_rate);
    if (ctx->fd < 0) {
        fprintf(stderr, "[ld2410c] 无法打开 %s\n", ctx->device);
        return -1;
    }

    if (ctx->verbose)
        printf("[ld2410c] 已打开 %s, %d bps\n", ctx->device, ctx->baud_rate);

    return 0;
}

void ld2410c_close(ld2410c_ctx_t *ctx)
{
    if (!ctx || ctx->fd < 0) return;
    if (ctx->verbose) printf("[ld2410c] 关闭 %s\n", ctx->device);
    close(ctx->fd);
    ctx->fd = -1;
}

int ld2410c_poll(ld2410c_ctx_t *ctx, int timeout_ms)
{
    fd_set rfds;
    struct timeval tv;
    int r;

    if (!ctx || ctx->fd < 0) return -1;

    if (timeout_ms < 0) timeout_ms = 0;
    FD_ZERO(&rfds);
    FD_SET(ctx->fd, &rfds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    r = select(ctx->fd + 1, &rfds, NULL, NULL, &tv);
    if (r <= 0) return 0;

    return find_and_parse_frame(ctx, ctx->fd);
}

const ld2410c_data_t *ld2410c_get_data(const ld2410c_ctx_t *ctx)
{
    if (!ctx || ctx->fd < 0) return NULL;
    return &ctx->latest;
}

const ld2410c_stats_t *ld2410c_get_stats(const ld2410c_ctx_t *ctx)
{
    if (!ctx || ctx->fd < 0) return NULL;
    return &ctx->stats;
}

/* ================================================================
 * 命令实现
 * ================================================================ */

int ld2410c_set_engineering_mode(ld2410c_ctx_t *ctx, int enable)
{
    uint16_t cmd = enable ? CMD_ENGINEER_ON : CMD_ENGINEER_OFF;
    uint8_t buf[64]; int len = 0;
    int ret;

    if (!ctx || ctx->fd < 0) return -1;

    if (enable_config(ctx->fd) < 0) return -1;
    ret = cmd_with_ack(ctx->fd, cmd, NULL, 0, buf, &len);
    if (ret == 0) {
        ctx->stats.acks_received++;
        ctx->is_engineering = enable;
    }
    end_config(ctx->fd);

    if (ctx->verbose)
        printf("[ld2410c] 工程模式: %s (ret=%d)\n", enable ? "开" : "关", ret);

    return ret;
}

int ld2410c_read_version(ld2410c_ctx_t *ctx, uint16_t *fw_type, uint16_t *fw_ver)
{
    uint8_t buf[64]; int len = 0;
    int ret;

    if (!ctx || ctx->fd < 0) return -1;

    if (enable_config(ctx->fd) < 0) return -1;
    ret = cmd_with_ack(ctx->fd, CMD_READ_VERSION, NULL, 0, buf, &len);
    if (ret == 0) {
        ctx->stats.acks_received++;
        if (len >= 4 && fw_type && fw_ver) {
            *fw_type = buf[0] | (buf[1] << 8);
            *fw_ver  = buf[2] | (buf[3] << 8);
        }
    }
    end_config(ctx->fd);
    return ret;
}

int ld2410c_set_max_gate(ld2410c_ctx_t *ctx, int gate_moving,
                         int gate_still, int unmanned_sec)
{
    uint8_t val[18];
    uint8_t buf[64]; int len = 0;
    int ret;

    if (!ctx || ctx->fd < 0) return -1;

    if (enable_config(ctx->fd) < 0) return -1;

    le16(&val[0], 0x0000);     le16(&val[2], gate_moving);
    val[4] = 0x00; val[5] = 0x00;
    le16(&val[6], 0x0001);     le16(&val[8], gate_still);
    val[10] = 0x00; val[11] = 0x00;
    le16(&val[12], 0x0002);    le16(&val[14], unmanned_sec);
    val[16] = 0x00; val[17] = 0x00;

    ret = cmd_with_ack(ctx->fd, CMD_MAX_DISTANCE, val, 18, buf, &len);
    if (ret == 0) ctx->stats.acks_received++;
    end_config(ctx->fd);

    if (ctx->verbose)
        printf("[ld2410c] 距离门: 运动=G%d 静止=G%d 无人=%ds (ret=%d)\n",
               gate_moving, gate_still, unmanned_sec, ret);

    return ret;
}

int ld2410c_set_sensitivity(ld2410c_ctx_t *ctx, uint8_t moving_sens,
                            uint8_t still_sens)
{
    uint8_t val[18];
    uint8_t buf[64]; int len = 0;
    int ret;

    if (!ctx || ctx->fd < 0) return -1;

    if (enable_config(ctx->fd) < 0) return -1;

    le16(&val[0], 0x0000);     le16(&val[2], 0xFFFF); /* 全部距离门 */
    val[4] = 0x00; val[5] = 0x00;
    le16(&val[6], 0x0001);     le16(&val[8], moving_sens);
    val[10] = 0x00; val[11] = 0x00;
    le16(&val[12], 0x0002);    le16(&val[14], still_sens);
    val[16] = 0x00; val[17] = 0x00;

    ret = cmd_with_ack(ctx->fd, CMD_SENSITIVITY, val, 18, buf, &len);
    if (ret == 0) ctx->stats.acks_received++;
    end_config(ctx->fd);

    if (ctx->verbose)
        printf("[ld2410c] 灵敏度: 运动=%d 静止=%d (ret=%d)\n",
               moving_sens, still_sens, ret);

    return ret;
}

int ld2410c_restart(ld2410c_ctx_t *ctx)
{
    if (!ctx || ctx->fd < 0) return -1;

    if (enable_config(ctx->fd) == 0) {
        uint8_t buf[64]; int len = 0;
        cmd_with_ack(ctx->fd, CMD_RESTART, NULL, 0, buf, &len);
    }
    /* 不依赖 ACK，直接发送 */
    send_command(ctx->fd, CMD_RESTART, NULL, 0);

    if (ctx->verbose) printf("[ld2410c] 已发送重启命令\n");

    /* 等待模块重启 */
    usleep(2000000);
    return 0;
}

int ld2410c_read_noise_status(ld2410c_ctx_t *ctx)
{
    uint8_t buf[64]; int len = 0;
    int ret;

    if (!ctx || ctx->fd < 0) return -1;

    if (enable_config(ctx->fd) < 0) return -1;
    ret = cmd_with_ack(ctx->fd, CMD_NOISE_STATUS, NULL, 0, buf, &len);
    if (ret == 0) ctx->stats.acks_received++;
    end_config(ctx->fd);

    if (ctx->verbose && ret == 0 && len >= 2) {
        uint16_t state = buf[0] | (buf[1] << 8);
        printf("[ld2410c] 底噪状态: %d\n", state);
    }

    return ret;
}
