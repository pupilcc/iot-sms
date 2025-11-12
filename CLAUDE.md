# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is an ESP-IDF firmware project for ESP32-C3 that receives SMS messages via a 4G Cat.1 modem (connected over UART) and publishes them to an MQTT broker over Wi-Fi. The project uses FreeRTOS for task management and includes components for Wi-Fi, UART/AT command handling, MQTT publishing, and SMS processing.

**Target hardware**: ESP32-C3
**ESP-IDF version**: 5.3.1
**Modem**: 4G Cat.1 module with AT command interface (e.g., Air712UG, SIM7600, etc.)

## Build and Development Commands

### Initial Setup
```bash
# Set up ESP-IDF environment (run from ESP-IDF directory)
. $HOME/esp/esp-idf/export.sh

# Configure project settings (Wi-Fi, UART, MQTT)
idf.py menuconfig
```

### Building
```bash
# Full build
idf.py build

# Clean build
idf.py fullclean
idf.py build
```

### Flashing and Monitoring
```bash
# Flash to device
idf.py -p /dev/ttyUSB0 flash

# Monitor serial output
idf.py -p /dev/ttyUSB0 monitor

# Flash and monitor in one command
idf.py -p /dev/ttyUSB0 flash monitor

# Exit monitor: Ctrl+]
```

### Configuration
```bash
# Configure project (sets Wi-Fi SSID/password, UART pins, MQTT broker, etc.)
idf.py menuconfig
# Navigate to: Application Configuration
```

Key configuration items in `Kconfig.projbuild`:
- Wi-Fi SSID and password
- UART port, TX/RX pins (default: UART1, TX=GPIO21, RX=GPIO20)
- UART baud rate (default: 115200)
- MQTT broker URI and topic

## Architecture

### Component Overview

The application follows a modular, task-based architecture using FreeRTOS:

1. **main.c** - Application entry point (`app_main`)
   - Initializes NVS, network stack, and event loop
   - Starts Wi-Fi connection
   - Creates SMS queue for inter-task communication
   - Spawns all FreeRTOS tasks

2. **wifi_manager** (`wifi_manager.c/h`)
   - Manages Wi-Fi station mode connection
   - Blocks until connection established or max retries exhausted
   - Function: `wifi_manager_init_sta()`

3. **uart_at_manager** (`uart_at_manager.c/h`)
   - Handles UART communication with 4G Cat.1 modem
   - Sends AT commands and parses responses
   - Detects incoming SMS notifications (`+CMT` unsolicited result codes)
   - Parses SMS in text mode and UCS2 encoding
   - Retrieves SIM operator info via IMSI
   - Runs in dedicated FreeRTOS task (`uart_at_task`)
   - Puts parsed SMS into queue for processing

4. **mqtt_manager** (`mqtt_manager.c/h`)
   - Manages MQTT client connection and publishing
   - Functions:
     - `mqtt_manager_start()` - Start client
     - `mqtt_manager_publish_sms()` - Publish SMS to configured topic
     - `mqtt_manager_is_connected()` - Check connection status

5. **sms_processor** (`sms_processor.c/h`)
   - FreeRTOS task that reads from SMS queue
   - Publishes SMS messages to MQTT when connected
   - Runs in dedicated task (`sms_processor_task`)

### Data Flow

```
4G Cat.1 Modem (UART)
    ↓ (AT commands + URC)
uart_at_manager (task)
    ↓ (sms_message_t struct)
SMS Queue (FreeRTOS queue)
    ↓
sms_processor (task)
    ↓ (JSON over MQTT)
MQTT Broker (over Wi-Fi)
```

### Key Data Structures

**sms_message_t** (defined in `uart_at_manager.h`):
```c
typedef struct {
    char sender[32];    // SMS sender phone number
    char content[256];  // SMS content (supports UTF-8 after UCS2 decoding)
} sms_message_t;
```

### AT Command Handling

The uart_at_manager implements a state machine for AT command communication:
- Commands are sent synchronously with timeout (default 10s)
- Responses are parsed for "OK", "ERROR", or unsolicited result codes (URC)
- URCs like `+CMT` (incoming SMS) are handled separately
- Uses FreeRTOS event groups for synchronization between UART event task and command sender

### SMS Parsing

SMS messages are retrieved in text mode (`AT+CMGF=1`) and may contain:
- 7-bit GSM encoding (ASCII-like)
- UCS2 encoding (hex-encoded UTF-16BE) - detected and decoded to UTF-8

### Concurrency Model

- **uart_event_task**: Collects UART data into buffer, detects line-based responses
- **uart_at_task**: Main AT manager task that initializes modem and processes commands/URCs
- **sms_processor_task**: Reads from SMS queue and publishes to MQTT
- Global SMS queue (`g_sms_queue`) handles producer-consumer pattern between UART and MQTT tasks

### Global Variables

- `g_sms_queue` (QueueHandle_t) - Shared SMS queue, created in `main.c`
- `g_sim_operator` (char[32]) - SIM operator name (e.g., "中国移动", "Giffgaff")
- `g_sim_phone_number` (char[20]) - SIM phone number (declared but retrieval not fully implemented)

## Common Development Workflows

### Adding New AT Commands

1. Add command string constant in `uart_at_manager.c`
2. Use `at_send_command()` to send and get response
3. Parse response using `strstr()`, `sscanf()`, or custom parser
4. Handle potential "ERROR" responses

### Modifying MQTT Payload Format

Edit `mqtt_manager_publish_sms()` in `mqtt_manager.c` - currently publishes JSON format with sender, content, and metadata.

### Adjusting Task Priorities/Stack Sizes

In `main.c` `app_main()`:
- `uart_at_task`: priority 6, stack 4096 bytes
- `sms_processor_task`: priority 4, stack 3072 bytes

Increase stack size if task shows stack overflow warnings in logs.

### Debugging UART/AT Communication

Set log level to VERBOSE in `main.c`:
```c
esp_log_level_set("uart_at_manager", ESP_LOG_VERBOSE);
```

This shows all UART TX/RX data and AT command details.

## Important Notes

- The modem must support AT command set compatible with standard GSM/LTE AT commands
- Tested with Air712UG, should work with other 4G Cat.1 modules (SIM7600, EC20, etc.)
- SMS must be in text mode (`AT+CMGF=1`), not PDU mode
- UCS2 decoding assumes UTF-16BE encoding (standard for UCS2 in GSM)
- MQTT publish will retry if not connected, but SMS may be lost if queue fills
- Wi-Fi connection failure halts entire application (infinite loop in `main.c:53`)
