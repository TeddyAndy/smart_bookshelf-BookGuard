/**
 * 智能书架 — 本地数据库实现 (SQLite3)
 *
 * 依赖: sqlite3.c / sqlite3.h (amalgamation, 直接编译进程序)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "db.h"
#include "sqlite3.h"

/* ================================================================
 * 内部辅助
 * ================================================================ */

#define CLEAR(p) memset((p), 0, sizeof(*(p)))

static void db_err(const db_ctx_t *ctx, const char *func, const char *sql)
{
    if (ctx && ctx->verbose && ctx->db) {
        fprintf(stderr, "[db] %s: %s\n", func, sqlite3_errmsg(ctx->db));
        if (sql) fprintf(stderr, "[db] SQL: %s\n", sql);
    }
}

/* 提取查询结果中的填充到 book_info_t (不包含 epc 键) */
static int extract_book_info(sqlite3_stmt *stmt, book_info_t *b)
{
    if (!b) return 0;
    snprintf(b->epc,             sizeof(b->epc),
             "%s", (const char *)sqlite3_column_text(stmt, 0) ?: "");
    snprintf(b->title,           sizeof(b->title),
             "%s", (const char *)sqlite3_column_text(stmt, 1) ?: "");
    snprintf(b->author,          sizeof(b->author),
             "%s", (const char *)sqlite3_column_text(stmt, 2) ?: "");
    snprintf(b->cover_path,      sizeof(b->cover_path),
             "%s", (const char *)sqlite3_column_text(stmt, 3) ?: "");
    b->expected_layer = sqlite3_column_int(stmt, 4);
    b->expected_start = sqlite3_column_double(stmt, 5);
    b->expected_end   = sqlite3_column_double(stmt, 6);
    b->pages          = sqlite3_column_int(stmt, 7);
    snprintf(b->registered_at,   sizeof(b->registered_at),
             "%s", (const char *)sqlite3_column_text(stmt, 8) ?: "");
    return 1;
}

/* 提取到 shelf_item_t */
static int extract_shelf_item(sqlite3_stmt *stmt, shelf_item_t *s)
{
    if (!s) return 0;
    snprintf(s->epc,       sizeof(s->epc),
             "%s", (const char *)sqlite3_column_text(stmt, 0) ?: "");
    snprintf(s->title,     sizeof(s->title),
             "%s", (const char *)sqlite3_column_text(stmt, 1) ?: "");
    snprintf(s->author,    sizeof(s->author),
             "%s", (const char *)sqlite3_column_text(stmt, 2) ?: "");
    s->layer      = sqlite3_column_int(stmt, 3);
    s->start_cm   = sqlite3_column_double(stmt, 4);
    s->end_cm     = sqlite3_column_double(stmt, 5);
    s->rssi       = sqlite3_column_int(stmt, 6);
    s->is_present = sqlite3_column_int(stmt, 7);
    snprintf(s->last_seen, sizeof(s->last_seen),
             "%s", (const char *)sqlite3_column_text(stmt, 8) ?: "");
    return 1;
}

/* ================================================================
 * 生命周期
 * ================================================================ */

int db_init(db_ctx_t *ctx, const char *path)
{
    int rc;
    const char *sql;

    if (!ctx) return -1;
    CLEAR(ctx);

    if (!path) path = DB_DEFAULT_PATH;
    snprintf(ctx->path, sizeof(ctx->path), "%s", path);

    rc = sqlite3_open(path, &ctx->db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[db] 无法打开 %s: %s\n", path, sqlite3_errmsg(ctx->db));
        sqlite3_close(ctx->db);
        ctx->db = NULL;
        return -1;
    }

    /* WAL + FULL同步 — 断电安全 */
    sqlite3_exec(ctx->db, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
    sqlite3_exec(ctx->db, "PRAGMA synchronous=FULL;", NULL, NULL, NULL);
    sqlite3_exec(ctx->db, "PRAGMA busy_timeout=3000;", NULL, NULL, NULL);
    /* WAL 自动检查点, 避免文件无限增长 */
    sqlite3_exec(ctx->db, "PRAGMA wal_autocheckpoint=1000;", NULL, NULL, NULL);

    /* ======== 建表 ======== */

    sql =
        "CREATE TABLE IF NOT EXISTS books ("
        "  epc             TEXT PRIMARY KEY,"
        "  title           TEXT NOT NULL,"
        "  author          TEXT DEFAULT '',"
        "  cover_path      TEXT DEFAULT '',"
        "  expected_layer  INTEGER DEFAULT -1,"
        "  expected_start  REAL DEFAULT -1,"
        "  expected_end    REAL DEFAULT -1,"
        "  active          INTEGER DEFAULT 1,"
        "  pages           INTEGER DEFAULT 0,"
        "  registered_at   TEXT DEFAULT (datetime('now','localtime'))"
        ");";
    rc = sqlite3_exec(ctx->db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { db_err(ctx, "books 建表失败", sql); return -1; }

    /* 兼容旧表: 尝试添加 pages 列 */
    sql = "ALTER TABLE books ADD COLUMN pages INTEGER DEFAULT 0;";
    sqlite3_exec(ctx->db, sql, NULL, NULL, NULL); /* 忽略 "duplicate column" 错误 */
    sql = "ALTER TABLE books ADD COLUMN active INTEGER DEFAULT 1;";
    sqlite3_exec(ctx->db, sql, NULL, NULL, NULL); /* 忽略 "duplicate column" 错误 */

    sql =
        "CREATE TABLE IF NOT EXISTS shelf_state ("
        "  epc         TEXT PRIMARY KEY,"
        "  title       TEXT DEFAULT '',"
        "  author      TEXT DEFAULT '',"
        "  layer       INTEGER NOT NULL,"
        "  start_cm    REAL NOT NULL,"
        "  end_cm      REAL NOT NULL,"
        "  rssi        INTEGER DEFAULT 0,"
        "  is_present  INTEGER DEFAULT 1,"
        "  last_seen   TEXT DEFAULT (datetime('now','localtime'))"
        ");";
    rc = sqlite3_exec(ctx->db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { db_err(ctx, "shelf_state 建表失败", sql); return -1; }

    sql =
        "CREATE TABLE IF NOT EXISTS book_log ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp   TEXT DEFAULT (datetime('now','localtime')),"
        "  epc         TEXT NOT NULL,"
        "  title       TEXT DEFAULT '',"
        "  action      TEXT NOT NULL,"
        "  layer       INTEGER,"
        "  start_cm    REAL,"
        "  end_cm      REAL,"
        "  rssi        INTEGER,"
        "  detail      TEXT DEFAULT ''"
        ");";
    rc = sqlite3_exec(ctx->db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { db_err(ctx, "book_log 建表失败", sql); return -1; }

    sql =
        "CREATE TABLE IF NOT EXISTS sensor_log ("
        "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp   TEXT DEFAULT (datetime('now','localtime')),"
        "  temperature REAL,"
        "  humidity    REAL,"
        "  lux         REAL,"
        "  radar_state INTEGER DEFAULT 0"
        ");";
    rc = sqlite3_exec(ctx->db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { db_err(ctx, "sensor_log 建表失败", sql); return -1; }

    sql =
        "CREATE TABLE IF NOT EXISTS system_state ("
        "  key         TEXT PRIMARY KEY,"
        "  value       TEXT DEFAULT '',"
        "  updated_at  TEXT DEFAULT (datetime('now','localtime'))"
        ");";
    rc = sqlite3_exec(ctx->db, sql, NULL, NULL, NULL);
    if (rc != SQLITE_OK) { db_err(ctx, "system_state 建表失败", sql); return -1; }

    if (ctx->verbose)
        printf("[db] 已打开 %s (WAL 模式)\n", path);

    return 0;
}

void db_close(db_ctx_t *ctx)
{
    if (!ctx || !ctx->db) return;
    if (ctx->verbose)
        printf("[db] 关闭 %s\n", ctx->path);
    sqlite3_close(ctx->db);
    ctx->db = NULL;
}

/* ================================================================
 * 书籍目录操作
 * ================================================================ */

int db_book_upsert(db_ctx_t *ctx, const char *epc, const char *title,
                   const char *author, int layer, double start_cm, double end_cm)
{
    return db_book_upsert_pages(ctx, epc, title, author, layer, start_cm, end_cm, 0);
}

int db_book_upsert_pages(db_ctx_t *ctx, const char *epc, const char *title,
                         const char *author, int layer, double start_cm,
                         double end_cm, int pages)
{
    const char *sql;
    sqlite3_stmt *stmt;
    int rc;

    if (!ctx || !ctx->db || !epc || !title) return -1;

    sql = "INSERT OR REPLACE INTO books "
          "(epc, title, author, cover_path, expected_layer, expected_start, expected_end, active, pages) "
          "VALUES (?, ?, ?, '', ?, ?, ?, 1, ?);";

    rc = sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) { db_err(ctx, "db_book_upsert", sql); return -1; }

    sqlite3_bind_text(stmt,  1, epc,    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,  2, title,  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,  3, author ? author : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt,   4, layer);
    sqlite3_bind_double(stmt, 5, start_cm);
    sqlite3_bind_double(stmt, 6, end_cm);
    sqlite3_bind_int(stmt,   7, pages);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        db_err(ctx, "db_book_upsert:step", NULL);
        return -1;
    }
    return 0;
}

int db_book_find(db_ctx_t *ctx, const char *epc, book_info_t *out)
{
    const char *sql;
    sqlite3_stmt *stmt;
    int rc;

    if (!ctx || !ctx->db || !epc) return -1;
    if (out) CLEAR(out);

    sql = "SELECT epc, title, author, cover_path, expected_layer, "
          "expected_start, expected_end, pages, registered_at "
          "FROM books WHERE epc = ?;";

    rc = sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) { db_err(ctx, "db_book_find", sql); return -1; }

    sqlite3_bind_text(stmt, 1, epc, -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        extract_book_info(stmt, out);
        sqlite3_finalize(stmt);
        return 1;  /* 找到 */
    }

    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;  /* 未找到 vs 错误 */
}

int db_book_delete(db_ctx_t *ctx, const char *epc)
{
    const char *sql;
    sqlite3_stmt *stmt;
    int rc;

    if (!ctx || !ctx->db || !epc) return -1;

    sql = "DELETE FROM books WHERE epc = ?;";
    rc = sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) { db_err(ctx, "db_book_delete", sql); return -1; }

    sqlite3_bind_text(stmt, 1, epc, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_book_list(db_ctx_t *ctx, book_info_t *out, int max_count)
{
    const char *sql;
    sqlite3_stmt *stmt;
    int rc, count = 0;

    if (!ctx || !ctx->db) return -1;

    sql = "SELECT epc, title, author, cover_path, expected_layer, "
          "expected_start, expected_end, pages, registered_at "
          "FROM books WHERE COALESCE(active, 1) = 1 "
          "ORDER BY expected_layer, expected_start;";

    rc = sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) { db_err(ctx, "db_book_list", sql); return -1; }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (out && count < max_count) {
            extract_book_info(stmt, &out[count]);
        }
        count++;
    }

    sqlite3_finalize(stmt);
    return count;
}

int db_book_count(db_ctx_t *ctx)
{
    if (!ctx || !ctx->db) return -1;
    return db_book_list(ctx, NULL, 0);
}

/* ================================================================
 * 书架状态操作
 * ================================================================ */

int db_shelf_upsert(db_ctx_t *ctx, const char *epc, const char *title,
                    const char *author, int layer, double start_cm,
                    double end_cm, int rssi, int is_present)
{
    const char *sql;
    sqlite3_stmt *stmt;
    int rc;

    if (!ctx || !ctx->db || !epc) return -1;

    sql = "INSERT OR REPLACE INTO shelf_state "
          "(epc, title, author, layer, start_cm, end_cm, rssi, is_present, last_seen) "
          "VALUES (?, ?, ?, ?, ?, ?, ?, ?, datetime('now','localtime'));";

    rc = sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) { db_err(ctx, "db_shelf_upsert", sql); return -1; }

    sqlite3_bind_text(stmt,   1, epc,    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,   2, title ? title : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,   3, author ? author : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt,    4, layer);
    sqlite3_bind_double(stmt, 5, start_cm);
    sqlite3_bind_double(stmt, 6, end_cm);
    sqlite3_bind_int(stmt,    7, rssi);
    sqlite3_bind_int(stmt,    8, is_present);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_shelf_find(db_ctx_t *ctx, const char *epc, shelf_item_t *out)
{
    const char *sql;
    sqlite3_stmt *stmt;
    int rc;

    if (!ctx || !ctx->db || !epc) return -1;
    if (out) CLEAR(out);

    sql = "SELECT epc, title, author, layer, start_cm, end_cm, "
          "rssi, is_present, last_seen FROM shelf_state WHERE epc = ?;";

    rc = sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) { db_err(ctx, "db_shelf_find", sql); return -1; }

    sqlite3_bind_text(stmt, 1, epc, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);

    if (rc == SQLITE_ROW) {
        extract_shelf_item(stmt, out);
        sqlite3_finalize(stmt);
        return 1;
    }

    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_shelf_list_layer(db_ctx_t *ctx, int layer, shelf_item_t *out, int max_count)
{
    const char *sql;
    sqlite3_stmt *stmt;
    int rc, count = 0;

    if (!ctx || !ctx->db) return -1;

    sql = "SELECT epc, title, author, layer, start_cm, end_cm, "
          "rssi, is_present, last_seen FROM shelf_state "
          "WHERE layer = ? ORDER BY start_cm;";

    rc = sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) { db_err(ctx, "db_shelf_list_layer", sql); return -1; }

    sqlite3_bind_int(stmt, 1, layer);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (out && count < max_count) {
            extract_shelf_item(stmt, &out[count]);
        }
        count++;
    }

    sqlite3_finalize(stmt);
    return count;
}

int db_shelf_list_all(db_ctx_t *ctx, shelf_item_t *out, int max_count)
{
    const char *sql;
    sqlite3_stmt *stmt;
    int rc, count = 0;

    if (!ctx || !ctx->db) return -1;

    sql = "SELECT epc, title, author, layer, start_cm, end_cm, "
          "rssi, is_present, last_seen FROM shelf_state "
          "WHERE is_present = 1 ORDER BY layer, start_cm;";

    rc = sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) { db_err(ctx, "db_shelf_list_all", sql); return -1; }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (out && count < max_count) {
            extract_shelf_item(stmt, &out[count]);
        }
        count++;
    }

    sqlite3_finalize(stmt);
    return count;
}

int db_shelf_clear(db_ctx_t *ctx)
{
    const char *sql = "DELETE FROM shelf_state;";
    int rc;

    if (!ctx || !ctx->db) return -1;
    rc = sqlite3_exec(ctx->db, sql, NULL, NULL, NULL);
    return (rc == SQLITE_OK) ? 0 : -1;
}

int db_shelf_delete(db_ctx_t *ctx, const char *epc)
{
    const char *sql;
    sqlite3_stmt *stmt;
    int rc;

    if (!ctx || !ctx->db || !epc) return -1;

    sql = "DELETE FROM shelf_state WHERE epc = ?;";
    rc = sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) { db_err(ctx, "db_shelf_delete", sql); return -1; }

    sqlite3_bind_text(stmt, 1, epc, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_shelf_count(db_ctx_t *ctx)
{
    if (!ctx || !ctx->db) return -1;
    return db_shelf_list_all(ctx, NULL, 0);
}

int db_meta_set(db_ctx_t *ctx, const char *key, const char *value)
{
    const char *sql;
    sqlite3_stmt *stmt;
    int rc;

    if (!ctx || !ctx->db || !key) return -1;
    sql = "INSERT INTO system_state (key, value, updated_at) "
          "VALUES (?, ?, datetime('now','localtime')) "
          "ON CONFLICT(key) DO UPDATE SET value=excluded.value, updated_at=excluded.updated_at;";
    rc = sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) { db_err(ctx, "db_meta_set", sql); return -1; }
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value ? value : "", -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

int db_meta_get(db_ctx_t *ctx, const char *key, char *out, int out_sz)
{
    const char *sql;
    sqlite3_stmt *stmt;
    int rc;

    if (!ctx || !ctx->db || !key || !out || out_sz <= 0) return -1;
    out[0] = '\0';
    sql = "SELECT value FROM system_state WHERE key = ?;";
    rc = sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) { db_err(ctx, "db_meta_get", sql); return -1; }
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        snprintf(out, (size_t)out_sz, "%s",
                 (const char *)sqlite3_column_text(stmt, 0) ?: "");
        sqlite3_finalize(stmt);
        return 1;
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* ================================================================
 * 操作日志
 * ================================================================ */

int db_log_add(db_ctx_t *ctx, const char *epc, const char *title,
               const char *action, int layer, double start_cm,
               double end_cm, int rssi, const char *detail)
{
    const char *sql;
    sqlite3_stmt *stmt;
    int rc;

    if (!ctx || !ctx->db || !epc || !action) return -1;

    sql = "INSERT INTO book_log "
          "(epc, title, action, layer, start_cm, end_cm, rssi, detail) "
          "VALUES (?, ?, ?, ?, ?, ?, ?, ?);";

    rc = sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) { db_err(ctx, "db_log_add", sql); return -1; }

    sqlite3_bind_text(stmt,   1, epc,    -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,   2, title ? title : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,   3, action, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt,    4, layer);
    sqlite3_bind_double(stmt, 5, start_cm);
    sqlite3_bind_double(stmt, 6, end_cm);
    sqlite3_bind_int(stmt,    7, rssi);
    sqlite3_bind_text(stmt,   8, detail ? detail : "", -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_log_recent(db_ctx_t *ctx, int limit, book_log_t *out, int max_count)
{
    const char *sql;
    sqlite3_stmt *stmt;
    int rc, count = 0;

    if (!ctx || !ctx->db) return -1;
    if (limit <= 0) limit = 20;

    sql = "SELECT id, timestamp, epc, title, action, layer, start_cm, "
          "end_cm, rssi, detail FROM book_log "
          "ORDER BY id DESC LIMIT ?;";

    rc = sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) { db_err(ctx, "db_log_recent", sql); return -1; }

    sqlite3_bind_int(stmt, 1, limit);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && count < max_count) {
        if (out) {
            book_log_t *log = &out[count];
            CLEAR(log);
            log->id = sqlite3_column_int(stmt, 0);
            snprintf(log->timestamp, sizeof(log->timestamp),
                     "%s", (const char *)sqlite3_column_text(stmt, 1) ?: "");
            snprintf(log->epc, sizeof(log->epc),
                     "%s", (const char *)sqlite3_column_text(stmt, 2) ?: "");
            snprintf(log->title, sizeof(log->title),
                     "%s", (const char *)sqlite3_column_text(stmt, 3) ?: "");
            snprintf(log->action, sizeof(log->action),
                     "%s", (const char *)sqlite3_column_text(stmt, 4) ?: "");
            log->layer    = sqlite3_column_int(stmt, 5);
            log->start_cm = sqlite3_column_double(stmt, 6);
            log->end_cm   = sqlite3_column_double(stmt, 7);
            log->rssi     = sqlite3_column_int(stmt, 8);
            snprintf(log->detail, sizeof(log->detail),
                     "%s", (const char *)sqlite3_column_text(stmt, 9) ?: "");
        }
        count++;
    }

    sqlite3_finalize(stmt);
    return count;
}

/* ================================================================
 * 传感器日志
 * ================================================================ */

int db_sensor_add(db_ctx_t *ctx, double temp, double hum,
                  double lux, int radar_state)
{
    const char *sql;
    sqlite3_stmt *stmt;
    int rc;

    if (!ctx || !ctx->db) return -1;

    sql = "INSERT INTO sensor_log "
          "(temperature, humidity, lux, radar_state) VALUES (?, ?, ?, ?);";

    rc = sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) { db_err(ctx, "db_sensor_add", sql); return -1; }

    sqlite3_bind_double(stmt, 1, temp);
    sqlite3_bind_double(stmt, 2, hum);
    sqlite3_bind_double(stmt, 3, lux);
    sqlite3_bind_int(stmt,    4, radar_state);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    return (rc == SQLITE_DONE) ? 0 : -1;
}

int db_sensor_recent(db_ctx_t *ctx, int limit, sensor_log_t *out, int max_count)
{
    const char *sql;
    sqlite3_stmt *stmt;
    int rc, count = 0;

    if (!ctx || !ctx->db) return -1;
    if (limit <= 0) limit = 20;

    sql = "SELECT id, timestamp, temperature, humidity, lux, radar_state "
          "FROM sensor_log ORDER BY id DESC LIMIT ?;";

    rc = sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) { db_err(ctx, "db_sensor_recent", sql); return -1; }

    sqlite3_bind_int(stmt, 1, limit);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && count < max_count) {
        if (out) {
            sensor_log_t *s = &out[count];
            CLEAR(s);
            s->id = sqlite3_column_int(stmt, 0);
            snprintf(s->timestamp, sizeof(s->timestamp),
                     "%s", (const char *)sqlite3_column_text(stmt, 1) ?: "");
            s->temperature = sqlite3_column_double(stmt, 2);
            s->humidity    = sqlite3_column_double(stmt, 3);
            s->lux         = sqlite3_column_double(stmt, 4);
            s->radar_state = sqlite3_column_int(stmt, 5);
        }
        count++;
    }

    sqlite3_finalize(stmt);
    return count;
}

int db_sensor_range(db_ctx_t *ctx, int hours, sensor_log_t *out, int max_count)
{
    const char *sql;
    sqlite3_stmt *stmt;
    int rc, count = 0;
    int bucket_sec;
    char modifier[32];

    if (!ctx || !ctx->db || !out || max_count <= 0) return -1;
    if (hours <= 1) {
        hours = 1;
        bucket_sec = 30;
    } else if (hours <= 24) {
        hours = 24;
        bucket_sec = 600;
    } else {
        hours = 168;
        bucket_sec = 3600;
    }
    snprintf(modifier, sizeof(modifier), "-%d hours", hours);

    sql =
        "SELECT max(id), max(timestamp), avg(temperature), avg(humidity), avg(lux), "
        "       max(radar_state), CAST(strftime('%s', timestamp) / ? AS INTEGER) AS bucket "
        "FROM sensor_log "
        "WHERE timestamp >= datetime('now', ?, 'localtime') "
        "GROUP BY bucket "
        "ORDER BY bucket DESC "
        "LIMIT ?;";

    rc = sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) { db_err(ctx, "db_sensor_range", sql); return -1; }

    sqlite3_bind_int(stmt, 1, bucket_sec);
    sqlite3_bind_text(stmt, 2, modifier, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, max_count);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && count < max_count) {
        sensor_log_t *s = &out[count];
        CLEAR(s);
        s->id = sqlite3_column_int(stmt, 0);
        snprintf(s->timestamp, sizeof(s->timestamp),
                 "%s", (const char *)sqlite3_column_text(stmt, 1) ?: "");
        s->temperature = sqlite3_column_double(stmt, 2);
        s->humidity    = sqlite3_column_double(stmt, 3);
        s->lux         = sqlite3_column_double(stmt, 4);
        s->radar_state = sqlite3_column_int(stmt, 5);
        count++;
    }

    sqlite3_finalize(stmt);
    return count;
}

/* ================================================================
 * 调试
 * ================================================================ */

void db_set_verbose(db_ctx_t *ctx, int verbose)
{
    if (ctx) ctx->verbose = verbose;
}

/* ================================================================
 * 归档 + 清理 (NAND↔SD 双库)
 * ================================================================ */

int db_archive_open(db_ctx_t *ctx, const char *path)
{
    if (!ctx || !path) return -1;

    /* 检查 SD 卡是否挂载: 父目录必须存在 */
    const char *slash = strrchr(path, '/');
    if (slash) {
        char dir[256];
        size_t dlen = slash - path;
        if (dlen >= sizeof(dir)) dlen = sizeof(dir) - 1;
        memcpy(dir, path, dlen);
        dir[dlen] = '\0';
        struct stat st;
        if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
            fprintf(stderr, "[db] SD 卡未挂载 (%s 不存在), 归档跳过\n", dir);
            return -1;  /* 非致命 */
        }
    }

    return db_init(ctx, path);
}

/* 判断系统时钟是否有效 (可信) */
static int time_is_valid(void) {
    time_t now = time(NULL);
    /* 2024-01-01 00:00:00 UTC = 1704067200 */
    return (now > 1704067200);
}

/* 行数兜底清理 (系统时间无效时使用) */
#define DB_BOOK_LOG_MAX_ROWS   1000
#define DB_SENSOR_LOG_MAX_ROWS 5000

static int cleanup_by_count(sqlite3 *db, const char *table,
                             const char *pk, int max_rows) {
    char sql[256];
    snprintf(sql, sizeof(sql),
        "DELETE FROM %s WHERE %s NOT IN "
        "(SELECT %s FROM %s ORDER BY %s DESC LIMIT %d);",
        table, pk, pk, table, pk, max_rows);
    return sqlite3_exec(db, sql, NULL, NULL, NULL);
}

int db_archive_migrate(db_ctx_t *local, db_ctx_t *archive)
{
    if (!local || !local->db) return -1;
    int total = 0;
    int clock_ok = time_is_valid();

    /* 确定保留天数: 有归档用正常值, 无归档用更长天数兜底 */
    int book_days = DB_FALLBACK_BOOK_DAYS;
    int sens_days = DB_FALLBACK_SENSOR_DAYS;

    if (clock_ok && archive && archive->db) {
        /* 用 local 连接 ATTACH 归档库, 事务保证原子性 */
        if (sqlite3_exec(local->db, "BEGIN;", NULL, NULL, NULL) != SQLITE_OK) {
            fprintf(stderr, "[db] BEGIN 失败: %s\n", sqlite3_errmsg(local->db));
            goto cleanup_local;
        }

        char sql[1024];
        /* archive->path 是 FSM_DB_ARCHIVE 常量, 不含单引号, 安全 */
        snprintf(sql, sizeof(sql),
            "ATTACH DATABASE '%s' AS arc;", archive->path);
        if (sqlite3_exec(local->db, sql, NULL, NULL, NULL) != SQLITE_OK) {
            fprintf(stderr, "[db] ATTACH 归档库失败: %s\n", sqlite3_errmsg(local->db));
            sqlite3_exec(local->db, "ROLLBACK;", NULL, NULL, NULL);
            goto cleanup_local;
        }

        /* 迁移 book_log */
        snprintf(sql, sizeof(sql),
            "INSERT OR IGNORE INTO arc.book_log "
            "SELECT * FROM book_log "
            "WHERE timestamp < datetime('now','-%d days','localtime');",
            DB_BOOK_LOG_LOCAL_DAYS);
        if (sqlite3_exec(local->db, sql, NULL, NULL, NULL) != SQLITE_OK) {
            fprintf(stderr, "[db] 归档 book_log 失败: %s\n", sqlite3_errmsg(local->db));
            sqlite3_exec(local->db, "ROLLBACK;", NULL, NULL, NULL);
            sqlite3_exec(local->db, "DETACH DATABASE arc;", NULL, NULL, NULL);
            goto cleanup_local;
        }
        total += sqlite3_changes(local->db);

        /* 迁移 sensor_log */
        snprintf(sql, sizeof(sql),
            "INSERT OR IGNORE INTO arc.sensor_log "
            "SELECT * FROM sensor_log "
            "WHERE timestamp < datetime('now','-%d days','localtime');",
            DB_SENSOR_LOCAL_DAYS);
        if (sqlite3_exec(local->db, sql, NULL, NULL, NULL) != SQLITE_OK) {
            fprintf(stderr, "[db] 归档 sensor_log 失败: %s\n", sqlite3_errmsg(local->db));
            sqlite3_exec(local->db, "ROLLBACK;", NULL, NULL, NULL);
            sqlite3_exec(local->db, "DETACH DATABASE arc;", NULL, NULL, NULL);
            goto cleanup_local;
        }
        total += sqlite3_changes(local->db);

        if (sqlite3_exec(local->db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
            fprintf(stderr, "[db] 归档 COMMIT 失败: %s\n", sqlite3_errmsg(local->db));
            sqlite3_exec(local->db, "ROLLBACK;", NULL, NULL, NULL);
            sqlite3_exec(local->db, "DETACH DATABASE arc;", NULL, NULL, NULL);
            goto cleanup_local;
        }
        /* DETACH 必须在事务外执行 */
        sqlite3_exec(local->db, "DETACH DATABASE arc;", NULL, NULL, NULL);
        book_days = DB_BOOK_LOG_LOCAL_DAYS;
        sens_days = DB_SENSOR_LOCAL_DAYS;
    }

cleanup_local:;
    if (clock_ok) {
        /* 日历天数清理 */
        char csql[512];
        snprintf(csql, sizeof(csql),
            "DELETE FROM book_log WHERE timestamp < datetime('now','-%d days','localtime');",
            book_days);
        sqlite3_exec(local->db, csql, NULL, NULL, NULL);
        total += sqlite3_changes(local->db);

        snprintf(csql, sizeof(csql),
            "DELETE FROM sensor_log WHERE timestamp < datetime('now','-%d days','localtime');",
            sens_days);
        sqlite3_exec(local->db, csql, NULL, NULL, NULL);
        total += sqlite3_changes(local->db);
    } else {
        /* 行数兜底清理 (时钟不可信) */
        fprintf(stderr, "[db] ⚠️ 系统时钟无效, 改用行数兜底清理\n");
        cleanup_by_count(local->db, "book_log", "id", DB_BOOK_LOG_MAX_ROWS);
        cleanup_by_count(local->db, "sensor_log", "id", DB_SENSOR_LOG_MAX_ROWS);
        total = -1;  /* 标记: 行数兜底不计数 */
    }

    if (total > 0 && local->verbose)
        printf("[db] 归档: 迁移 %d 条, 本地已清理 (book<%dd sens<%dd)\n",
               total, book_days, sens_days);

    return total;
}

int db_cleanup(db_ctx_t *local, db_ctx_t *archive)
{
    if (!local || !local->db) return -1;
    int clock_ok = time_is_valid();

    /* 本地: 迁移 (删除旧记录) */
    db_archive_migrate(local, archive);

    /* 归档: 清理超出保留期的数据 */
    if (archive && archive->db) {
        if (clock_ok) {
            char sql[256];
            snprintf(sql, sizeof(sql),
                "DELETE FROM book_log WHERE timestamp < datetime('now','-%d days','localtime');",
                DB_BOOK_LOG_ARCHIVE_DAYS);
            sqlite3_exec(archive->db, sql, NULL, NULL, NULL);

            snprintf(sql, sizeof(sql),
                "DELETE FROM sensor_log WHERE timestamp < datetime('now','-%d days','localtime');",
                DB_SENSOR_ARCHIVE_DAYS);
            sqlite3_exec(archive->db, sql, NULL, NULL, NULL);
        } else {
            cleanup_by_count(archive->db, "book_log", "id", DB_BOOK_LOG_MAX_ROWS);
            cleanup_by_count(archive->db, "sensor_log", "id", DB_SENSOR_LOG_MAX_ROWS);
        }
    }

    return 0;
}

void db_check_size(const char *local_path, const char *archive_path)
{
    struct stat st;

    if (local_path && stat(local_path, &st) == 0) {
        long mb = st.st_size / (1024 * 1024);
        if (mb >= DB_WARN_SIZE_MB)
            fprintf(stderr, "[db] ⚠️ 本地库 %ldMB (阈值 %dMB), 建议清理\n",
                    mb, DB_WARN_SIZE_MB);
    }

    if (archive_path && stat(archive_path, &st) == 0) {
        long mb = st.st_size / (1024 * 1024);
        if (mb >= DB_WARN_SIZE_MB)
            fprintf(stderr, "[db] ⚠️ 归档库 %ldMB (阈值 %dMB)\n",
                    mb, DB_WARN_SIZE_MB);
    }
}
