/**
 ******************************************************************************
 * @file        main.c
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.0
 * @date        2025-01-01
 * @brief       SPILCD实验
 * @license     Copyright (c) 2020-2032, 广州市星翼电子科技有限公司
 ******************************************************************************
 * @attention
 * 
 * 实验平台:正点原子 ESP32-S3 开发板
 * 在线视频:www.yuanzige.com
 * 技术论坛:www.openedv.com
 * 公司网址:www.alientek.com
 * 购买地址:openedv.taobao.com
 ******************************************************************************
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "led.h"
#include "myiic.h"
#include "my_spi.h"
#include "qspilcd.h"
#include "xl9555.h"
#include "lvgl.h"
#include "lv_demos.h"
#include "esp_lvgl_port.h"
#include <stdio.h>


/**
 * @brief       程序入口
 * @param       无
 * @retval      无
 */
void app_main(void)
{
    esp_err_t ret;
    ret = nvs_flash_init();     /* 初始化NVS */
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    led_init();                 /* LED初始化 */
    my_spi_init();              /* SPI初始化 */
    myiic_init();               /* IIC初始化 */  
    xl9555_init();              /* 初始化按键 */
    // ESP_ERROR_CHECK(qspilcd_touch_init());  /* 触摸初始化：暂时跳过，先验证屏幕 */
    ESP_ERROR_CHECK(qspilcd_init());        /* QSPI LCD初始化 */

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = qspilcd_get_io_handle(),
        .panel_handle = qspilcd_get_panel_handle(),
        .control_handle = NULL,
        .buffer_size = QSPILCD_H_RES * QSPILCD_V_RES,
        .double_buffer = false,
        .trans_size = 0,
        .hres = QSPILCD_H_RES,
        .vres = QSPILCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = true,
        },
        .rounder_cb = NULL,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
            .swap_bytes = true,
            .full_refresh = true,
            .direct_mode = false,
        },
    };
    lv_display_t *display = lvgl_port_add_disp(&display_cfg);
    // 触摸初始化暂时跳过，不能向 LVGL 注册空触摸句柄。

    lvgl_port_lock(0);
    // lv_demo_widgets();  /* 暂时替换为全屏绘制验证界面 */
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x1B2735), 0);

    lv_obj_t *top_block = lv_obj_create(screen);
    lv_obj_set_size(top_block, 200, 55);
    lv_obj_align(top_block, LV_ALIGN_TOP_MID, 0, 15);
    lv_obj_set_style_bg_color(top_block, lv_color_hex(0x007ACC), 0);
    lv_obj_set_style_border_width(top_block, 0, 0);
    lv_obj_t *top_label = lv_label_create(top_block);
    lv_label_set_text(top_label, "TOP: QSPI LCD OK");
    lv_obj_center(top_label);

    lv_obj_t *middle_block = lv_obj_create(screen);
    lv_obj_set_size(middle_block, 150, 90);
    lv_obj_align(middle_block, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(middle_block, lv_color_hex(0x20B26B), 0);
    lv_obj_set_style_border_width(middle_block, 0, 0);
    lv_obj_t *middle_label = lv_label_create(middle_block);
    lv_label_set_text(middle_label, "MIDDLE");
    lv_obj_center(middle_label);

    lv_obj_t *bottom_block = lv_obj_create(screen);
    lv_obj_set_size(bottom_block, 200, 55);
    lv_obj_align(bottom_block, LV_ALIGN_BOTTOM_MID, 0, -15);
    lv_obj_set_style_bg_color(bottom_block, lv_color_hex(0xE75A3C), 0);
    lv_obj_set_style_border_width(bottom_block, 0, 0);
    lv_obj_t *bottom_label = lv_label_create(bottom_block);
    lv_label_set_text(bottom_label, "BOTTOM: 240x300");
    lv_obj_center(bottom_label);
    lvgl_port_unlock();

    while (1)
    {
        LED0_TOGGLE();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
