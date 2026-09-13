/*
 * Copyright 2024 NXP
 * NXP Proprietary. This software is owned or controlled by NXP and may only be used strictly in
 * accordance with the applicable license terms. By expressly accepting such terms or by downloading, installing,
 * activating and/or otherwise using the software, you are agreeing that you have read, and that you agree to
 * comply with and are bound by, such license terms.  If you do not agree to be bound by the applicable license
 * terms, then you may not retain, install, activate or otherwise use the software.
 */

/*********************
 *      INCLUDES
 *********************/
#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "custom.h"
#include "esp_log.h"
#include "wifi_manager.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void main_screen_wifi_event_cb(lv_event_t *event);
static void status_timer_cb(lv_timer_t *timer);
static void wifi_provision_task(void *argument);
static void main_screen_gesture_event_cb(lv_event_t *event);
static void screen_1_gesture_event_cb(lv_event_t *event);

/**********************
 *  STATIC VARIABLES
 **********************/
static TaskHandle_t s_wifi_task;

/**
 * Create a demo application
 */
void custom_init(lv_ui *ui)
{
    setup_scr_screen_1(ui);
    ui->screen_1_del = false;

    lv_obj_add_flag(ui->main_screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ui->screen_1, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ui->main_screen_wifi, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(ui->screen_1_slider_1, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(ui->main_screen, main_screen_gesture_event_cb, LV_EVENT_GESTURE, ui);
    lv_obj_add_event_cb(ui->screen_1, screen_1_gesture_event_cb, LV_EVENT_GESTURE, ui);
    lv_obj_add_event_cb(ui->main_screen_wifi, main_screen_wifi_event_cb, LV_EVENT_CLICKED, ui);
    lv_timer_create(status_timer_cb, 250, ui);
}

static void main_screen_gesture_event_cb(lv_event_t *event)
{
    if (lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_TOP)
    {
        return;
    }

    lv_ui *ui = lv_event_get_user_data(event);
    lv_screen_load_anim(ui->screen_1, LV_SCR_LOAD_ANIM_MOVE_TOP, 300, 0, false);
}

static void screen_1_gesture_event_cb(lv_event_t *event)
{
    if (lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_TOP)
    {
        return;
    }

    lv_ui *ui = lv_event_get_user_data(event);
    lv_screen_load_anim(ui->main_screen, LV_SCR_LOAD_ANIM_MOVE_TOP, 300, 0, false);
}

static void main_screen_wifi_event_cb(lv_event_t *event)
{
    if (s_wifi_task == NULL &&
        xTaskCreatePinnedToCore(wifi_provision_task, "wifi_provision", 4096, NULL, 3,
                                &s_wifi_task, 1) != pdPASS)
    {
        ESP_LOGE("ui", "Unable to create Wi-Fi provisioning task");
    }
}

static void wifi_provision_task(void *argument)
{
    esp_err_t ret = wifi_manager_start_provisioning();
    if (ret != ESP_OK)
    {
        ESP_LOGE("ui", "Unable to start Wi-Fi provisioning: %s", esp_err_to_name(ret));
    }
    s_wifi_task = NULL;
    vTaskDelete(NULL);
}

static void status_timer_cb(lv_timer_t *timer)
{
    static bool icon_visible = true;
    static bool time_colon_visible = true;
    static uint32_t elapsed_ms;
    lv_ui *ui = lv_timer_get_user_data(timer);

    if (wifi_manager_is_provisioning())
    {
        icon_visible = !icon_visible;
        lv_obj_set_style_image_opa(ui->main_screen_wifi,
                                   icon_visible ? LV_OPA_COVER : LV_OPA_40,
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
    }
    else
    {
        icon_visible = true;
        lv_obj_set_style_image_opa(ui->main_screen_wifi, LV_OPA_COVER,
                                   LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    elapsed_ms += 250;
    if (elapsed_ms < 1000 || !wifi_manager_has_time())
    {
        return;
    }
    elapsed_ms = 0;

    time_t now;
    struct tm local_time;
    char time_text[6];
    time(&now);
    localtime_r(&now, &local_time);
    strftime(time_text, sizeof(time_text), time_colon_visible ? "%H:%M" : "%H %M", &local_time);
    lv_label_set_text(ui->main_screen_time, time_text);
    time_colon_visible = !time_colon_visible;
}
