"""BookGuard cloud backend — FastAPI + MQTT subscriber + PostgreSQL."""

import asyncio
import logging
from contextlib import asynccontextmanager

from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware

import db
import mqtt_client
from config import API_HOST, API_PORT

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(name)s] %(levelname)s: %(message)s",
)
logger = logging.getLogger("bookguard")


# ============================================================
# 生命周期
# ============================================================

@asynccontextmanager
async def lifespan(app: FastAPI):
    logger.info("BookGuard 云端后端 v1 启动中...")
    await db.init_db()
    logger.info("PostgreSQL 连接池已就绪")
    mqtt_client.run_mqtt_in_thread(asyncio.get_running_loop())
    yield
    mqtt_client.stop_mqtt()
    await db.close_db()
    logger.info("BookGuard 云端后端已停止")


app = FastAPI(title="BookGuard Cloud API", version="1.0.0", lifespan=lifespan)
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

@app.get("/")
async def root():
    return {"status": "ok", "service": "bookguard-cloud"}


# ============================================================
# 基础 API
# ============================================================

@app.get("/api/health")
async def health():
    return {"status": "ok", "version": "1.0.0"}


@app.get("/api/shelf/current")
async def shelf_current():
    """当前书架快照"""
    async with db.pool.acquire() as conn:
        row = await conn.fetchrow("SELECT * FROM shelf_snapshot WHERE id = 1")
        return dict(row) if row else {"total_books": 0, "occupied_slots": 0, "total_slots": 40}


@app.get("/api/sensors/latest")
async def sensors_latest():
    """最新传感器读数"""
    async with db.pool.acquire() as conn:
        row = await conn.fetchrow(
            "SELECT recorded_at, temp_c, hum_pct, lux, dew_point_c "
            "FROM sensor_log ORDER BY recorded_at DESC LIMIT 1"
        )
        return dict(row) if row else {"message": "暂无数据"}


@app.get("/api/events/recent")
async def events_recent(limit: int = 20):
    """最近借阅事件"""
    async with db.pool.acquire() as conn:
        rows = await conn.fetch(
            "SELECT occurred_at, event_type, epc, title, layer "
            "FROM shelf_events ORDER BY occurred_at DESC LIMIT $1", limit
        )
        return [dict(r) for r in rows]


# ============================================================
# 数据分析 API
# ============================================================

@app.get("/api/sensors/24h")
async def sensors_24h():
    """最近 24 小时传感器曲线（每小时采样）"""
    return await db.get_sensors_24h()


@app.get("/api/sensors/recent")
async def sensors_recent(minutes: int = 120):
    """最近 N 分钟传感器曲线（每分钟采样，默认 120 分钟）"""
    if minutes < 1 or minutes > 1440:
        raise HTTPException(400, "minutes 范围: 1-1440")
    return await db.get_sensors_recent(minutes)


@app.get("/api/sensors/summary")
async def sensors_summary():
    """7 天环境摘要 + 建议"""
    return await db.get_sensors_summary()


@app.get("/api/books/{epc}/history")
async def book_history(epc: str, limit: int = 50):
    """某本书的借阅历史"""
    if not epc:
        raise HTTPException(400, "epc 不能为空")
    return await db.get_book_history(epc, limit)


@app.get("/api/books/search")
async def search_books(q: str = "", limit: int = 20):
    """模糊搜索书名/EPC"""
    if not q.strip():
        raise HTTPException(400, "q 不能为空")
    return await db.search_books(q.strip(), limit)


@app.get("/api/stats/popular")
async def popular_books(limit: int = 10):
    """热门书籍排行 Top N"""
    return await db.get_popular_books(limit)


@app.get("/api/stats/daily")
async def daily_stats():
    """今日借阅统计"""
    return await db.get_daily_stats()


# ============================================================
# 推荐引擎
# ============================================================

@app.get("/api/recommend/related")
async def recommend_related(epc: str, limit: int = 8):
    """关联推荐 — 借过此书的人也借过哪些书"""
    if not epc:
        raise HTTPException(400, "epc 不能为空")
    return await db.get_related_books(epc, limit)


@app.get("/api/recommend/for-you")
async def recommend_for_you(limit: int = 10):
    """综合推荐 — 基于最近借阅的关联书 + 热门补位"""
    return await db.get_recommend_for_user(limit)


# ============================================================
# 启动
# ============================================================

if __name__ == "__main__":
    import uvicorn
    uvicorn.run("main:app", host=API_HOST, port=API_PORT, log_level="info")
