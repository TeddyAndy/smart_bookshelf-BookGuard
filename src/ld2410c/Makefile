# LD2410C 毫米波雷达 — 库 + CLI + 实时监测

CC       = arm-none-linux-gnueabihf-gcc
CFLAGS   = -Wall -O2
TARGETS  = ld2410c_test radar_monitor

.PHONY: all clean upload

all: $(TARGETS)

ld2410c_test: ld2410c.o ld2410c_test.o
	$(CC) $(CFLAGS) -o $@ $^

radar_monitor: ld2410c.o radar_monitor.o
	$(CC) $(CFLAGS) -o $@ $^

ld2410c.o: ld2410c.c ld2410c.h
	$(CC) $(CFLAGS) -c ld2410c.c -o $@

ld2410c_test.o: ld2410c_test.c ld2410c.h
	$(CC) $(CFLAGS) -c ld2410c_test.c -o $@

radar_monitor.o: radar_monitor.c ld2410c.h
	$(CC) $(CFLAGS) -c radar_monitor.c -o $@

# 桌面编译测试
desktop:
	gcc -Wall -O2 -o ld2410c_test_desktop ld2410c.c ld2410c_test.c
	gcc -Wall -O2 -o radar_monitor_desktop ld2410c.c radar_monitor.c

clean:
	rm -f $(TARGETS) *.o ld2410c_test_desktop radar_monitor_desktop

upload: $(TARGETS)
	scp $(TARGETS) root@192.168.0.232:/root/test/

info: $(TARGETS)
	@file $(TARGETS)
	@ls -lh $(TARGETS)
