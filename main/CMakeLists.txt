idf_component_register(SRCS "main.c" "config.c" "panel.c" "http.c" "core.c"
    INCLUDE_DIRS "." REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_http_client
    esp_timer driver json mbedtls)
