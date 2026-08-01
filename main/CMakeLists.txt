idf_component_register(SRCS "main.c" "remote.c" "wifi_prov.c"
                    INCLUDE_DIRS "."
                    REQUIRES ssd1306 esp_adc esp_driver_gpio
                             esp_wifi esp_netif esp_event
                             esp_http_client esp_http_server mbedtls
                             nvs_flash lwip
                             esp_https_ota bootloader_support
                             dns_server esp_timer
                    PRIV_REQUIRES app_update esp_hw_support
                    EMBED_TXTFILES "portal.html")
