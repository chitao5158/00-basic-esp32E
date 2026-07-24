idf_component_register(SRCS "main.c" "remote.c"
                    INCLUDE_DIRS "."
                    REQUIRES ssd1306 esp_adc esp_driver_gpio
                             esp_wifi esp_netif esp_event
                             esp_http_client mbedtls
                             nvs_flash)
