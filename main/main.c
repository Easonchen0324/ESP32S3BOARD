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
#include "esp_log.h"
#include <stdio.h>


static void touchpad_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    static uint32_t read_error_count;
    esp_lcd_touch_handle_t touch_handle = (esp_lcd_touch_handle_t)lv_indev_get_driver_data(indev);
    esp_lcd_touch_point_data_t touch_data[1] = {0};
    uint8_t touch_count = 0;

    esp_err_t ret = esp_lcd_touch_read_data(touch_handle);
    if (ret != ESP_OK)
    {
        data->state = LV_INDEV_STATE_RELEASED;
        read_error_count++;
        if (read_error_count <= 3 || (read_error_count % 100) == 0)
        {
            ESP_LOGW("main", "Touch read failed (%lu): %s",
                     (unsigned long)read_error_count, esp_err_to_name(ret));
        }
        return;
    }

    ret = esp_lcd_touch_get_data(touch_handle, touch_data, &touch_count, 1);
    if (ret != ESP_OK)
    {
        data->state = LV_INDEV_STATE_RELEASED;
        read_error_count++;
        if (read_error_count <= 3 || (read_error_count % 100) == 0)
        {
            ESP_LOGW("main", "Touch data failed (%lu): %s",
                     (unsigned long)read_error_count, esp_err_to_name(ret));
        }
        return;
    }

    if (touch_count > 0)
    {
        data->point.x = touch_data[0].x;
        data->point.y = touch_data[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
    }
    else
    {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}


static void touch_button_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED)
    {
        lv_obj_t *button = lv_event_get_target_obj(event);
        lv_obj_t *label = lv_event_get_user_data(event);
        lv_label_set_text(label, "TOUCH OK!");
        lv_obj_set_style_bg_color(button, lv_color_hex(0x20B26B), 0);
        ESP_LOGI("main", "Touch button clicked");
    }
}


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
    esp_err_t touch_ret = qspilcd_touch_init();   /* 触摸初始化 */
    if (touch_ret != ESP_OK)
    {
        ESP_LOGE("main", "Touch initialization failed: %s; continue without touch", esp_err_to_name(touch_ret));
    }
    ESP_ERROR_CHECK(qspilcd_init());        /* SPI LCD初始化 */

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));
    ESP_LOGI("main", "LVGL port initialized");

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
    ESP_LOGI("main", "LVGL display added: %p", (void *)display);

    if (touch_ret == ESP_OK)
    {
        lv_indev_t *touch_indev = NULL;
        lvgl_port_lock(0);
        touch_indev = lv_indev_create();
        if (touch_indev != NULL)
        {
            lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
            lv_indev_set_read_cb(touch_indev, touchpad_read_cb);
            lv_indev_set_disp(touch_indev, display);
            lv_indev_set_driver_data(touch_indev, qspilcd_get_touch_handle());
        }
        lvgl_port_unlock();

        if (touch_indev == NULL)
        {
            ESP_LOGE("main", "Failed to add LVGL touch input; continue without touch");
        }
    }

    ESP_LOGI("main", "Waiting for LVGL lock");
    lvgl_port_lock(0);
    ESP_LOGI("main", "LVGL lock acquired");
    // lv_demo_widgets();  /* 暂时替换为全屏绘制验证界面 */
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x1B2735), 0);

    lv_obj_t *top_block = lv_obj_create(screen);
    lv_obj_set_size(top_block, 200, 55);
    lv_obj_align(top_block, LV_ALIGN_TOP_MID, 0, 15);
    lv_obj_set_style_bg_color(top_block, lv_color_hex(0x007ACC), 0);
    lv_obj_set_style_bg_opa(top_block, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(top_block, 0, 0);
    lv_obj_t *top_label = lv_label_create(top_block);
    lv_label_set_text(top_label, "TOP: SPI LCD OK");
    lv_obj_set_style_text_color(top_label, lv_color_white(), 0);
    lv_obj_center(top_label);

    lv_obj_t *touch_button = lv_button_create(screen);
    lv_obj_set_size(touch_button, 150, 90);
    lv_obj_align(touch_button, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(touch_button, lv_color_hex(0xE39B25), 0);
    lv_obj_set_style_bg_opa(touch_button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(touch_button, 0, 0);
    lv_obj_t *touch_label = lv_label_create(touch_button);
    lv_label_set_text(touch_label, "TOUCH TEST");
    lv_obj_set_style_text_color(touch_label, lv_color_white(), 0);
    lv_obj_center(touch_label);
    lv_obj_add_event_cb(touch_button, touch_button_event_cb, LV_EVENT_CLICKED, touch_label);

    lv_obj_t *bottom_block = lv_obj_create(screen);
    lv_obj_set_size(bottom_block, 200, 55);
    lv_obj_align(bottom_block, LV_ALIGN_BOTTOM_MID, 0, -15);
    lv_obj_set_style_bg_color(bottom_block, lv_color_hex(0xE75A3C), 0);
    lv_obj_set_style_bg_opa(bottom_block, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bottom_block, 0, 0);
    lv_obj_t *bottom_label = lv_label_create(bottom_block);
    lv_label_set_text(bottom_label, "BOTTOM: 240x300");
    lv_obj_set_style_text_color(bottom_label, lv_color_white(), 0);
    lv_obj_center(bottom_label);
    ESP_LOGI("main", "Test UI created");
    lvgl_port_unlock();
    ESP_LOGI("main", "LVGL lock released");

    while (1)
    {
        LED0_TOGGLE();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
