idf_component_register(SRCS "wifi_ota.c"
                       INCLUDE_DIRS "."
                       REQUIRES app_update esp_app_format esp_event esp_http_server esp_netif esp_system esp_wifi
                       EMBED_TXTFILES "ota.html")
