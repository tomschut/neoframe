idf_component_register(SRCS "main.c" "config.c" "panel.c" "http.c" "core.c" "portal.c"
    "formutil.c" "webui.c" "logbuf.c" "ota.c" "lowpower.c" "schedule.c"
    INCLUDE_DIRS "." REQUIRES nvs_flash esp_wifi esp_event esp_netif esp_http_client
    esp_timer driver json mbedtls esp_http_server lwip app_update esp_https_ota esp_pm)
