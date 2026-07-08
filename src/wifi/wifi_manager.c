/**
 * WiFi 管理器 — 实现
 *
 * 依赖: wpa_supplicant (需运行中) + wpa_cli
 * 配置: /root/wpa.conf
 * 控制: wpa_cli -i wlan0 <command>
 */
#include "wifi_manager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

/* ── 配置路径 ────────────────────────────────────── */

#define WPA_CONF      "/root/wpa.conf"
#define WPA_CLI       "wpa_cli -i wlan0"
#define WPA_CTRL      "/var/run/wpa_supplicant/wlan0"

/* ── 内部: 执行命令并读输出 ──────────────────────── */

static int run_cmd(const char *cmd, char *out, int max) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    int total = 0;
    if (out && max > 0) {
        total = (int)fread(out, 1, max - 1, fp);
        if (total > 0) out[total] = '\0';
    }
    int rc = pclose(fp);
    return (rc == 0) ? total : -1;
}

static void wpa_escape(char *dst, size_t dst_sz, const char *src)
{
    size_t j = 0;
    if (!dst || dst_sz == 0) return;
    for (size_t i = 0; src && src[i] && j + 1 < dst_sz; ++i) {
        if ((src[i] == '\\' || src[i] == '"') && j + 2 < dst_sz) {
            dst[j++] = '\\';
        }
        dst[j++] = src[i];
    }
    dst[j] = '\0';
}

/* ── 内部: 解析 wpa_cli status 输出 ──────────────── */

static void parse_status(const char *raw, wifi_status_t *s) {
    if (!s) return;
    memset(s, 0, sizeof(*s));

    const char *p = raw;
    while (p && *p) {
        const char *end = strchr(p, '\n');
        size_t len = end ? (size_t)(end - p) : strlen(p);

        if (len >= 19 && strncmp(p, "wpa_state=COMPLETED", 19) == 0) {
            s->connected = 1;
        } else if (len > 5 && strncmp(p, "ssid=", 5) == 0) {
            size_t cp = len - 5;
            if (cp >= WIFI_SSID_MAX) cp = WIFI_SSID_MAX - 1;
            memcpy(s->ssid, p + 5, cp);
            s->ssid[cp] = '\0';
        } else if (len > 11 && strncmp(p, "ip_address=", 11) == 0) {
            size_t cp = len - 11;
            if (cp >= sizeof(s->ip)) cp = sizeof(s->ip) - 1;
            memcpy(s->ip, p + 11, cp);
            s->ip[cp] = '\0';
        }

        if (!end) break;
        p = end + 1;
    }
}

/* ── 内部: 检测是否有有效 IP ────────────────────── */

static int has_ip(void) {
    char buf[64] = {0};
    int fd = open("/sys/class/net/wlan0/operstate", O_RDONLY);
    if (fd < 0) return 0;
    read(fd, buf, sizeof(buf)-1);
    close(fd);
    return (strncmp(buf, "up", 2) == 0);
}

/* ═══════════════════════════════════════════════════
 * 公开 API
 * ═══════════════════════════════════════════════════ */

int wifi_is_running(void) {
    char buf[256] = {0};
    int rc = run_cmd("pidof wpa_supplicant 2>/dev/null", buf, sizeof(buf));
    return (rc > 0 && buf[0] != '\0');
}

int wifi_is_connected(void) {
    return has_ip();
}

int wifi_get_status(wifi_status_t *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));

    /* 接口不存在或未拉起时直接返回未连接 */
    if (!has_ip()) return 0;

    /* wpa_cli status 获取详情 */
    char buf[512] = {0};
    if (run_cmd(WPA_CLI " status 2>/dev/null", buf, sizeof(buf)) < 0)
        return 0;
    parse_status(buf, out);

    /* 信号强度 */
    char sig[32] = {0};
    if (run_cmd(WPA_CLI " signal_poll 2>/dev/null | grep RSSI", sig, sizeof(sig)) > 0) {
        /* 格式: RSSI=-45 */
        char *eq = strchr(sig, '=');
        if (eq) out->signal = atoi(eq + 1);
    }

    if (out->connected && !out->ip[0]) {
        char ip[64] = {0};
        if (run_cmd("ip -4 addr show wlan0 2>/dev/null | grep 'inet ' | awk '{print $2}' | cut -d/ -f1",
                    ip, sizeof(ip)) > 0) {
            ip[strcspn(ip, "\r\n")] = '\0';
            snprintf(out->ip, sizeof(out->ip), "%s", ip);
        }
    }

    return 0;
}

int wifi_scan(wifi_ap_t *aps, int max) {
    if (!aps || max <= 0) return -1;

    /* 触发扫描 */
    run_cmd(WPA_CLI " scan 2>/dev/null", NULL, 0);
    usleep(3000000);  /* 等 3 秒扫描完成 */

    /* 获取结果 */
    char buf[4096] = {0};
    if (run_cmd(WPA_CLI " scan_results 2>/dev/null", buf, sizeof(buf)) < 0)
        return -1;

    /* 解析: 跳过标题行, 逐行解析 */
    int count = 0;
    char *save_line = NULL;
    char *line = strtok_r(buf, "\n", &save_line);
    int skip_header = 1;

    while (line && count < max) {
        char *save_col = NULL;
        if (skip_header) { skip_header = 0; line = strtok_r(NULL, "\n", &save_line); continue; }

        /* 格式: bssid freq signal flags ssid */
        wifi_ap_t *ap = &aps[count];
        memset(ap, 0, sizeof(*ap));

        char *bssid = strtok_r(line, "\t ", &save_col);
        char *freq  = strtok_r(NULL, "\t ", &save_col);
        char *sig   = strtok_r(NULL, "\t ", &save_col);
        char *flags = strtok_r(NULL, "\t ", &save_col);

        if (bssid) snprintf(ap->bssid, sizeof(ap->bssid), "%s", bssid);
        if (freq)  ap->freq  = atoi(freq);
        if (sig)   ap->signal = atoi(sig);

        /* flags 和 ssid 之间可能有多个字段, ssid 在最后 */
        if (flags) {
            char *ssid = save_col;  /* 剩余全部 */
            if (ssid) {
                /* 去掉前后空白 */
                while (*ssid == ' ' || *ssid == '\t') ssid++;
                snprintf(ap->ssid, sizeof(ap->ssid), "%s", ssid);
            }
            snprintf(ap->flags, sizeof(ap->flags), "%s", flags);
        }

        count++;
        line = strtok_r(NULL, "\n", &save_line);
    }

    return count;
}

int wifi_connect(const char *ssid, const char *psk, int save) {
    char esc_ssid[WIFI_SSID_MAX * 2];
    char esc_psk[WIFI_PASS_MAX * 2];

    if (!ssid || !ssid[0]) return -1;
    wpa_escape(esc_ssid, sizeof(esc_ssid), ssid);
    wpa_escape(esc_psk, sizeof(esc_psk), psk ? psk : "");

    /* 构建新 wpa.conf 内容 */
    char conf[1024];
    int len = snprintf(conf, sizeof(conf),
        "ctrl_interface=/var/run/wpa_supplicant\n"
        "network={\n"
        "    scan_ssid=1\n"
        "    ssid=\"%s\"\n", esc_ssid);
    if (len < 0 || len >= (int)sizeof(conf)) return -1;

    if (psk && psk[0]) {
        int n = snprintf(conf + len, sizeof(conf) - len,
            "    psk=\"%s\"\n", esc_psk);
        if (n < 0 || n >= (int)sizeof(conf) - len) return -1;
        len += n;
    } else {
        int n = snprintf(conf + len, sizeof(conf) - len,
            "    key_mgmt=NONE\n");
        if (n < 0 || n >= (int)sizeof(conf) - len) return -1;
        len += n;
    }

    {
        int n = snprintf(conf + len, sizeof(conf) - len, "}\n");
        if (n < 0 || n >= (int)sizeof(conf) - len) return -1;
        len += n;
    }

    (void)save;  /* 屏幕连接需要可恢复配置；统一写入配置文件，避免 shell 转义风险。 */

    /* 持久化: 写 wpa.conf + 重载 */
    FILE *fp = fopen(WPA_CONF, "w");
    if (!fp) { perror("[wifi] 写入 " WPA_CONF); return -1; }
    fwrite(conf, 1, len, fp);
    fclose(fp);

    /* 重载配置 */
    run_cmd(WPA_CLI " reconfigure 2>/dev/null", NULL, 0);
    run_cmd(WPA_CLI " disconnect 2>/dev/null", NULL, 0);
    usleep(200000);
    run_cmd(WPA_CLI " reconnect 2>/dev/null", NULL, 0);
    run_cmd(WPA_CLI " reassociate 2>/dev/null", NULL, 0);
    printf("[wifi] 已连接并保存: %s\n", ssid);
    return 0;
}

int wifi_disconnect(void) {
    run_cmd(WPA_CLI " disconnect 2>/dev/null", NULL, 0);
    return 0;
}

int wifi_config_load(wifi_config_t *out) {
    if (!out) return -1;

    FILE *fp = fopen(WPA_CONF, "r");
    if (!fp) return -1;

    memset(out, 0, sizeof(*out));
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        /* 提取 ssid="..." */
        char *s = strstr(line, "ssid=\"");
        if (s) {
            s += 6;  /* strlen("ssid=\"") */
            int i = 0;
            while (*s && *s != '"' && i < WIFI_SSID_MAX - 1)
                out->ssid[i++] = *s++;
            out->ssid[i] = '\0';
        }
        /* 提取 psk="..." */
        s = strstr(line, "psk=\"");
        if (s) {
            s += 5;
            int i = 0;
            while (*s && *s != '"' && i < WIFI_PASS_MAX - 1)
                out->psk[i++] = *s++;
            out->psk[i] = '\0';
        }
    }
    fclose(fp);
    return (out->ssid[0] != '\0') ? 0 : -1;
}

int wifi_config_save(const wifi_config_t *cfg) {
    if (!cfg || !cfg->ssid[0]) return -1;
    return wifi_connect(cfg->ssid, cfg->psk, 1);
}
