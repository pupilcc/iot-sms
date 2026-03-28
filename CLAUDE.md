# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESP-IDF firmware for ESP32-C3 that receives SMS messages via a 4G Cat.1 modem (UART) and publishes them to an MQTT broker over Wi-Fi. Uses FreeRTOS for task management.

**Target hardware**: ESP32-C3
**ESP-IDF version**: 5.3.1
**Modem**: 4G Cat.1 module with AT command interface (tested with Air724UG)

## Build and Development Commands

```bash
# Set up ESP-IDF environment (required before any idf.py command)
. $HOME/esp/esp-idf/export.sh

# Build
idf.py build

# Clean build
idf.py fullclean && idf.py build

# Configure project (Wi-Fi, UART, MQTT, timezone, SIM number)
idf.py menuconfig  # Navigate to: Application Configuration

# Flash and monitor
idf.py -p /dev/ttyUSB0 flash monitor  # Exit monitor: Ctrl+]
```

There are no unit tests or linting tools configured for this project.

## Architecture

### Data Flow

```
4G Cat.1 Modem (UART)
    → uart_at_manager (parses AT URCs, reassembles fragmented SMS)
    → FreeRTOS SMS queue (g_sms_queue, capacity: 10)
    → sms_processor (publishes to MQTT, retries with NVS persistence)
    → MQTT Broker (JSON over Wi-Fi)
```

### Components

- **main.c** — Entry point. Initializes NVS, Wi-Fi, SMS queue, spawns tasks, starts MQTT, syncs time via SNTP. Wi-Fi failure halts the application.
- **wifi_manager** — Blocking Wi-Fi STA connection with retry.
- **uart_at_manager** — UART communication with modem. Sends AT commands synchronously with timeout. Handles `+CMT` URCs for incoming SMS. Parses both GSM 7-bit and UCS2 (UTF-16BE→UTF-8) encoding. Detects SIM operator via IMSI lookup (`s_operator_map`). Runs two internal tasks: `uart_event_task` (UART data collection) and `uart_at_task` (AT command/URC processing).
- **mqtt_manager** — MQTT client. `mqtt_manager_publish_sms()` publishes SMS as JSON. `mqtt_manager_publish_device_ready()` sends a ready message with operator info to `esp32/drive` topic.
- **sms_processor** — Reads from SMS queue, publishes via MQTT. Non-blocking retry mechanism (3 attempts, 10s interval). On exhausted retries, persists to NVS via sms_storage. On startup, retries any SMS stored from previous sessions.
- **sms_storage** — NVS-backed persistence for failed SMS messages. FIFO queue in NVS namespace `sms_failed`, max 20 messages. Uses blob storage with key shifting on delete.
- **sntp_manager** — SNTP time synchronization. Non-critical; initialized last.

### Key Data Structure

```c
typedef struct {
    char sender[32];     // Phone number
    char content[2048];  // SMS content (supports ~4-5 concatenated SMS fragments)
} sms_message_t;
```

### Task Configuration (in main.c)

| Task | Priority | Stack |
|------|----------|-------|
| `uart_at_task` | 6 | 8192 bytes |
| `sms_processor_task` | 4 | 10240 bytes |

### Concurrency

- `g_sms_queue` (QueueHandle_t) — producer-consumer between UART task and SMS processor
- `g_sim_operator` (char[32]) — SIM operator name, set during modem init
- `g_sim_phone_number` (char[20]) — SIM phone number (configurable via menuconfig)

### Configuration (Kconfig.projbuild)

All settings under `Application Configuration` menu:
- Wi-Fi SSID/password
- UART port, TX/RX pins (default: UART1, GPIO21/GPIO20), baud rate (default: 115200)
- MQTT broker URI and SMS topic
- SIM phone number (optional)
- Timezone in POSIX format (default: UTC0)

## Common Development Workflows

### Adding a New AT Command

Use `at_send_command()` in `uart_at_manager.c`, parse response with `strstr()`/`sscanf()`, handle "ERROR" responses.

### Adding a New SIM Operator

Add entry to `s_operator_map` array in `uart_at_manager.c` with MCC/MNC prefix and operator name.

### Modifying MQTT Payload

Edit `mqtt_manager_publish_sms()` in `mqtt_manager.c`. Current format: JSON with sender, content, operator, timestamp.

### Debugging UART/AT Communication

```c
esp_log_level_set("uart_at_manager", ESP_LOG_VERBOSE);
```
