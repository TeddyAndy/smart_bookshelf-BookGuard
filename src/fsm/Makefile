CC     = arm-none-linux-gnueabihf-gcc
CFLAGS = -Wall -O2 -g
BIN    = fsm_main

# 依赖模块
WS_DIR   = ../ws2812b
UHF_DIR  = ../uhf
DB_DIR   = ../db
RADAR_DIR = ../ld2410c
MQTT_DIR  = ../mqtt
WIFI_DIR  = ../wifi
COMMON_DIR = ../common

MODULES = $(WS_DIR)/ws2812b.o $(WS_DIR)/shelf_led.o \
          $(UHF_DIR)/uhf_driver.o $(UHF_DIR)/uhf_scanner.o \
          $(DB_DIR)/db.o $(DB_DIR)/sqlite3.o \
          $(RADAR_DIR)/ld2410c.o \
          $(MQTT_DIR)/mqtt_client.o $(MQTT_DIR)/json_builder.o \
          $(WIFI_DIR)/wifi_manager.o \
          $(COMMON_DIR)/sb_ipc.o
LIBS = -lpthread -ldl -lm

INC = -I$(WS_DIR) -I$(UHF_DIR) -I$(DB_DIR) -I$(RADAR_DIR) -I$(MQTT_DIR) -I$(WIFI_DIR) -I$(COMMON_DIR)

$(BIN): fsm.o main.o $(MODULES)
	$(CC) $(CFLAGS) $(INC) -o $@ fsm.o main.o $(MODULES) $(LIBS)

fsm.o: fsm.c fsm.h $(UHF_DIR)/uhf_scanner.h $(DB_DIR)/db.h $(RADAR_DIR)/ld2410c.h $(MQTT_DIR)/mqtt_client.h $(WIFI_DIR)/wifi_manager.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

main.o: main.c fsm.h
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

$(WIFI_DIR)/wifi_manager.o: $(WIFI_DIR)/wifi_manager.c $(WIFI_DIR)/wifi_manager.h
	$(CC) $(CFLAGS) -I$(WIFI_DIR) -c -o $@ $<

$(COMMON_DIR)/sb_ipc.o: $(COMMON_DIR)/sb_ipc.c $(COMMON_DIR)/sb_ipc.h
	$(CC) $(CFLAGS) -I$(COMMON_DIR) -c -o $@ $<

upload: $(BIN)
	scp $(BIN) root@192.168.0.232:/root/

clean:
	rm -f $(BIN) *.o

.PHONY: upload clean
