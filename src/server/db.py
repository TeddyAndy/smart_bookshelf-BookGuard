"""数据库操作 — asyncpg 异步写入"""

import json
import logging
from datetime import datetime, timezone

import asyncpg

from config import DB_DSN

logger = logging.getLogger("bookguard.db")

# 连接池
pool: asyncpg.Pool | None = None


def _parse_ts(ts_val):
    """将时间戳（ISO 字符串 或 Unix 秒数/毫秒数）转为 datetime，失败用当前 UTC"""
    if not ts_val:
        return datetime.now(timezone.utc)
    try:
        # Unix 时间戳（秒或毫秒）
        if isinstance(ts_val, (int, float)):
            if ts_val > 1e12:  # 毫秒
                ts_val = ts_val / 1000
            return datetime.fromtimestamp(ts_val, tz=timezone.utc)
        # ISO 字符串
        s = str(ts_val).replace("Z", "+00:00")
        return datetime.fromisoformat(s)
    except (ValueError, TypeError):
        return datetime.now(timezone.utc)


async def init_db():
    """初始化数据库连接池"""
    global pool
    async def set_tz(conn):
        await conn.execute("SET timezone = 'Asia/Shanghai'")
    pool = await asyncpg.create_pool(
        DB_DSN,
        min_size=2,
        max_size=10,
        init=set_tz,
    )
    logger.info("数据库连接池已创建")


async def close_db():
    """关闭连接池"""
    global pool
    if pool:
        await pool.close()
        logger.info("数据库连接池已关闭")


async def insert_sensor(data: dict):
    """写入传感器读数 → sensor_log"""
    ts = _parse_ts(data.get("ts"))
    sql = """
        INSERT INTO sensor_log (recorded_at, temp_c, hum_pct, lux, dew_point_c, radar_state)
        VALUES ($1, $2, $3, $4, $5, $6)
    """
    async with pool.acquire() as conn:
        await conn.execute(
            sql,
            ts,
            data.get("temp_c", 0),
            data.get("hum_pct", 0),
            data.get("lux", 0),
            data.get("dew_point_c"),
            data.get("radar_state"),
        )
    logger.debug(f"传感器数据已写入: {data.get('temp_c')}°C {data.get('hum_pct')}% {data.get('lux')}lx")


async def update_snapshot(data: dict):
    """更新书架快照 → shelf_snapshot (UPSERT)

    时间戳保护: 只有新数据的时间戳比库里的更新 (或库里为空) 才覆盖。
    避免 MQTT retained 旧消息在服务重启时覆盖掉 FSM 刚发的新鲜数据。
    """
    ts = _parse_ts(data.get("ts"))
    sql = """
        INSERT INTO shelf_snapshot (id, updated_at, total_books, occupied_slots, total_slots, layers, books)
        VALUES (1, $1, $2, $3, $4, $5::jsonb, $6::jsonb)
        ON CONFLICT (id) DO UPDATE SET
            updated_at = EXCLUDED.updated_at,
            total_books = EXCLUDED.total_books,
            occupied_slots = EXCLUDED.occupied_slots,
            total_slots = EXCLUDED.total_slots,
            layers = EXCLUDED.layers,
            books = EXCLUDED.books
        WHERE shelf_snapshot.updated_at IS NULL
           OR shelf_snapshot.updated_at < EXCLUDED.updated_at
    """
    layers = json.dumps(data.get("layers", {}))
    books = json.dumps(data.get("books", []))
    async with pool.acquire() as conn:
        await conn.execute(
            sql,
            ts,
            data.get("total_books", 0),
            data.get("occupied_slots", 0),
            data.get("total_slots", 40),
            layers,
            books,
        )
    logger.debug(f"书架快照已更新: {data.get('occupied_slots')}/{data.get('total_slots')}")


async def insert_event(data: dict):
    """记录借阅事件 → shelf_events + 更新 book_stats"""
    ts = _parse_ts(data.get("ts"))
    event_type = data.get("event_type", "scan")
    epc = data.get("epc", "")
    title = data.get("title", "")

    # 1) 写入事件
    sql_event = """
        INSERT INTO shelf_events (occurred_at, event_type, epc, title, layer, pos_cm, rssi)
        VALUES ($1, $2, $3, $4, $5, $6, $7)
    """
    async with pool.acquire() as conn:
        await conn.execute(
            sql_event,
            ts, event_type, epc, title,
            data.get("layer"), data.get("pos_cm"), data.get("rssi"),
        )

        # 2) 更新书籍统计
        if epc and event_type in ("borrow", "return", "register", "misplaced"):
            sql_stats = """
                INSERT INTO book_stats (epc, title, borrow_count, last_borrow_at, popularity, updated_at)
                VALUES ($1, $2, 1, CASE WHEN $3 = 'borrow' THEN $4::timestamptz ELSE NULL END, 0.5, $4)
                ON CONFLICT (epc) DO UPDATE SET
                    title = EXCLUDED.title,
                    borrow_count = book_stats.borrow_count + CASE WHEN $3 = 'borrow' THEN 1 ELSE 0 END,
                    last_borrow_at = CASE WHEN $3 = 'borrow' THEN $4::timestamptz ELSE book_stats.last_borrow_at END,
                    last_return_at = CASE WHEN $3 = 'return' THEN $4::timestamptz ELSE book_stats.last_return_at END,
                    popularity = LEAST(1.0, book_stats.popularity + CASE WHEN $3 = 'borrow' THEN 0.05 ELSE 0.01 END),
                    updated_at = $4
            """
            await conn.execute(sql_stats, epc, title, event_type, ts)

    logger.info(f"事件已记录: {event_type} epc={epc} title={title}")


async def insert_cmd(data: dict, topic: str):
    """记录小程序命令 → shelf_events (event_type=find/scan)"""
    ts = _parse_ts(data.get("timestamp") or data.get("ts"))
    # bookshelf/cmd/find → find, bookshelf/cmd/scan → scan
    cmd_type = topic.rsplit("/", 1)[-1]
    query = data.get("query", "")
    force = data.get("force", False)

    sql = """
        INSERT INTO shelf_events (occurred_at, event_type, epc, title, detail)
        VALUES ($1, $2, $3, $4, $5)
    """
    # title 用搜索词，一目了然
    if query:
        title = f"搜索: {query}"
    elif cmd_type == "scan":
        title = "盘点扫描" + (" (强制)" if force else "")
    else:
        title = "用户操作"

    async with pool.acquire() as conn:
        await conn.execute(
            sql,
            ts, cmd_type,
            "",  # epc
            title,
            None,  # detail
        )
    logger.info(f"命令已记录: {cmd_type} query={query} force={force}")


# ============================================================
# 查询函数 (HTTP API 用)
# ============================================================

async def get_sensors_24h():
    """获取最近 24 小时的传感器数据（每小时一个采样点）"""
    sql = """
        WITH hourly AS (
            SELECT date_trunc('hour', recorded_at) AS hour,
                   AVG(temp_c) AS temp_c,
                   AVG(hum_pct) AS hum_pct,
                   AVG(lux) AS lux
            FROM sensor_log
            WHERE recorded_at > now() - INTERVAL '24 hours'
            GROUP BY 1
            ORDER BY 1
        )
        SELECT hour AS recorded_at, ROUND(temp_c::numeric, 1) AS temp_c,
               ROUND(hum_pct::numeric, 1) AS hum_pct,
               ROUND(lux::numeric, 0) AS lux
        FROM hourly
    """
    async with pool.acquire() as conn:
        rows = await conn.fetch(sql)
        return [dict(r) for r in rows]


async def get_sensors_recent(minutes: int = 120):
    """获取最近 N 分钟的传感器数据（每分钟一个采样点）"""
    sql = """
        WITH minutely AS (
            SELECT date_trunc('minute', recorded_at) AS minute,
                   AVG(temp_c) AS temp_c,
                   AVG(hum_pct) AS hum_pct,
                   AVG(lux) AS lux
            FROM sensor_log
            WHERE recorded_at > now() - ($1::int || ' minutes')::interval
            GROUP BY 1
            ORDER BY 1
        )
        SELECT minute AS recorded_at, ROUND(temp_c::numeric, 1) AS temp_c,
               ROUND(hum_pct::numeric, 1) AS hum_pct,
               ROUND(lux::numeric, 0) AS lux
        FROM minutely
    """
    async with pool.acquire() as conn:
        rows = await conn.fetch(sql, minutes)
        return [dict(r) for r in rows]


async def get_sensors_summary():
    """7 天环境摘要 + 异常检测"""
    sql = """
        SELECT
            ROUND(AVG(temp_c)::numeric, 1) AS avg_temp_c,
            ROUND(MIN(temp_c)::numeric, 1) AS min_temp_c,
            ROUND(MAX(temp_c)::numeric, 1) AS max_temp_c,
            ROUND(AVG(hum_pct)::numeric, 1) AS avg_hum_pct,
            ROUND(MIN(hum_pct)::numeric, 1) AS min_hum_pct,
            ROUND(MAX(hum_pct)::numeric, 1) AS max_hum_pct,
            ROUND(AVG(lux)::numeric, 0) AS avg_lux,
            ROUND(MAX(lux)::numeric, 0) AS max_lux,
            COUNT(*) AS readings,
            COUNT(CASE WHEN temp_c > 35 THEN 1 END) AS high_temp_count,
            COUNT(CASE WHEN hum_pct > 80 THEN 1 END) AS high_hum_count,
            COUNT(CASE WHEN lux > 50000 THEN 1 END) AS high_light_count
        FROM sensor_log
        WHERE recorded_at > now() - INTERVAL '7 days'
    """
    async with pool.acquire() as conn:
        row = await conn.fetchrow(sql)
        if not row or row["readings"] == 0:
            return {"message": "暂无7天数据", "days": 0}
        d = dict(row)
        # 生成建议
        suggestions = []
        if d["high_temp_count"] > 10:
            suggestions.append("⚠️ 温度频繁超标 (>35°C)，建议改善通风")
        if d["high_hum_count"] > 10:
            suggestions.append("⚠️ 湿度过高 (>80%)，建议除湿防霉")
        if d["high_light_count"] > 5:
            suggestions.append("⚠️ 光照过强，建议遮光保护书籍")
        if not suggestions and d["readings"] > 100:
            suggestions.append("✅ 环境良好，适合书籍保存")
        d["suggestions"] = suggestions
        d["days"] = 7
        return d


async def get_book_history(epc: str, limit: int = 50):
    """某本书的借阅历史"""
    sql = """
        SELECT occurred_at, event_type, title, layer, pos_cm, rssi
        FROM shelf_events
        WHERE epc = $1
        ORDER BY occurred_at DESC
        LIMIT $2
    """
    async with pool.acquire() as conn:
        rows = await conn.fetch(sql, epc, limit)
        return [dict(r) for r in rows]


async def get_events_recent(limit: int = 20):
    """最近借阅事件"""
    sql = """
        SELECT occurred_at, event_type, epc, title
        FROM shelf_events
        ORDER BY occurred_at DESC
        LIMIT $1
    """
    async with pool.acquire() as conn:
        rows = await conn.fetch(sql, limit)
        return [dict(r) for r in rows]


async def get_popular_books(limit: int = 10):
    """热门书籍排行"""
    sql = """
        SELECT epc, title, borrow_count, popularity,
               last_borrow_at, last_return_at
        FROM book_stats
        ORDER BY popularity DESC, borrow_count DESC
        LIMIT $1
    """
    async with pool.acquire() as conn:
        rows = await conn.fetch(sql, limit)
        return [dict(r) for r in rows]


async def get_daily_stats():
    """借阅统计摘要"""
    sql = """
        SELECT
            COUNT(*) FILTER (WHERE event_type = 'borrow') AS today_borrows,
            COUNT(*) FILTER (WHERE event_type = 'return') AS today_returns,
            COUNT(*) FILTER (WHERE event_type = 'register') AS today_registers
        FROM shelf_events
        WHERE occurred_at > CURRENT_DATE
    """
    async with pool.acquire() as conn:
        row = await conn.fetchrow(sql)
        return dict(row) if row else {}


async def search_books(query: str, limit: int = 20):
    """模糊搜索书名/EPC（查 book_stats 表）"""
    sql = """
        SELECT epc, title, borrow_count, popularity
        FROM book_stats
        WHERE title ILIKE '%' || $1 || '%'
           OR epc ILIKE '%' || $1 || '%'
        ORDER BY popularity DESC, borrow_count DESC
        LIMIT $2
    """
    async with pool.acquire() as conn:
        rows = await conn.fetch(sql, query, limit)
        return [dict(r) for r in rows]


async def get_related_books(epc: str, limit: int = 8):
    """关联推荐 — 借过此书的人也借过哪些书（同时段共现分析）

    算法：找到目标书的每次借阅时间，在 ±1h 窗口内查找
          同一时段被借出的其他书，按共现次数排序。
    """
    sql = """
        WITH target_times AS (
            SELECT occurred_at
            FROM shelf_events
            WHERE epc = $1 AND event_type = 'borrow'
        ),
        coborrows AS (
            SELECT e.epc, e.title,
                   COUNT(DISTINCT t.occurred_at) AS co_count
            FROM shelf_events e
            INNER JOIN target_times t
                ON e.occurred_at BETWEEN t.occurred_at - INTERVAL '1 hour'
                                     AND t.occurred_at + INTERVAL '1 hour'
            WHERE e.epc != $1
              AND e.event_type = 'borrow'
            GROUP BY e.epc, e.title
        )
        SELECT epc, title, co_count,
               ROUND(co_count::numeric / (SELECT COUNT(*) FROM target_times) * 100, 0)::int AS co_pct
        FROM coborrows
        ORDER BY co_count DESC
        LIMIT $2
    """
    async with pool.acquire() as conn:
        rows = await conn.fetch(sql, epc, limit)
        return [dict(r) for r in rows]


async def get_recommend_for_user(limit: int = 10):
    """综合推荐：热门 + 最新借阅的关联书，混合排序

    策略：
      1. 取最近借过的 3 本书的关联推荐
      2. 补入热门书中还没借过的
      3. 去重后返回 Top N
    """
    sql = """
        -- 最近借过的 EPC
        WITH recent_epcs AS (
            SELECT epc
            FROM shelf_events
            WHERE event_type = 'borrow'
            GROUP BY epc
            ORDER BY MAX(occurred_at) DESC
            LIMIT 3
        ),
        -- 每个最近 EPC 的关联书（聚合），权重高于热门
        related_from_recent AS (
            SELECT e2.epc, e2.title,
                   COUNT(*) * 20 AS score,
                   'related'::text AS reason
            FROM shelf_events e1
            JOIN shelf_events e2
                ON e2.occurred_at BETWEEN e1.occurred_at - INTERVAL '1 hour'
                                      AND e1.occurred_at + INTERVAL '1 hour'
                AND e2.epc != e1.epc
                AND e2.event_type = 'borrow'
            WHERE e1.epc IN (SELECT epc FROM recent_epcs)
              AND e1.event_type = 'borrow'
              AND e2.epc NOT IN (SELECT epc FROM recent_epcs)
            GROUP BY e2.epc, e2.title
        ),
        -- 热门书补位（排除已借过 + 关联已覆盖的）
        popular_new AS (
            SELECT bs.epc, bs.title,
                   bs.borrow_count AS score,
                   'popular'::text AS reason
            FROM book_stats bs
            WHERE bs.epc NOT IN (SELECT epc FROM recent_epcs)
              AND bs.borrow_count > 0
            ORDER BY bs.popularity DESC
            LIMIT 10
        ),
        combined AS (
            SELECT * FROM related_from_recent
            UNION ALL
            SELECT * FROM popular_new
        )
        SELECT epc, title, SUM(score) AS score,
               CASE WHEN SUM(score) >= 20 THEN 'related' ELSE 'popular' END AS reason
        FROM combined
        GROUP BY epc, title
        ORDER BY SUM(score) DESC
        LIMIT $1
    """
    async with pool.acquire() as conn:
        rows = await conn.fetch(sql, limit)
        return [dict(r) for r in rows]
