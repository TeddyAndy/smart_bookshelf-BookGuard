# Hardware Wiring

## Main Modules

| Module | Interface | Notes |
|---|---|---|
| Upper UHF RFID | USB CH341 serial | Stable `/dev/serial/by-path/...1.2...` path recommended |
| Lower UHF RFID | USB CH341 serial | Stable `/dev/serial/by-path/...1.3...` path recommended |
| LD2410C radar | UART1 `/dev/ttyS1` | 115200 baud |
| BH1750 light sensor | I2C2 `0x23` | Shared I2C bus |
| SHT30 temperature/humidity | I2C2 `0x44` | Shared I2C bus |
| Infrared boards | I2C2 + GPIO32/33/34 | TCA9555 receiver + 74HC595 transmitter control |
| WS2812B LED strip | SPI1 `/dev/spidev1.0` | Default 78 LEDs |
| 7-inch screen | MIPI DSI + touch input | LVGL application |

## Infrared Board Layout

The firmware reserves four 16-point infrared boards. A 38 cm shelf maps to 32 points, roughly 1.1875 cm per point.

| Board | I2C address | Layer | Range | Default |
|---|---:|---:|---|---|
| `upper_left` | `0x21` | 0 | 0-19 cm | disabled |
| `upper_right` | `0x20` | 0 | 19-38 cm | enabled |
| `lower_left` | `0x22` | 1 | 0-19 cm | disabled |
| `lower_right` | `0x24` | 1 | 19-38 cm | disabled |

Shared control lines:

```text
SER/DATA    GPIO32
RCLK/LATCH  GPIO33
SRCLK/CLOCK GPIO34
I2C         /dev/i2c-2
```

Enable additional boards in `src/fsm/fsm.c` by changing the corresponding `g_ir_boards[]` entry.

## Storage

The live database is stored at `/root/bookshelf.db`. Historical logs can be archived to SD card at `/mnt/sdcard/bookshelf/history.db`.

