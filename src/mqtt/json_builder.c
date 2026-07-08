/**
 * JSON 构造器实现
 */

#include "json_builder.h"
#include <string.h>
#include <stdio.h>

/* ── 内部: 追加字符 ─────────────────────────────── */

static inline void append_char(json_builder_t *jb, char c) {
    if (jb->pos + 1 < jb->size) {
        jb->buf[jb->pos++] = c;
        jb->buf[jb->pos] = '\0';
    }
}

/* ── 内部: 追加字符串 ────────────────────────────── */

static void append_str(json_builder_t *jb, const char *s) {
    if (!s) return;
    int len = (int)strlen(s);
    if (jb->pos + len >= jb->size) len = jb->size - jb->pos - 1;
    if (len > 0) {
        memcpy(jb->buf + jb->pos, s, len);
        jb->pos += len;
        jb->buf[jb->pos] = '\0';
    }
}

/* ── 内部: 逗号分隔 ──────────────────────────────── */

static void add_comma(json_builder_t *jb) {
    if (jb->nest_level > 0 && jb->nest_stack[jb->nest_level - 1]) {
        append_char(jb, ',');
    }
    if (jb->nest_level > 0) {
        jb->nest_stack[jb->nest_level - 1] = 1;
    }
}

/* ── 内部: 写 key ────────────────────────────────── */

static void add_key(json_builder_t *jb, const char *key) {
    if (key) {
        add_comma(jb);
        append_char(jb, '"');
        append_str(jb, key);
        append_str(jb, "\":");
    }
}

static void begin_value(json_builder_t *jb, const char *key) {
    if (key) {
        add_key(jb, key);
    } else {
        add_comma(jb);
    }
}

/* ── 公开 API ────────────────────────────────────── */

void json_init(json_builder_t *jb, char *buf, int size) {
    memset(jb, 0, sizeof(*jb));
    jb->buf = buf;
    jb->size = size;
    jb->buf[0] = '\0';
}

void json_obj_open(json_builder_t *jb, const char *key) {
    begin_value(jb, key);
    append_char(jb, '{');
    if (jb->nest_level < JSON_MAX_NEST) {
        jb->nest_stack[jb->nest_level] = 0;
        jb->nest_level++;
    }
}

void json_obj_close(json_builder_t *jb) {
    append_char(jb, '}');
    if (jb->nest_level > 0) jb->nest_level--;
}

void json_arr_open(json_builder_t *jb, const char *key) {
    begin_value(jb, key);
    append_char(jb, '[');
    if (jb->nest_level < JSON_MAX_NEST) {
        jb->nest_stack[jb->nest_level] = 0;
        jb->nest_level++;
    }
}

void json_arr_close(json_builder_t *jb) {
    append_char(jb, ']');
    if (jb->nest_level > 0) jb->nest_level--;
}

void json_str(json_builder_t *jb, const char *key, const char *value) {
    add_key(jb, key);
    append_char(jb, '"');
    if (value) {
        /* 简易转义: 处理 " \ \n \r \t */
        for (const char *p = value; *p; p++) {
            char esc = 0;
            switch (*p) {
            case '"':  esc = '"';  break;
            case '\\': esc = '\\'; break;
            case '\n': esc = 'n';  break;
            case '\r': esc = 'r';  break;
            case '\t': esc = 't';  break;
            }
            if (esc) {
                append_char(jb, '\\');
                append_char(jb, esc);
            } else {
                append_char(jb, *p);
            }
        }
    }
    append_char(jb, '"');
}

void json_int(json_builder_t *jb, const char *key, int64_t value) {
    add_key(jb, key);
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%lld", (long long)value);
    append_str(jb, tmp);
}

void json_float(json_builder_t *jb, const char *key, double value) {
    add_key(jb, key);
    char tmp[32];
    snprintf(tmp, sizeof(tmp), "%.1f", value);
    append_str(jb, tmp);
}

void json_bool(json_builder_t *jb, const char *key, int value) {
    add_key(jb, key);
    append_str(jb, value ? "true" : "false");
}

void json_raw(json_builder_t *jb, const char *key, const char *raw_json) {
    add_key(jb, key);
    append_str(jb, raw_json);
}

int json_len(json_builder_t *jb) {
    return jb->pos;
}
