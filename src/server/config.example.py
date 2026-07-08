"""BookGuard 云端后端配置 (示例)"""
import os

# PostgreSQL
DB_HOST = os.getenv("DB_HOST", "127.0.0.1")
DB_PORT = int(os.getenv("DB_PORT", "5432"))
DB_NAME = os.getenv("DB_NAME", "bookshelf")
DB_USER = os.getenv("DB_USER", "your_user")
DB_PASS = os.getenv("DB_PASS", "your_password")
DB_DSN = f"postgresql://{DB_USER}:{DB_PASS}@{DB_HOST}:{DB_PORT}/{DB_NAME}"

# EMQX MQTT
MQTT_HOST = os.getenv("MQTT_HOST", "127.0.0.1")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
MQTT_USER = os.getenv("MQTT_USER", "your_mqtt_user")
MQTT_PASS = os.getenv("MQTT_PASS", "your_mqtt_password")
MQTT_TOPICS = [
    ("bookshelf/status", 1), ("bookshelf/sensors", 1),
    ("bookshelf/shelf", 1), ("bookshelf/state", 1),
    ("bookshelf/event", 1), ("bookshelf/cmd/#", 1),
]

API_HOST = os.getenv("API_HOST", "0.0.0.0")
API_PORT = int(os.getenv("API_PORT", "8000"))
