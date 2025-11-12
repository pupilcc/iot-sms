# IoT SMS Gateway

An ESP-IDF firmware project that receives SMS messages via a 4G Cat.1 modem and publishes them to an MQTT broker over Wi-Fi.

## Features

- 📱 **SMS Reception**: Receives SMS messages through 4G Cat.1 modem via UART
- 🌐 **Wi-Fi Connectivity**: Connects to Wi-Fi network for internet access
- 📡 **MQTT Publishing**: Publishes received SMS to configurable MQTT broker
- 🔄 **Real-time Processing**: FreeRTOS-based architecture for concurrent task handling
- 🌍 **Unicode Support**: Handles UCS2 encoded SMS (Chinese, Arabic, etc.) with UTF-8 conversion
- 📞 **SIM Information**: Automatically detects SIM operator based on IMSI

## Hardware Requirements

- **Microcontroller**: ESP32
- **Modem**: 4G Cat.1 module with AT command interface (e.g., Air724UG)
- **Connection**: UART interface between ESP32-C3 and modem
- **Power Supply**: 5V USB or compatible power source

## Supported Modems

This project uses standard AT commands and should work with most 4G Cat.1 modules, including:
- Air724UG (tested)
- Other modules supporting standard GSM/LTE AT commands

## Software Requirements

- **ESP-IDF**: Version 5.3.1 or higher
- **Python**: 3.8+ (for ESP-IDF tools)

## Getting Started

### 1. Install ESP-IDF

Follow the [official ESP-IDF installation guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/get-started/index.html).

### 2. Clone the Repository

```bash
git clone <repository-url>
cd iot-sms
```

### 3. Configure the Project

```bash
# Set up ESP-IDF environment
. $HOME/esp/esp-idf/export.sh

# Open configuration menu
idf.py menuconfig
```

Navigate to **Application Configuration** and set:
- **Wi-Fi SSID**: Your Wi-Fi network name
- **Wi-Fi Password**: Your Wi-Fi password
- **UART Port Number**: UART port for modem (default: 1)
- **UART TXD Pin**: TX pin number (default: GPIO 21)
- **UART RXD Pin**: RX pin number (default: GPIO 20)
- **UART Baud Rate**: Baud rate (default: 115200)
- **MQTT Broker URI**: Your MQTT broker address (e.g., `mqtt://broker.emqx.io:1883`)
- **MQTT Topic**: Topic for SMS messages (default: `esp32/sms`)

### 4. Build and Flash

```bash
# Build the project
idf.py build

# Flash to device (replace /dev/ttyUSB0 with your port)
idf.py -p /dev/ttyUSB0 flash

# Monitor output
idf.py -p /dev/ttyUSB0 monitor
```

To exit the monitor, press `Ctrl+]`.

## Project Architecture

### Component Overview

```
┌─────────────────────────────────────────────────────────┐
│                      ESP32-C3                            │
│                                                          │
│  ┌──────────────┐    ┌─────────────────┐               │
│  │ Wi-Fi Manager│◄───┤ Main (app_main) │               │
│  └──────────────┘    └─────────────────┘               │
│         │                     │                          │
│         │             ┌───────▼────────┐                │
│    ┌────▼─────┐       │  SMS Queue     │                │
│    │  MQTT    │       │  (FreeRTOS)    │                │
│    │ Manager  │       └───────┬────────┘                │
│    └────▲─────┘               │                          │
│         │             ┌───────▼────────┐                │
│         │             │ SMS Processor  │                │
│         │             │     (task)     │                │
│         └─────────────┤                │                │
│                       └────────────────┘                │
│                                                          │
│         ┌─────────────────────────┐                     │
│         │  UART AT Manager (task) │                     │
│         └───────────┬─────────────┘                     │
│                     │ UART                               │
└─────────────────────┼────────────────────────────────────┘
                      │
              ┌───────▼────────┐
              │  4G Cat.1      │
              │  Modem Module  │
              └────────────────┘
```

### Data Flow

1. **4G Modem** receives SMS and sends `+CMT` URC (Unsolicited Result Code) via UART
2. **UART AT Manager** parses the SMS (sender, content) and enqueues it
3. **SMS Processor** dequeues the SMS message
4. **MQTT Manager** publishes the SMS to the MQTT broker as JSON

### MQTT Message Format

```json
{
  "sender": "+8613800000000",
  "content": "Hello World",
  "operator": "中国移动",
  "timestamp": "2025-11-12T10:30:00Z"
}
```

## Configuration Files

- **Kconfig.projbuild**: Configuration options for Wi-Fi, UART, and MQTT
- **sdkconfig.defaults**: Default SDK configuration values
- **CMakeLists.txt**: Build configuration

## Project Structure

```
iot-sms/
├── main/
│   ├── main.c                 # Application entry point
│   ├── wifi_manager.c/h       # Wi-Fi connection management
│   ├── uart_at_manager.c/h    # UART and AT command handling
│   ├── mqtt_manager.c/h       # MQTT client management
│   ├── sms_processor.c/h      # SMS processing task
│   ├── CMakeLists.txt         # Component build config
│   └── Kconfig.projbuild      # Project configuration menu
├── CMakeLists.txt             # Project build config
├── sdkconfig.defaults         # Default configuration
├── LICENSE                    # Apache 2.0 License
├── CLAUDE.md                  # Developer documentation
└── README.md                  # This file
```

## Troubleshooting

### Modem Not Responding

1. Check UART connections (TX, RX, GND)
2. Verify baud rate matches modem configuration (default: 115200)
3. Ensure modem has adequate power supply (4G modules draw significant current)
4. Check if SIM card is inserted and activated

### Wi-Fi Connection Failed

1. Verify SSID and password in configuration
2. Check Wi-Fi signal strength
3. Ensure 2.4GHz Wi-Fi is used (ESP32-C3 doesn't support 5GHz)

### SMS Not Received

1. Check SIM card has SMS service activated
2. Verify modem initialization logs for errors
3. Set log level to DEBUG for detailed AT command logs:
   ```c
   esp_log_level_set("uart_at_manager", ESP_LOG_DEBUG);
   ```
4. Test SMS reception by sending a test message to the SIM card number

### MQTT Connection Failed

1. Verify MQTT broker URI is correct and accessible
2. Check if broker requires authentication (not implemented in this version)
3. Ensure Wi-Fi is connected before MQTT connection attempts

## Development

### Adding New AT Commands

To add support for new AT commands, modify `uart_at_manager.c`:

1. Use `at_send_command()` function to send commands
2. Parse the response buffer for expected data
3. Handle "OK", "ERROR", and timeout cases

### Customizing MQTT Payload

Edit `mqtt_manager_publish_sms()` in `mqtt_manager.c` to change the JSON format or add additional fields.

### Adjusting Task Priorities

Modify task creation in `main.c` `app_main()`:
- `uart_at_task`: Priority 6, Stack 4096 bytes
- `sms_processor_task`: Priority 4, Stack 3072 bytes

## Supported Operators

The system automatically detects the following SIM operators based on IMSI:

**China:**
- 中国移动 (China Mobile): 46000, 46002, 46007, 46008
- 中国联通 (China Unicom): 46001, 46006, 46009, 46010
- 中国电信 (China Telecom): 46003, 46005, 46011, 46012
- 中国广电 (China Broadcasting): 46015

**International:**
- Giffgaff (UK): 23410
- Skinny (NZ): 53005

Additional operators can be added in `uart_at_manager.c` in the `s_operator_map` array.

## Known Limitations

- SMS must be in text mode (PDU mode not supported)
- MQTT broker authentication not implemented
- Only supports unsolicited SMS notifications (no polling)
- Queue size limited to 10 SMS messages
- Wi-Fi connection failure halts the application

## License

This project is licensed under the Apache License 2.0 - see the [LICENSE](LICENSE) file for details.

## Disclaimer

⚠️ **AI-Generated Code**: All code in this project was generated by Artificial Intelligence. While the code has been structured to follow best practices and includes error handling, it should be thoroughly tested and reviewed before use in production environments.

**Important Notes:**
- This is an experimental project for educational and development purposes
- Code quality, security, and reliability should be independently verified
- The AI-generated code may contain bugs or unexpected behavior
- Users are responsible for testing and validating the code for their specific use cases
- No warranty is provided for the functionality or fitness for any particular purpose

## Contributing

Contributions are welcome! Please feel free to submit issues or pull requests.

## Acknowledgments

- ESP-IDF framework by Espressif Systems
- MQTT library (ESP-MQTT component)
- FreeRTOS real-time operating system

## Support

For issues and questions:
- Open an issue on the GitHub repository
- Refer to [ESP-IDF documentation](https://docs.espressif.com/projects/esp-idf/en/latest/)
- Check [CLAUDE.md](CLAUDE.md) for developer-specific guidance

---

**Made with AI** 🤖 | **Powered by ESP-IDF** ⚡ | **Licensed under Apache 2.0** 📄
