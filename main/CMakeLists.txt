idf_component_register(SRCS "main.c"
                           "wifi_manager.c"
                           "uart_at_manager.c"
                           "mqtt_manager.c"
                           "sms_processor.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES "esp_wifi" "esp_event" "nvs_flash" "esp_netif" "mqtt" "driver")
