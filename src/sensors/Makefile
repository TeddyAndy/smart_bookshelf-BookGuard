# I2C 传感器合并读取 — BH1750 光照 + SHT30 温湿度
CC = arm-none-linux-gnueabihf-gcc
CFLAGS = -Wall -O2 -g -lm
TARGET = i2c_sensors
WATCH_TARGET = i2c_watch
SRC = i2c_sensors.c
WATCH_SRC = i2c_watch.c
BOARD_IP ?= 192.168.0.232
BOARD_USER ?= root
BOARD_DIR ?= /root/test/

.PHONY: all clean upload upload-watch desktop desktop-watch info

all: $(TARGET) $(WATCH_TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

$(WATCH_TARGET): $(WATCH_SRC)
	$(CC) -Wall -O2 -g -o $@ $<

# 桌面编译 (本地测试)
desktop:
	gcc -Wall -O2 -g -o $(TARGET) $(SRC) -lm

desktop-watch:
	gcc -Wall -O2 -g -o $(WATCH_TARGET) $(WATCH_SRC)

clean:
	rm -f $(TARGET) $(WATCH_TARGET)

upload: $(TARGET)
	scp $(TARGET) $(BOARD_USER)@$(BOARD_IP):$(BOARD_DIR)

upload-watch: $(WATCH_TARGET)
	scp $(WATCH_TARGET) $(BOARD_USER)@$(BOARD_IP):$(BOARD_DIR)

info: $(TARGET)
	@file $(TARGET)
	@ls -lh $(TARGET)
