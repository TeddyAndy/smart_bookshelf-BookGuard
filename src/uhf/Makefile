# UHF RFID — Makefile
# 工具链: arm-none-linux-gnueabihf-gcc (RK3506 ARMv7)
CC      = arm-none-linux-gnueabihf-gcc
CFLAGS  = -Wall -O2 -g

DRIVER_OBJ = uhf_driver.o
SCANNER_OBJ = uhf_scanner.o
TARGETS = uhf_cli uhf_shelf_sweep

# DB 路径
DB_DIR = ../db

all: $(TARGETS)

# ── 驱动 ──
$(DRIVER_OBJ): uhf_driver.c uhf_driver.h
	$(CC) $(CFLAGS) -c -o $@ $<

# ── 扫描器 ──
$(SCANNER_OBJ): uhf_scanner.c uhf_scanner.h uhf_driver.h
	$(CC) $(CFLAGS) -c -o $@ $<

# ── CLI ──
uhf_cli: uhf_cli.c $(DRIVER_OBJ) uhf_driver.h
	$(CC) $(CFLAGS) -o $@ $< $(DRIVER_OBJ)

uhf_shelf_sweep: uhf_shelf_sweep.c $(DRIVER_OBJ) uhf_driver.h
	$(CC) $(CFLAGS) -o $@ $< $(DRIVER_OBJ)

# ── 部署 ──
upload:
	scp uhf_cli root@192.168.0.232:/root/test/

clean:
	rm -f $(TARGETS) *.o

.PHONY: all upload clean
