/**
 * 智能书架 — 本地数据库 API
 *
 * 基于 SQLite3，数据库文件存 SD 卡 /mnt/sdcard/bookshelf/bookshelf.db
 *
 * 使用: db_init(&ctx, path) → 操作 → db_close(&ctx)
 */

#ifndef DB_H
#define DB_H

#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 数据库路径默认值 (NAND 本地库, 板端 /root/bookshelf.db) */
#define DB_DEFAULT_PATH  "/root/bookshelf.db"

/* ================================================================
 * 上下文
 * ================================================================ */

typedef struct sqlite3 sqlite3;

typedef struct {
    sqlite3  *db;
    char      path[256];
    int       verbose;
} db_ctx_t;

/* ================================================================
 * 数据结构
 * ================================================================ */

/* 书籍目录条目 (books 表) */
typedef struct {
    char    epc[64];            /* EPC hex 字符串 */
    char    title[128];         /* 书名 */
    char    author[64];         /* 作者 */
    char    cover_path[256];    /* 封面图片路径 */
    int     expected_layer;     /* 预期层 (-1=未分配, 0=上, 1=下) */
    double  expected_start;     /* 预期起始 cm */
    double  expected_end;       /* 预期结束 cm */
    int     pages;              /* 书页数 (用于估算厚度) */
    char    registered_at[32];  /* 注册时间 */
} book_info_t;

/* 书架实时状态 (shelf_state 表) */
typedef struct {
    char    epc[64];            /* EPC */
    char    title[128];         /* 书名 (冗余) */
    char    author[64];         /* 作者 (冗余) */
    int     layer;              /* 所在层 (0=上, 1=下) */
    double  start_cm;           /* 起始位置 cm */
    double  end_cm;             /* 结束位置 cm */
    int     rssi;               /* 信号强度 */
    int     is_present;         /* 是否在架 */
    char    last_seen[32];      /* 最后盘点时间 */
} shelf_item_t;

/* 操作日志条目 (book_log 表) */
typedef struct {
    int     id;
    char    timestamp[32];
    char    epc[64];
    char    title[128];
    char    action[32];         /* borrowed/returned/misplaced/registered/found */
    int     layer;
    double  start_cm;
    double  end_cm;
    int     rssi;
    char    detail[256];
} book_log_t;

/* 传感器记录 (sensor_log 表) */
typedef struct {
    int     id;
    char    timestamp[32];
    double  temperature;        /* °C */
    double  humidity;           /* %RH */
    double  lux;                /* lx */
    int     radar_state;        /* 0=无人 1=运动 2=静止 3=运动+静止 */
} sensor_log_t;

/* ================================================================
 * 生命周期
 * ================================================================ */

/**
 * 打开数据库并自动建表 (CREATE TABLE IF NOT EXISTS)
 * @param ctx   调用方分配的上下文 (栈上或全局均可)
 * @param path  数据库文件路径 (传 NULL 则用 DB_DEFAULT_PATH)
 * @return 0=成功, -1=失败
 */
int  db_init(db_ctx_t *ctx, const char *path);

/**
 * 安全关闭数据库
 * @param ctx  已初始化的上下文 (可以传 NULL，安全无操作)
 */
void db_close(db_ctx_t *ctx);

/* ================================================================
 * 书籍目录操作 (books 表)
 * ================================================================ */

/**
 * 添加/更新书籍 (REPLACE)
 * @return 0=成功, -1=失败
 */
int  db_book_upsert(db_ctx_t *ctx, const char *epc, const char *title,
                    const char *author, int layer, double start_cm, double end_cm);
int  db_book_upsert_pages(db_ctx_t *ctx, const char *epc, const char *title,
                          const char *author, int layer, double start_cm,
                          double end_cm, int pages);

/** 按 EPC 查找书籍，返回 1=找到 0=未找到 -1=错误 */
int  db_book_find(db_ctx_t *ctx, const char *epc, book_info_t *out);

/** 删除书籍 */
int  db_book_delete(db_ctx_t *ctx, const char *epc);

/** 列出全部书籍，返回数量，-1=错误。out 可为 NULL 只计数。 */
int  db_book_list(db_ctx_t *ctx, book_info_t *out, int max_count);

/** 总数 */
int  db_book_count(db_ctx_t *ctx);

/* ================================================================
 * 书架状态操作 (shelf_state 表)
 * ================================================================ */

/** 更新/插入一条书架状态 */
int  db_shelf_upsert(db_ctx_t *ctx, const char *epc, const char *title,
                     const char *author, int layer, double start_cm,
                     double end_cm, int rssi, int is_present);

/** 获取单条书架状态 */
int  db_shelf_find(db_ctx_t *ctx, const char *epc, shelf_item_t *out);

/** 获取某层的全部状态，返回数量 */
int  db_shelf_list_layer(db_ctx_t *ctx, int layer, shelf_item_t *out, int max_count);

/** 列出全部在架书籍 */
int  db_shelf_list_all(db_ctx_t *ctx, shelf_item_t *out, int max_count);

/** 清空书架状态 (盘点前调用) */
int  db_shelf_clear(db_ctx_t *ctx);

/** 删除单本书的书架状态 */
int  db_shelf_delete(db_ctx_t *ctx, const char *epc);

/** 在架数量 */
int  db_shelf_count(db_ctx_t *ctx);

/* ================================================================
 * 系统元信息 (system_state 表)
 * ================================================================ */

int  db_meta_set(db_ctx_t *ctx, const char *key, const char *value);
int  db_meta_get(db_ctx_t *ctx, const char *key, char *out, int out_sz);

/* ================================================================
 * 操作日志 (book_log 表)
 * ================================================================ */

/** 追加一条操作日志 */
int  db_log_add(db_ctx_t *ctx, const char *epc, const char *title,
                const char *action, int layer, double start_cm,
                double end_cm, int rssi, const char *detail);

/** 查询最近 N 条日志，返回数量 */
int  db_log_recent(db_ctx_t *ctx, int limit, book_log_t *out, int max_count);

/* ================================================================
 * 传感器日志 (sensor_log 表)
 * ================================================================ */

/** 追加一条传感器记录 */
int  db_sensor_add(db_ctx_t *ctx, double temp, double hum,
                   double lux, int radar_state);

/** 查询最近 N 条传感器记录 */
int  db_sensor_recent(db_ctx_t *ctx, int limit, sensor_log_t *out, int max_count);

/** 查询最近 hours 小时传感器趋势，按时间桶聚合后返回 newest-first */
int  db_sensor_range(db_ctx_t *ctx, int hours, sensor_log_t *out, int max_count);

/* ================================================================
 * 调试
 * ================================================================ */

/** 设置详细模式 (1=打印 SQL 错误到 stderr) */
void db_set_verbose(db_ctx_t *ctx, int verbose);

/* ================================================================
 * 归档 + 清理 (NAND↔SD 双库)
 * ================================================================ */

/* 保留策略 */
#define DB_BOOK_LOG_LOCAL_DAYS   3     /* NAND 保留 book_log 天数 */
#define DB_SENSOR_LOCAL_DAYS     1     /* NAND 保留 sensor 天数 */
#define DB_BOOK_LOG_ARCHIVE_DAYS 90    /* SD 保留 book_log 天数 */
#define DB_SENSOR_ARCHIVE_DAYS   90    /* SD 保留 sensor 天数 */
#define DB_FALLBACK_BOOK_DAYS    7     /* SD 不在时 NAND 多保留几天 */
#define DB_FALLBACK_SENSOR_DAYS  3
#define DB_WARN_SIZE_MB          10    /* DB 文件超过此值告警 */

/**
 * 打开归档数据库 (SD 卡)
 * @param ctx   已分配
 * @param path  SD 卡路径 (/mnt/sdcard/bookshelf/history.db)
 * @return 0=成功, -1=SD卡不存在/无法打开 (非致命)
 */
int  db_archive_open(db_ctx_t *ctx, const char *path);

/**
 * 从本地库迁移旧数据到归档库
 * 规则: book_log > 3天 → 插入 SD → 删本地
 *       sensor_log > 1天 → 插入 SD → 删本地
 * @param local   本地 NAND 库
 * @param archive 归档 SD 库 (可为 NULL, 跳过)
 * @return 迁移条数, -1=出错
 */
int  db_archive_migrate(db_ctx_t *local, db_ctx_t *archive);

/**
 * 清理过期数据 (双库都清)
 * @param local   本地 NAND 库
 * @param archive 归档 SD 库 (可为 NULL)
 * @return 0=成功
 */
int  db_cleanup(db_ctx_t *local, db_ctx_t *archive);

/**
 * 检查数据库文件大小, 超过阈值告警
 * @param local_path   本地库路径
 * @param archive_path 归档库路径 (可为 NULL)
 */
void db_check_size(const char *local_path, const char *archive_path);

#ifdef __cplusplus
}
#endif

#endif /* DB_H */
