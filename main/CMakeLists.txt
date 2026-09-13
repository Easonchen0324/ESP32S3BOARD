idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES BSP nvs_flash esp_netif esp_lvgl_port lvgl GUI_GUIDER SD_WEB WIFI_MANAGER)
