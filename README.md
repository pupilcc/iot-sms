# IoT SMS Gateway

An ESP-IDF firmware for ESP32-C3 that receives SMS messages via a 4G Cat.1 modem and publishes them to an MQTT broker over Wi-Fi.

## Features

- Receives SMS through 4G Cat.1 modem via UART AT commands
- Publishes SMS to configurable MQTT broker as JSON (with sender, content, operator, local number, timestamp)
- Handles UCS2 encoded SMS (Chinese, Arabic, etc.) with automatic UTF-8 conversion
- Supports long SMS reassembly (concatenated SMS fragments up to ~4-5 segments)
- NVS-based SMS persistence: failed publishes are saved to flash and retried on reconnect or reboot
- Automatic SIM operator detection via IMSI lookup
- SNTP time synchronization for accurate message timestamps
- Publishes device ready status to `esp32/device` topic on startup
- FreeRTOS-based concurrent task architecture

## Hardware Requirements

- **Microcontroller**: ESP32-C3
- **Modem**: 4G Cat.1 module with AT command interface (tested with Air724UG)
- **Connection**: UART between ESP32-C3 and modem (default: TX=GPIO21, RX=GPIO20)

## Software Requirements

- **ESP-IDF**: 5.3.1 or higher
- **Python**: 3.8+ (for ESP-IDF tools)

## Getting Started

### 1. Install ESP-IDF

Follow the [official ESP-IDF installation guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/get-started/index.html).

### 2. Clone and Configure

```bash
git clone <repository-url>
cd iot-sms

# Set up ESP-IDF environment
. $HOME/esp/esp-idf/export.sh

# Open configuration menu
idf.py menuconfig
```

Navigate to **Application Configuration** and set:

| Setting | Default | Description |
|---------|---------|-------------|
| Wi-Fi SSID | - | Your 2.4GHz Wi-Fi network name |
| Wi-Fi Password | - | Your Wi-Fi password |
| UART Port Number | 1 | UART port for modem |
| UART TXD Pin | GPIO 21 | TX pin (connects to modem RX) |
| UART RXD Pin | GPIO 20 | RX pin (connects to modem TX) |
| UART Baud Rate | 115200 | Baud rate for modem communication |
| MQTT Broker URI | `mqtt://broker.emqx.io:1883` | Your MQTT broker address |
| MQTT Topic for SMS | `esp32/sms` | Topic for publishing SMS messages |
| SIM Phone Number | (empty) | Local SIM number (optional, included in MQTT payload) |
| Timezone | `UTC0` | POSIX timezone string (e.g., `CST-8` for UTC+8) |

### 3. Build and Flash

```bash
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

Exit monitor with `Ctrl+]`.

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│                       ESP32-C3                           │
│                                                          │
│  app_main                                                │
│    ├── wifi_manager ──── Wi-Fi STA connection             │
│    ├── sntp_manager ──── Time sync (NTP)                  │
│    ├── uart_at_manager ── AT commands / SMS parsing       │
│    │        │                                             │
│    │        ▼                                             │
│    │   SMS Queue (FreeRTOS, capacity: 10)                 │
│    │        │                                             │
│    │        ▼                                             │
│    ├── sms_processor ─── Publish / retry / persist        │
│    │        │                                             │
│    │        ├── mqtt_manager ── MQTT publish               │
│    │        └── sms_storage ─── NVS persistence (max 20)  │
│    └── mqtt_manager ──── MQTT client + device ready msg   │
└──────────────────────────────────────────────────────────┘
         │ UART
    ┌────▼─────────┐
    │ 4G Cat.1     │
    │ Modem Module │
    └──────────────┘
```

### MQTT Message Format

**SMS messages** (topic: `esp32/sms`):
```json
{
  "sender": "+8613800000000",
  "content": "Hello World",
  "local_number": "+8613900000000",
  "operator": "中国移动",
  "timestamp": "2025-11-12T10:30:00Z"
}
```

**Device ready** (topic: `esp32/device`):
```json
{
  "status": "ready",
  "operator": "中国移动",
  "local_number": "+8613900000000",
  "timestamp": "2025-11-12T10:30:00Z"
}
```

### SMS Retry and Persistence

When MQTT publish fails:
1. Retry up to 3 times with 10-second intervals
2. If all retries fail, save to NVS flash storage (up to 20 messages)
3. On next MQTT connection (or reboot), retry stored messages in FIFO order

## Supported Operators

Operator detection is automatic via IMSI prefix lookup:

| Country | Operator | MCC/MNC Prefixes |
|---------|----------|-----------------|
| China | 中国移动 (China Mobile) | 46000, 46002, 46007, 46008 |
| China | 中国联通 (China Unicom) | 46001, 46006, 46009, 46010 |
| China | 中国电信 (China Telecom) | 46003, 46005, 46011, 46012 |
| China | 中国广电 (China Broadcasting) | 46015 |
| UK | Giffgaff | 23410 |
| NZ | Skinny | 53005 |
| HK | Haha | 45403 |

Add new operators in `s_operator_map` array in `main/uart_at_manager.c`.

## Troubleshooting

### Modem Not Responding
- Check UART wiring (TX↔RX, shared GND)
- Verify baud rate matches modem (default: 115200)
- Ensure modem has sufficient power (4G modules draw significant current during transmission)
- Check SIM card is inserted and activated

### Wi-Fi Connection Failed
- Verify SSID and password in menuconfig
- ESP32-C3 only supports 2.4GHz Wi-Fi (not 5GHz)

### SMS Not Received
- Ensure SIM card has SMS service activated
- Set log level to DEBUG/VERBOSE for AT command diagnostics:
  ```c
  esp_log_level_set("uart_at_manager", ESP_LOG_VERBOSE);
  ```
- Send a test SMS to the SIM card number

### MQTT Connection Failed
- Verify broker URI is correct and reachable
- Check logs for Wi-Fi/network diagnostic messages
- Broker authentication is not implemented (plain MQTT only)

## Known Limitations

- SMS text mode only (`AT+CMGF=1`), PDU mode not supported
- MQTT broker authentication not implemented
- Only processes unsolicited SMS notifications (no polling/reading stored SMS)
- Wi-Fi connection failure at startup halts the application
- Queue capacity: 10 SMS in memory, 20 in NVS persistence

## License

Apache License 2.0 - see [LICENSE](LICENSE).
