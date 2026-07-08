/**
 * CPH305F UHF RFID 驱动 — 实现
 *
 * 帧格式 (来自协议手册):
 *   | 52 46 | type | addr(2) | code | param_len(2) | [TLV…] | csum |
 *   type: 0x00=命令  0x01=响应  0x02=通知
 *
 * TLV 格式: | type(1) | len(1) | value(len) |
 *   0x01=EPC  0x05=RSSI  0x07=Status  0x20=FW版本  0x21=设备类型
 *   0x26=SingleParam  0x50=SingleTag(嵌套)
 *
 * 响应匹配规则:
 *   命令帧 type=0x00 code=X → 响应帧 type=0x01 code=X
 *   通知帧 type=0x02 code=0x80 → 与命令响应无关, 须跳过
 */
#include "uhf_driver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/time.h>
#include <time.h>

/* ── 帧常量 ─────────────────────────────────────────────── */

#define HDR1       0x52
#define HDR2       0x46
#define TYPE_CMD   0x00
#define TYPE_RESP  0x01
#define TYPE_NOTIFY 0x02
#define CODE_START 0x21
#define CODE_STOP  0x23
#define CODE_ONCE  0x22
#define CODE_INFO  0x40
#define CODE_SPARAM_SET 0x48
#define CODE_SPARAM_GET 0x49
#define CODE_NOTIFY_TAG 0x80

#define TLV_STATUS     0x07
#define TLV_EPC        0x01
#define TLV_RSSI       0x05
#define TLV_ANTENNA    0x0A
#define TLV_FW_VER     0x20
#define TLV_DEV_TYPE   0x21
#define TLV_SINGLE_PARAM 0x26
#define TLV_SINGLE_TAG 0x50

#define PARAM_POWER   0x01

#define BUF_SIZE      1024
#define FRAME_MAX     512
#define CMD_TIMEOUT   3000    /* ms, 总等待时间 */
#define POLL_INTERVAL 100     /* ms, 每次 recv 间隔 */

/* ── 帧对象 ─────────────────────────────────────────────── */

typedef struct {
    uint8_t  hdr[2];        /* 52 46 */
    uint8_t  type;          /* 0=CMD 1=RESP 2=NOTIFY */
    uint16_t addr;
    uint8_t  code;
    uint16_t param_len;
    uint8_t  params[FRAME_MAX];
    uint8_t  csum;
} frame_t;

/* ── 工具 ────────────────────────────────────────────────── */

static uint8_t checksum(const uint8_t *d, int n) {
    int s = 0; for (int i = 0; i < n; i++) s += d[i];
    return (~s) + 1;
}

static void hexdump(const char *tag, const uint8_t *d, int n) {
    fprintf(stderr, "[uhf] %s: ", tag);
    for (int i = 0; i < n; i++) fprintf(stderr, "%02X ", d[i]);
    fprintf(stderr, "\n");
}

/* ── 帧构建 ─────────────────────────────────────────────── */

static int frame_build(uint16_t addr, uint8_t code,
                       const uint8_t *tlv, int tlv_len,
                       uint8_t *out, int max) {
    (void)max;
    int p = 0;
    out[p++] = HDR1; out[p++] = HDR2;
    out[p++] = TYPE_CMD;
    out[p++] = (addr >> 8) & 0xFF; out[p++] = addr & 0xFF;
    out[p++] = code;
    out[p++] = (tlv_len >> 8) & 0xFF; out[p++] = tlv_len & 0xFF;
    if (tlv && tlv_len > 0) { memcpy(out + p, tlv, tlv_len); p += tlv_len; }
    out[p] = checksum(out, p); p++;
    return p;
}

/* ── 帧解析 ─────────────────────────────────────────────── */

static int frame_parse(const uint8_t *d, int n, frame_t *f) {
    if (n < 9) return -1;
    if (d[0] != HDR1 || d[1] != HDR2) return -1;
    f->hdr[0] = d[0]; f->hdr[1] = d[1];
    f->type      = d[2];
    f->addr      = ((uint16_t)d[3] << 8) | d[4];
    f->code      = d[5];
    f->param_len = ((uint16_t)d[6] << 8) | d[7];
    if (f->param_len > FRAME_MAX) return -1;
    if (f->param_len > 0) memcpy(f->params, d + 8, f->param_len);
    int body = 8 + f->param_len;
    f->csum = d[body];
    if (checksum(d, body) != f->csum) return -1;
    return 0;
}

/* ── TLV 解析 ───────────────────────────────────────────── */

static int tlv_find(const uint8_t *d, int n, uint8_t want,
                    uint8_t *val, int max) {
    int off = 0;
    while (off + 2 <= n) {
        uint8_t t = d[off], l = d[off+1];
        if (off + 2 + l > n) break;
        if (t == want) {
            int cp = l < max ? l : max;
            if (val) memcpy(val, d + off + 2, cp);
            return l;
        }
        off += 2 + l;
    }
    return -1;
}

/* ── 串口 ────────────────────────────────────────────────── */

static int serial_open(const char *dev, int baud) {
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { perror("[uhf] open"); return -1; }

    struct termios t;
    if (tcgetattr(fd, &t) != 0) { perror("tcgetattr"); close(fd); return -1; }
    speed_t s = B115200;  /* 只支持 115200 */
    cfsetispeed(&t, s); cfsetospeed(&t, s);
    t.c_cflag &= ~(PARENB | CSTOPB | CSIZE | CRTSCTS);
    t.c_cflag |= CS8 | CREAD | CLOCAL;
    t.c_iflag &= ~(IXON | IXOFF | IXANY);
    t.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    t.c_oflag &= ~OPOST;
    t.c_cc[VMIN] = 0; t.c_cc[VTIME] = 1;
    if (tcsetattr(fd, TCSANOW, &t) != 0) { perror("tcsetattr"); close(fd); return -1; }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

static int serial_send(int fd, const uint8_t *d, int n) {
    int off = 0, retries = 5;
    while (off < n && retries-- > 0) {
        int w = write(fd, d + off, n - off);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { usleep(5000); continue; }
            perror("[uhf] write"); return -1;
        }
        off += w;
    }
    return (off == n) ? off : -1;
}

static int serial_recv(int fd, uint8_t *buf, int max, int ms) {
    fd_set rfds; struct timeval tv;
    FD_ZERO(&rfds); FD_SET(fd, &rfds);
    tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
    int r = select(fd + 1, &rfds, NULL, NULL, &tv);
    if (r < 0) return (errno == EINTR) ? 0 : -1;
    if (r == 0) return 0;
    int tot = 0;
    while (tot < max) {
        r = read(fd, buf + tot, max - tot);
        if (r < 0) { if (errno == EAGAIN) break; perror("[uhf] read"); return -1; }
        if (r == 0) break;
        tot += r;
    }
    return tot;
}

/* ═══════════════════════════════════════════════════════════
 * cmd_send_recv — 核心收发函数
 *
 * 发送命令帧, 循环接收直到拿到匹配的响应帧.
 * 规则:
 *   1. type=0x02(通知帧) → 跳过 (不是对我们的命令的回复)
 *   2. type=0x01(响应帧) 且 code==发送的code → 这就是我们要的
 *   3. 检查 Status TLV(0x07), 非零则错误
 * 最长等 CMD_TIMEOUT ms.
 * ═══════════════════════════════════════════════════════════ */

static int cmd_send_recv(int fd, uint8_t code,
                         const uint8_t *tlv, int tlv_len,
                         uint8_t *out_params, int *out_len,
                         int verbose) {
    uint8_t cmd[FRAME_MAX];
    int cmd_len = frame_build(0x0000, code, tlv, tlv_len, cmd, sizeof(cmd));
    if (serial_send(fd, cmd, cmd_len) < 0) return -1;
    if (verbose) hexdump("TX", cmd, cmd_len);

    int waited = 0;
    while (waited < CMD_TIMEOUT) {
        uint8_t buf[BUF_SIZE];
        int n = serial_recv(fd, buf, sizeof(buf), POLL_INTERVAL);
        if (n < 0) return -1;
        waited += POLL_INTERVAL;
        if (n == 0) continue;
        if (verbose) hexdump("RX", buf, n);

        /* 遍历 buf 中所有帧 */
        int off = 0;
        while (off + 9 <= n) {
            /* 找帧头 */
            while (off + 1 < n && (buf[off] != HDR1 || buf[off+1] != HDR2))
                off++;
            if (off + 9 > n) break;

            frame_t f;
            if (frame_parse(buf + off, n - off, &f) != 0) { off++; continue; }
            int fsz = 8 + f.param_len + 1;  /* 帧总长度 */
            off += fsz;

            /* 跳过通知帧 (模块主动上报的标签数据) */
            if (f.type == TYPE_NOTIFY) continue;

            /* 检查是否匹配的响应帧 */
            if (f.type != TYPE_RESP) continue;
            if (f.code != code) {
                if (verbose) fprintf(stderr, "[uhf] skip resp code=0x%02X want=0x%02X\n", f.code, code);
                continue;
            }

            /* 匹配! 检查状态 */
            uint8_t st;
            int sl = tlv_find(f.params, f.param_len, TLV_STATUS, &st, 1);
            if (sl > 0 && st != 0x00) {
                fprintf(stderr, "[uhf] cmd 0x%02X status=0x%02X\n", code, st);
                return -1;
            }

            if (out_params && out_len) {
                *out_len = f.param_len;
                if (f.param_len > 0) memcpy(out_params, f.params, f.param_len);
            }
            return 0;
        }
    }

    fprintf(stderr, "[uhf] no response for cmd 0x%02X after %dms\n", code, CMD_TIMEOUT);
    return -1;
}

/* ── 单参数 TLV ─────────────────────────────────────────── */

static int tlv_single_param(uint8_t ptype, uint8_t val, uint8_t *buf) {
    buf[0] = TLV_SINGLE_PARAM;  /* 0x26 */
    buf[1] = 0x02;              /* len=2: param_type(1) + value(1) */
    buf[2] = ptype;
    buf[3] = val;
    return 4;
}

static int tlv_query_param(uint8_t ptype, uint8_t *buf) {
    buf[0] = TLV_SINGLE_PARAM;
    buf[1] = 0x01;              /* len=1: just param_type */
    buf[2] = ptype;
    return 3;
}

/* ── 标签缓存 ────────────────────────────────────────────── */

static uhf_tag_t *tag_find(uhf_t *u, const uint8_t *epc, uint8_t elen) {
    for (int i = 0; i < u->tag_count; i++)
        if (u->tags[i].epc_len == elen && memcmp(u->tags[i].epc, epc, elen) == 0)
            return &u->tags[i];
    return NULL;
}

static int tag_add(uhf_t *u, const uint8_t *epc, uint8_t elen,
                   uint8_t rssi, uint8_t ant) {
    uhf_tag_t *t = tag_find(u, epc, elen);
    if (t) {
        t->rssi = rssi;
        t->last_seen = time(NULL);
        t->read_count++;
        return 0;
    }
    if (u->tag_count >= UHF_MAX_TAGS) return -1;
    t = &u->tags[u->tag_count];
    memset(t, 0, sizeof(*t));
    memcpy(t->epc, epc, elen);
    t->epc_len = elen;
    t->rssi = rssi;
    t->antenna = ant;
    t->first_seen = t->last_seen = time(NULL);
    t->read_count = 1;
    /* EPC → hex string */
    int pos = 0;
    for (int i = 0; i < elen && pos < (int)sizeof(t->epc_str) - 2; i++)
        pos += snprintf(t->epc_str + pos, 3, "%02X", epc[i]);
    u->tag_count++;
    if (u->on_tag) u->on_tag(t, u->on_tag_user);
    return 1;
}

/* ── 处理标签通知帧 (0x80) ──────────────────────────────── */

static void handle_notify(uhf_t *u, frame_t *f) {
    if (f->type != TYPE_NOTIFY || f->code != CODE_NOTIFY_TAG) return;

    /* 找 TLV 0x50 (Single Tag), 其内部嵌套 EPC/RSSI 等 */
    uint8_t stag[256]; int slen = 0;
    int off = 0;
    while (off + 2 <= (int)f->param_len) {
        uint8_t t = f->params[off], l = f->params[off+1];
        if (off + 2 + l > (int)f->param_len) break;
        if (t == TLV_SINGLE_TAG) { memcpy(stag, f->params + off + 2, l); slen = l; break; }
        off += 2 + l;
    }
    if (slen < 4) return;

    /* 解析嵌套 TLV */
    uint8_t epc[UHF_EPC_MAX] = {0}; int elen = 0;
    uint8_t rssi = 0, ant = 0;
    off = 0;
    while (off + 2 <= slen) {
        uint8_t t = stag[off], l = stag[off+1];
        if (off + 2 + l > slen) break;
        switch (t) {
        case TLV_EPC:
            elen = l < UHF_EPC_MAX ? l : UHF_EPC_MAX;
            memcpy(epc, stag + off + 2, elen);
            break;
        case TLV_RSSI:
            if (l >= 1) rssi = stag[off+2];
            break;
        case TLV_ANTENNA:
            if (l >= 1) ant = stag[off+2];
            break;
        }
        off += 2 + l;
    }
    if (elen > 0) tag_add(u, epc, elen, rssi, ant);
}

/* ═══════════════════════════════════════════════════════════
 * 公开 API
 * ═══════════════════════════════════════════════════════════ */

int uhf_init(uhf_t *u, const char *dev) {
    if (!u || !dev) return -1;
    memset(u, 0, sizeof(*u));
    snprintf(u->device, sizeof(u->device), "%s", dev);

    u->fd = serial_open(dev, 115200);
    if (u->fd < 0) return -1;

    /* 先盲发停止盘存 + 等待清空 (模块可能在上次盘存状态) */
    {
        uint8_t stop_cmd[32];
        int sl = frame_build(0x0000, CODE_STOP, NULL, 0, stop_cmd, sizeof(stop_cmd));
        serial_send(u->fd, stop_cmd, sl);
        usleep(200000);  /* 给模块 200ms 处理 */
        tcflush(u->fd, TCIOFLUSH);
    }

    /* 查设备信息 (0x40), 重试最多2次 */
    int retry;
    uint8_t rbuf[FRAME_MAX]; int rlen = 0;
    for (retry = 0; retry < 2; retry++) {
        if (cmd_send_recv(u->fd, CODE_INFO, NULL, 0, rbuf, &rlen, u->verbose) >= 0)
            break;
        tcflush(u->fd, TCIOFLUSH);
    }
    if (retry == 2) {
        close(u->fd); u->fd = -1; return -1;
    }

    /* 解析 FW 版本 (TLV 0x20) */
    uint8_t fw[4]; int fl = tlv_find(rbuf, rlen, TLV_FW_VER, fw, sizeof(fw));
    if (fl >= 3) memcpy(u->fw_ver, fw, 3);

    /* 读当前功率 (非致命) */
    if (uhf_get_power(u, &u->power) < 0) u->power = 26;

    return 0;
}

void uhf_deinit(uhf_t *u) {
    if (!u) return;
    if (u->fd >= 0) {
        if (u->scanning) uhf_inventory_stop(u);
        close(u->fd);
    }
    u->fd = -1; u->scanning = 0;
}

int uhf_set_power(uhf_t *u, uint8_t dbm) {
    if (!u || u->fd < 0) return -1;
    uint8_t tlv[8];
    int tl = tlv_single_param(PARAM_POWER, dbm, tlv);
    if (cmd_send_recv(u->fd, CODE_SPARAM_SET, tlv, tl, NULL, NULL, u->verbose) < 0)
        return -1;
    u->power = dbm;
    return 0;
}

int uhf_get_power(uhf_t *u, uint8_t *dbm) {
    if (!u || u->fd < 0 || !dbm) return -1;
    uint8_t tlv[8]; int tl = tlv_query_param(PARAM_POWER, tlv);
    uint8_t rbuf[FRAME_MAX]; int rlen;
    if (cmd_send_recv(u->fd, CODE_SPARAM_GET, tlv, tl, rbuf, &rlen, u->verbose) < 0)
        return -1;
    /* 找 TLV 0x26 (Single Param), value: [param_type, val] */
    uint8_t val[4]; int vl = tlv_find(rbuf, rlen, TLV_SINGLE_PARAM, val, sizeof(val));
    if (vl >= 2) { *dbm = val[1]; return 0; }
    return -1;
}

int uhf_inventory_start(uhf_t *u) {
    if (!u || u->fd < 0) return -1;
    if (cmd_send_recv(u->fd, CODE_START, NULL, 0, NULL, NULL, u->verbose) < 0)
        return -1;
    u->scanning = 1;
    return 0;
}

int uhf_inventory_stop(uhf_t *u) {
    if (!u || u->fd < 0) return -1;
    int r = cmd_send_recv(u->fd, CODE_STOP, NULL, 0, NULL, NULL, u->verbose);
    if (r == 0) u->scanning = 0;
    return r;
}

int uhf_inventory_once(uhf_t *u, int timeout_ms) {
    if (!u || u->fd < 0) return -1;
    /* 发 0x22 (单次盘存) */
    uint8_t cmd[32]; int cl = frame_build(0x0000, CODE_ONCE, NULL, 0, cmd, sizeof(cmd));
    if (serial_send(u->fd, cmd, cl) < 0) return -1;
    /* 等标签通知帧, 最长 timeout_ms */
    int waited = 0;
    while (waited < timeout_ms) {
        uint8_t buf[BUF_SIZE];
        int n = serial_recv(u->fd, buf, sizeof(buf), POLL_INTERVAL);
        if (n < 0) return -1;
        waited += POLL_INTERVAL;
        if (n == 0) continue;
        int off = 0;
        while (off + 9 <= n) {
            while (off + 1 < n && (buf[off] != HDR1 || buf[off+1] != HDR2)) off++;
            if (off + 9 > n) break;
            frame_t f;
            if (frame_parse(buf + off, n - off, &f) != 0) { off++; continue; }
            int fsz = 8 + f.param_len + 1; off += fsz;
            if (f.type == TYPE_NOTIFY) handle_notify(u, &f);
        }
    }
    return 0;
}

int uhf_poll(uhf_t *u, int timeout_ms) {
    if (!u || u->fd < 0) return -1;
    uint8_t buf[BUF_SIZE];
    int n = serial_recv(u->fd, buf, sizeof(buf), timeout_ms);
    if (n < 0) return -1;
    if (n == 0) return 0;

    int added = 0;
    int off = 0;
    while (off + 9 <= n) {
        while (off + 1 < n && (buf[off] != HDR1 || buf[off+1] != HDR2)) off++;
        if (off + 9 > n) break;
        frame_t f;
        if (frame_parse(buf + off, n - off, &f) != 0) { off++; continue; }
        int fsz = 8 + f.param_len + 1; off += fsz;
        if (f.type == TYPE_NOTIFY) {
            int before = u->tag_count;
            handle_notify(u, &f);
            if (u->tag_count > before) added++;
        }
    }
    return added;
}

void uhf_tags_clear(uhf_t *u) { if (u) u->tag_count = 0; }

int uhf_tag_index(uhf_t *u, const char *epc_str) {
    if (!u || !epc_str) return -1;
    for (int i = 0; i < u->tag_count; i++)
        if (strcasecmp(u->tags[i].epc_str, epc_str) == 0) return i;
    return -1;
}

int uhf_tag_count(uhf_t *u) { return u ? u->tag_count : 0; }

void uhf_on_tag(uhf_t *u, uhf_on_tag_fn fn, void *user) {
    if (u) { u->on_tag = fn; u->on_tag_user = user; }
}

void uhf_verbose(uhf_t *u, int on) { if (u) u->verbose = on; }
