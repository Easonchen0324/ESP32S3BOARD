idf_component_register(SRCS "sd_web_server.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_wifi esp_event esp_netif esp_http_server)
