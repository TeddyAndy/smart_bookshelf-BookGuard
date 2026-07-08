CROSS_COMPILE ?= $(HOME)/Downloads/sdk/ELF-RK3506-linux-source/prebuilts/gcc/linux-x86/arm/gcc-arm-10.3-2021.07-x86_64-arm-none-linux-gnueabihf/bin/arm-none-linux-gnueabihf-
CC      = $(CROSS_COMPILE)gcc
CFLAGS  = -Wall -O2 -g
BOARD_IP    ?= 192.168.0.232
BOARD_USER  ?= root
BOARD_DIR   ?= /root/test/

# --- 目标 ---
TARGET_WS2812B  = ws2812b_test
TARGET_SHELF    = shelf_led_test

# --- 依赖 ---
OBJ_WS2812B     = ws2812b.o
OBJ_SHELF       = shelf_led.o

.PHONY: all clean upload upload-shelf

all: $(TARGET_WS2812B) $(TARGET_SHELF)

# ================================================================
# ws2812b 底层测试
# ================================================================
$(TARGET_WS2812B): ws2812b_test.o $(OBJ_WS2812B)
	$(CC) $(CFLAGS) -o $@ $^

ws2812b_test.o: ws2812b_test.c ws2812b.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_WS2812B): ws2812b.c ws2812b.h
	$(CC) $(CFLAGS) -c -o $@ $<

# ================================================================
# shelf_led 书架灯带库 + 测试
# ================================================================
$(TARGET_SHELF): shelf_led_test.o $(OBJ_SHELF) $(OBJ_WS2812B)
	$(CC) $(CFLAGS) -o $@ $^

shelf_led_test.o: shelf_led_test.c shelf_led.h ws2812b.h
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_SHELF): shelf_led.c shelf_led.h ws2812b.h
	$(CC) $(CFLAGS) -c -o $@ $<

# ================================================================
# 上传
# ================================================================
upload: all
	ssh $(BOARD_USER)@$(BOARD_IP) "mkdir -p $(BOARD_DIR)"
	scp $(TARGET_WS2812B) $(TARGET_SHELF) $(BOARD_USER)@$(BOARD_IP):$(BOARD_DIR)

clean:
	rm -f $(TARGET_WS2812B) $(TARGET_SHELF) *.o
