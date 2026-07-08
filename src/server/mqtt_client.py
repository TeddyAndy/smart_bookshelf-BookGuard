"""MQTT 订阅客户端 — 订阅 EMQX 主题并写入 PostgreSQL"""

import asyncio
import json
import logging
import threading

import paho.mqtt.client as mqtt

import db
from config import MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASS, MQTT_TOPICS

logger = logging.getLogger("bookguard.mqtt")

client: mqtt.Client | None = None
_main_loop: asyncio.AbstractEventLoop | None = None


def _done_callback(future):
    """协程完成回调，记录错误"""
    exc = future.exception()
    if exc:
        logger.error(f"数据库写入失败: {exc}")


def _run_async(coro):
    """将异步协程调度到主事件循环（非阻塞）"""
    if _main_loop is None or not _main_loop.is_running():
        logger.error("主事件循环未就绪")
        return
    future = asyncio.run_coroutine_threadsafe(coro, _main_loop)
    future.add_done_callback(_done_callback)


def on_connect(client, userdata, flags, reason_code, properties):
    if reason_code == 0:
        logger.info(f"MQTT 已连接: {MQTT_HOST}:{MQTT_PORT}")
        for topic, qos in MQTT_TOPICS:
            client.subscribe(topic, qos)
            logger.info(f"  已订阅: {topic}")
    else:
        logger.warning(f"MQTT 连接失败: rc={reason_code}")


def on_disconnect(client, userdata, flags, reason_code, properties):
    logger.warning(f"MQTT 断开: rc={reason_code}, 将自动重连")


def on_message(client, userdata, msg):
    """处理收到的 MQTT 消息（非阻塞调度）"""
    try:
        payload = json.loads(msg.payload.decode("utf-8"))
        topic = msg.topic
        logger.info(f"MQTT {topic}: {json.dumps(payload, ensure_ascii=False)[:120]}")

        if topic == "bookshelf/sensors":
            # FSM 用 "timestamp" (Unix秒), insert_sensor 期待 "ts"
            if "ts" not in payload and "timestamp" in payload:
                payload["ts"] = payload["timestamp"]
            _run_async(db.insert_sensor(payload))

        elif topic == "bookshelf/shelf":
            # 兼容: bookshelf/shelf 直接更新快照 (手动/批量同步时使用)
            if "ts" not in payload and "timestamp" in payload:
                payload["ts"] = payload["timestamp"]
            _run_async(db.update_snapshot(payload))

        elif topic == "bookshelf/event":
            # FSM 用 "event" 键, insert_event 期待 "event_type"
            # 映射: borrowed→borrow, returned→return, misplaced→misplaced
            raw_event = payload.get("event", "")
            event_map = {
                "borrowed": "borrow",
                "returned": "return",
                "misplaced": "misplaced",
            }
            payload["event_type"] = event_map.get(raw_event, raw_event or "scan")
            if "ts" not in payload and "timestamp" in payload:
                payload["ts"] = payload["timestamp"]
            _run_async(db.insert_event(payload))

        elif topic == "bookshelf/status":
            # bookshelf/status 是 FSM 全量快照，包含 shelf + sensors 两个子对象
            # 1) 提取书架数据 → update_snapshot
            shelf = payload.get("shelf", {})
            if shelf:
                snapshot = {
                    "ts": payload.get("timestamp", 0),
                    "total_books": shelf.get("total", 0),
                    "occupied_slots": shelf.get("present", 0),
                    "total_slots": 40,
                    "books": shelf.get("books", []),
                }
                _run_async(db.update_snapshot(snapshot))

            # 2) 提取传感器数据 → insert_sensor (sensors 子对象)
            sensors = payload.get("sensors", {})
            if sensors and "temp_c" in sensors:
                if "ts" not in sensors:
                    sensors["ts"] = payload.get("timestamp", 0)
                _run_async(db.insert_sensor(sensors))
    except json.JSONDecodeError:
        logger.warning(f"非 JSON 消息: {msg.payload[:100]}")
    except Exception as e:
        logger.error(f"消息处理异常: {e}", exc_info=True)


def start_mqtt():
    """启动 MQTT 客户端（阻塞，在独立线程中运行）"""
    global client
    client = mqtt.Client(
        mqtt.CallbackAPIVersion.VERSION2,
        client_id="bookguard-server",
    )
    client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.on_connect = on_connect
    client.on_disconnect = on_disconnect
    client.on_message = on_message

    client.will_set("bookshelf/status", json.dumps({"online": False}), qos=1, retain=True)
    client.reconnect_delay_set(min_delay=1, max_delay=30)
    client.connect_async(MQTT_HOST, MQTT_PORT, 60)
    client.loop_forever()


def run_mqtt_in_thread(loop: asyncio.AbstractEventLoop):
    """在 daemon 线程中运行 MQTT，绑定主事件循环"""
    global _main_loop
    _main_loop = loop
    t = threading.Thread(target=start_mqtt, daemon=True, name="mqtt")
    t.start()
    logger.info("MQTT 线程已启动")
    return t


def stop_mqtt():
    """停止 MQTT 客户端"""
    global client
    if client:
        client.loop_stop()
        client.disconnect()
        logger.info("MQTT 已停止")
