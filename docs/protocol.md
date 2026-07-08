# MQTT and Local IPC Protocol

## MQTT Topics

| Topic | Direction | Payload |
|---|---|---|
| `bookshelf/status` | board to cloud/app | Full shelf snapshot |
| `bookshelf/device/status` | board to cloud/app | Device-only status when shelf data is unavailable |
| `bookshelf/event` | board to cloud/app | Borrow/return/misplaced events |
| `bookshelf/sensors` | board to cloud/app | Temperature, humidity, light, radar |
| `bookshelf/cmd/find` | cloud/app to board | Find a book by title/EPC |
| `bookshelf/cmd/refresh` | cloud/app to board | Request a status snapshot |

## Status Snapshot

```json
{
  "type": "snapshot",
  "status": "online",
  "state": "ACTIVE",
  "timestamp": 1710000000,
  "uptime_sec": 123,
  "shelf": {
    "total": 10,
    "present": 9,
    "missing": 1,
    "books": [
      {
        "epc": "E200...",
        "title": "Book Title",
        "layer": 0,
        "pos_cm": 12.0,
        "present": true,
        "rssi": 180
      }
    ]
  },
  "radar": {
    "has_person": true,
    "move_dist_cm": 80,
    "still_dist_cm": 0
  },
  "sensors": {
    "temp_c": 25.1,
    "hum_pct": 51.3,
    "lux": 120
  },
  "stats": {
    "events_today": 0
  }
}
```

## Local UI IPC

The screen process does not access hardware directly.

```text
/tmp/fsm_state  FSM writes current status for the UI
/tmp/sb_cmd     UI writes one command for the FSM
```

Supported command examples:

```text
FIND <epc-or-query>
FIND_CANCEL
ADD_START
ADD_CANCEL
REGISTER_PAGES <layer> <pages> <title>
DELETE_BOOK <epc>
NET_SCAN
NET_CONNECT <ssid>|<password>
NET_DISCONNECT
FAULT_RETRY
CLEAR_ISSUE
SYSTEM_REBOOT
SYSTEM_POWEROFF
```

