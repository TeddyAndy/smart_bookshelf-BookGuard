# Smart Bookshelf

An embedded smart bookshelf prototype based on ELF3506/RK3506. The system combines UHF RFID, infrared occupancy sensing, a local SQLite database, WS2812B LEDs, an LVGL touch UI, MQTT telemetry, and optional cloud/app components.

## Features

- Dual-layer UHF RFID inventory and book identity tracking.
- Infrared occupancy array support for position changes on each shelf.
- State machine for boot self-check, active scanning, sleep, find mode, and fault handling.
- Local SQLite storage with WAL and optional SD-card archive database.
- 7-inch LVGL screen for shelf view, search, add/delete book, WiFi, recent status, and fault retry.
- WS2812B LED guidance for search, misplaced books, and device status.
- MQTT status, event, and sensor publishing.
- FastAPI backend and WeChat Mini Program example clients.

## Repository Layout

```text
src/
  fsm/        Main board state machine
  ui/         LVGL screen application
  uhf/        CPH305F UHF RFID driver and scanner
  infrared/   Infrared array test tools
  db/         SQLite schema and persistence layer
  mqtt/       Lightweight MQTT 3.1.1 client
  wifi/       wpa_supplicant wrapper
  sensors/    BH1750/SHT30 utilities
  ld2410c/    LD2410C radar driver
  ws2812b/    WS2812B LED driver
  app/        WeChat Mini Program example
  server/     FastAPI backend example
scripts/     Board supervisor scripts
docs/        Public wiring and protocol notes
data/        Book thickness reference data
hardware/    Infrared module EDA file
```

## Configuration

This release uses placeholder network settings. Update these before deployment:

- `src/fsm/fsm.h`: `FSM_MQTT_BROKER`, `FSM_MQTT_USER`, `FSM_MQTT_PASS`
- `src/app/utils/config.js`: API and MQTT WebSocket settings for the Mini Program
- `src/server/config.example.py`: PostgreSQL, MQTT, and optional model service settings

Do not commit real WiFi passwords, MQTT passwords, cloud credentials, or production database files to a public repository.

## Build

The board binaries are cross-compiled with the ELF3506/RK3506 Buildroot toolchain.

```bash
cd src/fsm
make -B fsm_main
```

Build the LVGL screen application:

```bash
cmake -S src/ui -B /tmp/sb-ui-arm \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/buildroot/toolchainfile.cmake
cmake --build /tmp/sb-ui-arm -j4
```

## Runtime Files

Default board paths:

```text
/root/fsm_main
/root/SmartBookshelf
/root/bookshelf.db
/mnt/sdcard/bookshelf/history.db
/tmp/fsm_state
/tmp/sb_cmd
/tmp/ui.log
/tmp/fsm_main.log
```

`fsm_main` owns hardware access. The UI process only reads the database/state file and writes commands to `/tmp/sb_cmd`.

## Documentation

- [Hardware Wiring](docs/hardware-wiring.md)
- [MQTT and IPC Protocol](docs/protocol.md)

