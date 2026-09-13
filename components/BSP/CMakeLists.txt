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
            fatfs
            esp_lcd
            esp_lcd_touch
            esp_lcd_touch_ft5x06)

idf_component_register(SRC_DIRS ${src_dirs}
                       INCLUDE_DIRS ${include_dirs}
                       REQUIRES ${requires})

target_compile_definitions(${COMPONENT_LIB} PRIVATE
            ESP_LCD_TK018_VER_MAJOR=1
            ESP_LCD_TK018_VER_MINOR=0
            ESP_LCD_TK018_VER_PATCH=2)

component_compile_options(-ffast-math -O3 -Wno-error=format=-Wno-format)
