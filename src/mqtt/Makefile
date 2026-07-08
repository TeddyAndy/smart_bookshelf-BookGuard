# src/mqtt/Makefile
# 交叉编译 MQTT 客户端 (ARM, 自包含, 零外部依赖)
#
# 工具链优先级:
#   1. arm-none-linux-gnueabihf-gcc (SDK 自带, preferred)
#   2. arm-buildroot-linux-gnueabihf-gcc (Buildroot)
#
CROSS ?= arm-none-linux-gnueabihf-
CC    = $(CROSS)gcc

CFLAGS  = -Wall -Wextra -O2 -g
LDFLAGS = -lpthread

BOARD_IP ?= 192.168.0.232
BOARD_DIR ?= /root/test/

# ── 源文件 ─────────────────────────────────────────
MQTT_SRC = mqtt_client.c json_builder.c
DB_SRC   = ../db/db.c ../db/sqlite3.c

# ── 目标 ───────────────────────────────────────────
TARGETS = mqtt_test mqtt_sensor_bridge

.PHONY: all clean upload upload_sensor

all: $(TARGETS)

mqtt_test: mqtt_test.c $(MQTT_SRC) mqtt_client.h json_builder.h
	$(CC) $(CFLAGS) -o $@ $< $(MQTT_SRC) $(LDFLAGS)

mqtt_sensor_bridge: mqtt_sensor_bridge.c $(MQTT_SRC) $(DB_SRC)
	$(CC) $(CFLAGS) -I.. -o $@ $< $(MQTT_SRC) $(DB_SRC) $(LDFLAGS) -ldl

clean:
	rm -f $(TARGETS) *.o

upload: $(TARGETS)
	ssh root@$(BOARD_IP) "mkdir -p $(BOARD_DIR)"
	scp $(TARGETS) root@$(BOARD_IP):$(BOARD_DIR)/
	@echo "✅ 已上传到 $(BOARD_IP):$(BOARD_DIR)"

upload_sensor: mqtt_sensor_bridge
	ssh root@$(BOARD_IP) "mkdir -p $(BOARD_DIR)"
	scp mqtt_sensor_bridge root@$(BOARD_IP):$(BOARD_DIR)/
	@echo "✅ sensor bridge 已上传"
