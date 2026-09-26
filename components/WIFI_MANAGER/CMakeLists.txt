idf_component_register(SRCS "wifi_manager.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_event esp_http_server esp_netif esp_wifi lwip nvs_flash wifi_provisioning
                       EMBED_TXTFILES "ap_provision.html")
