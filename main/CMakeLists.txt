idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_wifi esp_netif esp_http_server nvs_flash esp_timer json esp_psram esp32-camera)
