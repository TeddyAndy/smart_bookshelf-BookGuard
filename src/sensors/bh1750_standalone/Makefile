# BH1750 光照传感器测试程序 Makefile

CC = arm-none-linux-gnueabihf-gcc
CFLAGS = -Wall -O2 -g
TARGET = bh1750_test
SRC = bh1750_test.c

.PHONY: all clean upload

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(TARGET)

# 上传到板子测试目录
upload: $(TARGET)
	scp $(TARGET) root@192.168.0.232:/root/test/

# 查看文件信息
info: $(TARGET)
	@file $(TARGET)
	@ls -lh $(TARGET)
