# IoT SMS Gateway

An ESP-IDF firmware for ESP32-C3 that receives SMS messages via a 4G Cat.1 modem and publishes them to an MQTT broker over Wi-Fi.

## Features

- Receives SMS through 4G Cat.1 modem via UART, supporting both standard AT command firmware and Yinerda (银尔达) DTU transparent firmware (selectable in menuconfig)
- Publishes SMS to configurable MQTT broker as JSON (with sender, content, operator, local number, timestamp)
- Handles UCS2 encoded SMS (Chinese, Arabic, etc.) with automatic UTF-8 conversion
- Supports long SMS reassembly (concatenated SMS fragments up to ~4-5 segments)
- NVS-based SMS persistence: failed publishes are saved to flash and retried on reconnect or reboot
- Automatic SIM operator detection (IMSI lookup in AT mode, ICCID prefix in DTU mode)
- Remote logging and device metrics: forwards `ESP_LOG` output in JSON batches and publishes periodic metrics over MQTT
- SNTP time synchronization for accurate message timestamps
- Publishes device ready status to `esp32/device` topic on startup
- FreeRTOS-based concurrent task architecture

## Hardware Requirements

- **Microcontroller**: ESP32-C3
- **Modem**: 4G Cat.1 module, either with AT command firmware (tested with Air724UG) or Yinerda (银尔达) DTU transparent firmware
- **Connection**: UART between ESP32-C3 and modem (see below)

### Pin and UART Assignments

| Function | Peripheral | Pins | Parameters |
|----------|------------|------|------------|
| 4G modem communication | UART1 (`CONFIG_APP_UART_PORT_NUM`) | TX=GPIO0, RX=GPIO1 (project default) | 115200 baud, 8N1, no flow control |
| Log console / flashing | UART0 (primary console) | ESP32-C3 default: TX=GPIO21, RX=GPIO20 | 115200 baud |
| Log console / flashing | USB-Serial-JTAG (secondary console) | GPIO18 (USB D-), GPIO19 (USB D+) | — |

All modem UART settings (port, pins, baud rate) are configurable via `idf.py menuconfig` → **Application Configuration**.

### Wiring

Only three wires are needed between the ESP32-C3 and the modem. With the default
pin configuration (TX=GPIO0, RX=GPIO1):

```
ESP32-C3                        4G Cat.1 Modem
────────                        ──────────────
GPIO0 (UART1 TX)  ───────────►  RXD (main UART)
GPIO1 (UART1 RX)  ◄───────────  TXD (main UART)
GND               ───────────   GND
```

Steps:

1. **Cross-connect the UART**: ESP32-C3 TX goes to the modem's RXD, and ESP32-C3 RX goes to the modem's TXD. Connect to the modem's *main* UART (the one that accepts AT / DTU commands), not its debug/log UART.
2. **Connect GND to GND**: a common ground is required, including when the two boards use separate power supplies. Even if both boards are powered from the same power module (which already ties their grounds together), still run a dedicated short GND wire between the two boards, routed alongside the TX/RX wires: it gives the UART signals a low-noise return path, so the modem's high transmit-burst currents stay on its own power ground wire instead of shifting the signal ground reference (which causes intermittent garbled UART data during transmission).
3. **Check the UART logic level**: the ESP32-C3 uses 3.3V logic. Most 4G dev/DTU boards expose a 3.3V-TTL UART and can be wired directly, but a bare Air724UG module uses 1.8V UART logic and needs a level shifter — check your modem board's documentation.
4. **Power each board separately**: power the ESP32-C3 via its USB port; power the modem from its own supply as specified by its board (4G transmit bursts draw high current, so the supply should handle ≥2A).
5. **Insert the SIM card and attach the 4G antenna** before powering the modem on.

When powering both boards from a single power module, each board's 5V **and** GND
must run back to the power module as a pair — a supply is a complete loop, and each
board's supply current must return to the module on its own GND wire. The
board-to-board GND from step 2 is a third, additional wire:

```
Power module 5V   ────►  ESP32-C3 5V/VIN
Power module GND  ────►  ESP32-C3 GND      ← supply return for the ESP32-C3
Power module 5V   ────►  4G modem 5V/VIN
Power module GND  ────►  4G modem GND      ← supply return for the modem (transmit bursts flow here)
ESP32-C3 GND      ◄───►  4G modem GND      ← signal ground, routed alongside TX/RX
```

The power-supply GND wires carry the supply current; the board-to-board GND wire
ideally carries almost none — it only keeps the two boards' 0V references aligned
for the UART. Both are required, and neither replaces the other.

If you change the pins in menuconfig, wire according to your configured pins instead.

Notes:

- **Pin conflict with UART0 console**: GPIO21/GPIO20 are the ESP32-C3 UART0 default console pins. If the modem UART is mapped to them, it takes over these pins at startup and UART0 console output stops (logs remain available via the USB-Serial-JTAG secondary console). The project default of TX=GPIO0, RX=GPIO1 avoids this conflict.
- RTS/CTS hardware flow control is not used; only TX, RX, and GND need to be wired.
- The firmware does not drive any modem power-key or reset pin — the modem must power up and boot on its own.
- Avoid ESP32-C3 strapping pins (GPIO2, GPIO8, GPIO9) when choosing custom UART pins.
- Wi-Fi is provided by the ESP32-C3's internal radio and requires no external pins.

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
| 4G Modem Firmware Type | AT firmware | Firmware on the modem: standard AT commands, or Yinerda (银尔达) DTU transparent firmware |
| Wi-Fi SSID | - | Your 2.4GHz Wi-Fi network name |
| Wi-Fi Password | - | Your Wi-Fi password |
| UART Port Number | 1 | UART port for modem |
| UART TXD Pin | GPIO 0 | TX pin (connects to modem RX) |
| UART RXD Pin | GPIO 1 | RX pin (connects to modem TX) |
| UART Baud Rate | 115200 | Baud rate for modem communication |
| MQTT Broker URI | `mqtt://broker.emqx.io:1883` | Your MQTT broker address |
| MQTT Topic for SMS | `esp32/sms` | Topic for publishing SMS messages |
| SIM Phone Number | (empty) | Local SIM number (optional, included in MQTT payload) |
| Timezone | `UTC0` | POSIX timezone string (e.g., `CST-8` for UTC+8) |

#### Remote Logging and Metrics

The remote logging task forwards local `ESP_LOG` output in JSON batches and
publishes device metrics through the same MQTT connection. Configure these
options under **Application Configuration**:

| Kconfig option | Default | Description |
|----------------|---------|-------------|
| `CONFIG_APP_DEVICE_NAME` | (empty) | Device identifier included in the `device` field of log and metrics messages. When empty, the firmware derives an ID in the form `esp32c3-xxxxxx` from the Wi-Fi MAC address. |
| `CONFIG_APP_REMOTE_LOG_ENABLE` | `y` | Enables the remote logging and metrics task. Disabling it stops both MQTT log forwarding and metrics publishing; serial console logging is not affected. |
| `CONFIG_APP_MQTT_TOPIC_LOG` | `esp32/log` | MQTT topic used for batched log messages. Messages are published with QoS 0. This option is available when remote logging is enabled. |
| `CONFIG_APP_REMOTE_LOG_LEVEL` | `4` (`DEBUG`) | Most verbose log level forwarded remotely: `1=ERROR`, `2=WARN`, `3=INFO`, `4=DEBUG`, `5=VERBOSE`. The selected level and all more severe levels are forwarded. |
| `CONFIG_APP_MQTT_TOPIC_METRICS` | `esp32/metrics` | MQTT topic used for device metrics. Messages are published with QoS 0. |
| `CONFIG_APP_METRICS_INTERVAL_S` | `60` | Metrics publishing interval in seconds. Set it to a positive integer. The first metrics message is attempted about 5 seconds after the task starts. |

`CONFIG_LOG_MAXIMUM_LEVEL` is the compile-time ceiling for all logging. It must
be at least as verbose as `CONFIG_APP_REMOTE_LOG_LEVEL`; otherwise, more verbose
messages are compiled out and cannot be forwarded. The supplied
`sdkconfig.defaults` sets the ceiling to `DEBUG`. Set both options to `VERBOSE`
if verbose logs need to be forwarded.

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
│    │   (or uart_dtu_manager for Yinerda DTU firmware)     │
│    │        │                                             │
│    │        ▼                                             │
│    │   SMS Queue (FreeRTOS, capacity: 10)                 │
│    │        │                                             │
│    │        ▼                                             │
│    ├── sms_processor ─── Publish / retry / persist        │
│    │        │                                             │
│    │        ├── mqtt_manager ── MQTT publish               │
│    │        └── sms_storage ─── NVS persistence (max 20)  │
│    ├── remote_log ────── Log forwarding + metrics (MQTT)  │
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

Operator detection is automatic. With AT firmware, the operator is resolved via IMSI prefix lookup:

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

With Yinerda DTU firmware, the operator is resolved via ICCID prefix lookup instead
(`s_iccid_operator_map` array in `main/uart_dtu_manager.c`), which currently covers
the mainland China operators only.

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
  esp_log_level_set("uart_at_manager", ESP_LOG_VERBOSE);   // AT firmware
  esp_log_level_set("uart_dtu_manager", ESP_LOG_VERBOSE);  // Yinerda DTU firmware
  ```
- Send a test SMS to the SIM card number

### MQTT Connection Failed
- Verify broker URI is correct and reachable
- Check logs for Wi-Fi/network diagnostic messages
- Broker authentication is not implemented (plain MQTT only)

## Known Limitations

- AT firmware mode: SMS text mode only (`AT+CMGF=1`), PDU mode not supported
- MQTT broker authentication not implemented
- AT firmware mode only processes unsolicited SMS notifications (no polling/reading of stored SMS); DTU mode additionally polls the modem's SMS cache every 10 seconds
- Wi-Fi connection failure at startup halts the application
- Queue capacity: 10 SMS in memory, 20 in NVS persistence

## License

Apache License 2.0 - see [LICENSE](LICENSE).
