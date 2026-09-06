idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES BSP nvs_flash esp_lvgl_port lvgl)
