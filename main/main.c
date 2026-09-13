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
#include "gui_guider.h"
#include "events_init.h"
#include "custom.h"
#include "sd_web_server.h"
#include <stdio.h>


lv_ui guider_ui;


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

#if 0                           /* 暂时禁用SD卡初始化 */
    vTaskDelay(pdMS_TO_TICKS(500));         /* 等待SD卡上电稳定 */
    esp_err_t sd_ret = ESP_FAIL;
    for (int attempt = 1; attempt <= 5; attempt++)
    {
        sd_ret = my_spi_init();             /* SD卡SPI初始化并挂载FAT文件系统 */
        if (sd_ret == ESP_OK)
        {
            break;
        }

        ESP_LOGW("main", "SD card initialization attempt %d/5 failed: %s",
                 attempt, esp_err_to_name(sd_ret));
        if (attempt < 5)
        {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    if (sd_ret != ESP_OK)
    {
        ESP_LOGE("main", "SD card initialization failed: %s; continue without SD web",
                 esp_err_to_name(sd_ret));
    }
#endif
    myiic_init();               /* IIC初始化 */  
    xl9555_init();              /* 初始化按键 */
    esp_err_t touch_ret = qspilcd_touch_init();   /* 触摸初始化 */
    if (touch_ret != ESP_OK)
    {
        ESP_LOGE("main", "Touch initialization failed: %s; continue without touch", esp_err_to_name(touch_ret));
    }
    ESP_ERROR_CHECK(qspilcd_init());        /* SPI LCD初始化 */

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_affinity = 1;
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));
    ESP_LOGI("main", "LVGL port initialized");

    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = qspilcd_get_io_handle(),
        .panel_handle = qspilcd_get_panel_handle(),
        .control_handle = NULL,
        .buffer_size = QSPILCD_H_RES * QSPILCD_DRAW_BUFF_HEIGHT,
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
            .full_refresh = false,
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
    setup_bottom_layer();
    init_scr_del_flag(&guider_ui);
    init_keyboard(&guider_ui);
    setup_scr_main_screen(&guider_ui);
    guider_ui.main_screen_del = false;
    lv_screen_load(guider_ui.main_screen);
    events_init(&guider_ui);
    custom_init(&guider_ui);
    ESP_LOGI("main", "GUI Guider UI created");
    lvgl_port_unlock();
    ESP_LOGI("main", "LVGL lock released");

#if 0                           /* 暂时禁用SD卡网络文件服务 */
    if (sd_ret == ESP_OK)
    {
        vTaskDelay(pdMS_TO_TICKS(300));      /* 等待首次界面刷新完成后再启动网络 */
        esp_err_t web_ret = sd_web_start(SD_MOUNT_POINT);
        if (web_ret != ESP_OK)
        {
            ESP_LOGE("main", "SD web initialization failed: %s; continue without SD web",
                     esp_err_to_name(web_ret));
        }
    }
#endif

    while (1)
    {
        LED0_TOGGLE();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
