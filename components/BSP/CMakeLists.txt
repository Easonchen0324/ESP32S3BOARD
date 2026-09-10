set(src_dirs
            LED
            MYIIC
            XL9555
            MYSPI
            SPILCD
            QSPILCD)

set(include_dirs
            LED
            MYIIC
            XL9555
            MYSPI
            SPILCD
            QSPILCD)

set(requires
            driver
            esp_lcd
            esp_lcd_touch
            esp_lcd_touch_ft5x06)

set(tk018_vendor_source
            "${CMAKE_CURRENT_LIST_DIR}/../../屏幕资料/ESP_LCD_QSPI_TK018F3716_LVGL/ESP_LCD_QSPI_TK018F3716_LVGL/esp32_lcd_qspi_lvgl/components/espressif__esp_lcd_tk018/esp_lcd_tk018.c")
set(tk018_build_source "${CMAKE_CURRENT_BINARY_DIR}/esp_lcd_tk018_vendor.c")
configure_file(${tk018_vendor_source} ${tk018_build_source} COPYONLY)

idf_component_register(SRC_DIRS ${src_dirs}
                       EXCLUDE_SRCS QSPILCD/esp_lcd_tk018.c
                       INCLUDE_DIRS ${include_dirs}
                       REQUIRES ${requires})

target_sources(${COMPONENT_LIB} PRIVATE ${tk018_build_source})

target_compile_definitions(${COMPONENT_LIB} PRIVATE
            ESP_LCD_TK018_VER_MAJOR=1
            ESP_LCD_TK018_VER_MINOR=0
            ESP_LCD_TK018_VER_PATCH=2)

component_compile_options(-ffast-math -O3 -Wno-error=format=-Wno-format)
