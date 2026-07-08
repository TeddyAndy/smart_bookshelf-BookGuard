/**
 * 轻量 JSON 构造器 — 专为 MQTT 消息序列化设计
 *
 * 用法:
 *   char buf[4096];
 *   json_builder_t jb;
 *   json_init(&jb, buf, sizeof(buf));
 *   json_obj_open(&jb, NULL);
 *     json_str(&jb, "status", "online");
 *     json_int(&jb, "uptime", 3600);
 *     json_obj_open(&jb, "radar");
 *       json_str(&jb, "state", "静止");
 *       json_int(&jb, "move_dist_cm", 0);
 *     json_obj_close(&jb);
 *   json_obj_close(&jb);
 *   // buf = {"status":"online","uptime":3600,"radar":{"state":"静止","move_dist_cm":0}}
 */

#ifndef JSON_BUILDER_H
#define JSON_BUILDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JSON_MAX_NEST 8

typedef struct {
    char  *buf;
    int    size;
    int    pos;
    int    need_comma;
    int    nest_stack[JSON_MAX_NEST];  /* 每层是否有内容 */
    int    nest_level;
} json_builder_t;

/**
 * 初始化构造器
 * @param jb  构造器上下文
 * @param buf 输出缓冲区
 * @param size 缓冲区大小
 */
void json_init(json_builder_t *jb, char *buf, int size);

/**
 * 开始对象 { }
 * @param key 字段名 (最外层可为 NULL)
 */
void json_obj_open(json_builder_t *jb, const char *key);

/** 结束对象 */
void json_obj_close(json_builder_t *jb);

/** 开始数组 [ ] */
void json_arr_open(json_builder_t *jb, const char *key);

/** 结束数组 */
void json_arr_close(json_builder_t *jb);

/** 字符串字段 (自动转义) */
void json_str(json_builder_t *jb, const char *key, const char *value);

/** 整数字段 */
void json_int(json_builder_t *jb, const char *key, int64_t value);

/** 浮点字段 (保留 1 位小数) */
void json_float(json_builder_t *jb, const char *key, double value);

/** 布尔字段 */
void json_bool(json_builder_t *jb, const char *key, int value);

/** 裸 JSON (预先构造好的 JSON 片段，不转义) */
void json_raw(json_builder_t *jb, const char *key, const char *raw_json);

/** 返回当前构建的字符串长度 */
int json_len(json_builder_t *jb);

#ifdef __cplusplus
}
#endif

#endif /* JSON_BUILDER_H */
