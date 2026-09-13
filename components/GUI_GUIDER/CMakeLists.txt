idf_component_register(
    SRCS
        "generated/gui_guider.c"
        "generated/setup_scr_main_screen.c"
        "generated/setup_scr_screen_1.c"
        "generated/events_init.c"
        "generated/widgets_init.c"
        "generated/guider_fonts/lv_font_montserratMedium_16.c"
        "generated/guider_fonts/lv_font_montserratMedium_40.c"
        "generated/images/_wifi_RGB565A8_48x48.c"
        "custom/custom.c"
    INCLUDE_DIRS
        "generated"
        "generated/guider_fonts"
        "custom"
    REQUIRES lvgl WIFI_MANAGER
)
