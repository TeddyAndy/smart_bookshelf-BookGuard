# Infrared reflection array standalone monitor
CC = arm-none-linux-gnueabihf-gcc
CFLAGS = -Wall -Wextra -O2 -g
TARGET = ir_monitor
LIVE_TARGET = ir_live
SRC = ir_monitor.c
LIVE_SRC = ir_live.c
BOARD_IP ?= 192.168.0.232
BOARD_USER ?= root
BOARD_DIR ?= /root/test/

.PHONY: all clean upload upload-live desktop desktop-live info

all: $(TARGET) $(LIVE_TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

$(LIVE_TARGET): $(LIVE_SRC)
	$(CC) $(CFLAGS) -o $@ $<

desktop:
	gcc $(CFLAGS) -o $(TARGET) $(SRC)

desktop-live:
	gcc $(CFLAGS) -o $(LIVE_TARGET) $(LIVE_SRC)

clean:
	rm -f $(TARGET) $(LIVE_TARGET)

upload: $(TARGET) $(LIVE_TARGET)
	scp $(TARGET) $(LIVE_TARGET) $(BOARD_USER)@$(BOARD_IP):$(BOARD_DIR)

upload-live: $(LIVE_TARGET)
	scp $(LIVE_TARGET) $(BOARD_USER)@$(BOARD_IP):$(BOARD_DIR)

info: $(TARGET)
	@file $(TARGET)
	@ls -lh $(TARGET)
