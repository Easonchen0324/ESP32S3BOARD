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
#include <time.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "custom.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "wifi_manager.h"
#include "wifi_ota.h"

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void status_timer_cb(lv_timer_t *timer);
static void screen_swipe_event_cb(lv_event_t *event);
static void network_button_event_cb(lv_event_t *event);
static void back_button_event_cb(lv_event_t *event);
static void network_action_task(void *argument);
static void weather_update_task(void *argument);
static esp_err_t weather_fetch_code(int *weather_code);
static esp_err_t weather_http_event_handler(esp_http_client_event_t *event);
static const lv_image_dsc_t *weather_icon_from_code(int weather_code, const char **icon_name);

#define WEATHER_URL "https://api.open-meteo.com/v1/forecast?latitude=24.4798&longitude=118.0894&current=weather_code&timezone=Asia%2FShanghai"
#define WEATHER_RESPONSE_SIZE 512
#define WEATHER_RETRY_MS 60000
#define WEATHER_REFRESH_MS (30 * 60 * 1000)
#define SCREEN_SWITCH_ANIMATION_MS 300
#define NETWORK_ACTION_TASK_STACK_SIZE 6144

/** @brief 界面按钮对应的网络操作，枚举值会作为 FreeRTOS 任务参数传递。 */
typedef enum {
    UI_NETWORK_ACTION_AP = 1,
    UI_NETWORK_ACTION_BLE,
    UI_NETWORK_ACTION_SMARTCONFIG,
    UI_NETWORK_ACTION_OTA,
} ui_network_action_t;

/* 防止用户连续点击时重复创建多个网络启动任务。 */
static volatile bool s_network_action_running;

typedef struct {
    char data[WEATHER_RESPONSE_SIZE];
    size_t length;
    bool overflow;
} weather_response_t;

/**
 * Create a demo application
 */
void custom_init(lv_ui *ui)
{
    lv_timer_create(status_timer_cb, 1000, ui);

    setup_scr_screen_1(ui);
    ui->screen_1_del = false;

    lv_obj_add_flag(ui->main_screen, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ui->screen_1, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui->main_screen, screen_swipe_event_cb, LV_EVENT_GESTURE, ui);
    lv_obj_add_event_cb(ui->screen_1, screen_swipe_event_cb, LV_EVENT_GESTURE, ui);

    /* 四个网络按钮共用一个回调，通过当前控件判断要执行的功能。 */
    lv_obj_add_event_cb(ui->screen_1_btn_1, network_button_event_cb, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->screen_1_btn_2, network_button_event_cb, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->screen_1_btn_3, network_button_event_cb, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->screen_1_btn_4, network_button_event_cb, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->screen_1_btn_5, back_button_event_cb, LV_EVENT_CLICKED, ui);

    if (xTaskCreate(weather_update_task, "weather", 6144, ui, 3, NULL) != pdPASS)
    {
        ESP_LOGE("ui", "Unable to create weather update task");
    }
}

/**
 * @brief 网络功能按钮事件。
 *
 * AP 扫描等操作可能持续一段时间，因此这里只创建后台任务，不直接阻塞 LVGL 线程。
 */
static void network_button_event_cb(lv_event_t *event)
{
    lv_ui *ui = lv_event_get_user_data(event);
    lv_obj_t *button = lv_event_get_current_target(event);
    ui_network_action_t action;

    if (button == ui->screen_1_btn_1)
    {
        action = UI_NETWORK_ACTION_AP;
    }
    else if (button == ui->screen_1_btn_2)
    {
        action = UI_NETWORK_ACTION_BLE;
    }
    else if (button == ui->screen_1_btn_3)
    {
        action = UI_NETWORK_ACTION_SMARTCONFIG;
    }
    else if (button == ui->screen_1_btn_4)
    {
        action = UI_NETWORK_ACTION_OTA;
    }
    else
    {
        return;
    }

    if (s_network_action_running)
    {
        ESP_LOGW("ui", "Network action is already starting, ignore repeated click");
        return;
    }

    s_network_action_running = true;
    if (xTaskCreate(network_action_task, "ui_network", NETWORK_ACTION_TASK_STACK_SIZE,
                    (void *)(intptr_t)action, 4, NULL) != pdPASS)
    {
        s_network_action_running = false;
        ESP_LOGE("ui", "Unable to create network action task");
    }
}

/** @brief Back 按钮只负责返回主界面，不停止已经启动的网络服务。 */
static void back_button_event_cb(lv_event_t *event)
{
    lv_ui *ui = lv_event_get_user_data(event);
    lv_screen_load_anim(ui->main_screen, LV_SCREEN_LOAD_ANIM_MOVE_BOTTOM,
                        SCREEN_SWITCH_ANIMATION_MS, 0, false);
}

/** @brief 在后台执行具体网络功能，完成启动后输出结果并自动退出任务。 */
static void network_action_task(void *argument)
{
    ui_network_action_t action = (ui_network_action_t)(intptr_t)argument;
    const char *action_name = "unknown";
    esp_err_t ret = ESP_ERR_INVALID_ARG;

    switch (action)
    {
        case UI_NETWORK_ACTION_AP:
            action_name = "AP provisioning";
            ret = wifi_manager_start_ap_provisioning();
            break;

        case UI_NETWORK_ACTION_BLE:
            action_name = "BLE provisioning";
            ret = wifi_manager_start_ble_provisioning();
            break;

        case UI_NETWORK_ACTION_SMARTCONFIG:
            action_name = "SmartConfig provisioning";
            ret = wifi_manager_start_smartconfig_provisioning();
            break;

        case UI_NETWORK_ACTION_OTA:
            action_name = "Wi-Fi OTA";
            /* OTA 依赖默认事件循环；即使暂无 Wi-Fi，也先注册获取 IP 后的启动事件。 */
            ret = wifi_manager_init();
            if (ret == ESP_OK)
            {
                ret = wifi_ota_start();
            }
            break;

        default:
            break;
    }

    if (ret == ESP_OK)
    {
        ESP_LOGI("ui", "%s started", action_name);
    }
    else
    {
        ESP_LOGE("ui", "%s start failed: %s", action_name, esp_err_to_name(ret));
    }

    s_network_action_running = false;
    vTaskDelete(NULL);
}

static void screen_swipe_event_cb(lv_event_t *event)
{
    lv_ui *ui = lv_event_get_user_data(event);
    lv_obj_t *screen = lv_event_get_current_target(event);
    lv_indev_t *indev = lv_indev_active();

    if (indev == NULL)
    {
        return;
    }

    lv_dir_t direction = lv_indev_get_gesture_dir(indev);
    if (screen == ui->main_screen && direction == LV_DIR_TOP)
    {
        lv_indev_wait_release(indev);
        lv_screen_load_anim(ui->screen_1, LV_SCREEN_LOAD_ANIM_MOVE_TOP,
                            SCREEN_SWITCH_ANIMATION_MS, 0, false);
    }
    else if (screen == ui->screen_1 && direction == LV_DIR_BOTTOM)
    {
        lv_indev_wait_release(indev);
        lv_screen_load_anim(ui->main_screen, LV_SCREEN_LOAD_ANIM_MOVE_BOTTOM,
                            SCREEN_SWITCH_ANIMATION_MS, 0, false);
    }
}

static void status_timer_cb(lv_timer_t *timer)
{
    static bool time_colon_visible = true;
    lv_ui *ui = lv_timer_get_user_data(timer);

    if (!wifi_manager_has_time())
    {
        return;
    }

    time_t now;
    struct tm local_time;
    char time_text[6];
    char date_text[32];
    time(&now);
    localtime_r(&now, &local_time);
    strftime(time_text, sizeof(time_text), time_colon_visible ? "%H:%M" : "%H %M", &local_time);
    strftime(date_text, sizeof(date_text), "%a %b %d", &local_time);
    lv_label_set_text(ui->main_screen_time, time_text);
    lv_label_set_text(ui->main_screen_day, date_text);
    time_colon_visible = !time_colon_visible;
}

static void weather_update_task(void *argument)
{
    lv_ui *ui = argument;

    while (true)
    {
        int weather_code;
        esp_err_t ret = weather_fetch_code(&weather_code);
        if (ret == ESP_OK && lvgl_port_lock(0))
        {
            const char *icon_name;
            const lv_image_dsc_t *icon = weather_icon_from_code(weather_code, &icon_name);
            ESP_LOGI("ui", "Weather code=%d, icon=%s", weather_code, icon_name);
            lv_image_set_src(ui->main_screen_weather, icon);
            ESP_LOGI("ui", "Weather icon updated: %s", icon_name);
            lvgl_port_unlock();
        }
        else if (ret != ESP_OK)
        {
            ESP_LOGW("ui", "Weather update failed: %s", esp_err_to_name(ret));
        }

        vTaskDelay(pdMS_TO_TICKS(ret == ESP_OK ? WEATHER_REFRESH_MS : WEATHER_RETRY_MS));
    }
}

static esp_err_t weather_fetch_code(int *weather_code)
{
    weather_response_t response = {0};
    esp_http_client_config_t config = {
        .url = WEATHER_URL,
        .event_handler = weather_http_event_handler,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (ret != ESP_OK)
    {
        return ret;
    }
    if (status_code != 200 || response.overflow)
    {
        return ESP_FAIL;
    }

    response.data[response.length] = '\0';
    cJSON *root = cJSON_Parse(response.data);
    cJSON *current = root == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(root, "current");
    cJSON *code = current == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(current, "weather_code");
    if (!cJSON_IsNumber(code))
    {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    *weather_code = code->valueint;
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t weather_http_event_handler(esp_http_client_event_t *event)
{
    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0)
    {
        return ESP_OK;
    }

    weather_response_t *response = event->user_data;
    if (response->length + event->data_len >= sizeof(response->data))
    {
        response->overflow = true;
        return ESP_OK;
    }

    memcpy(response->data + response->length, event->data, event->data_len);
    response->length += event->data_len;
    return ESP_OK;
}

static const lv_image_dsc_t *weather_icon_from_code(int weather_code, const char **icon_name)
{
    *icon_name = "weather_cloudy";
    if (weather_code == 0)
    {
        *icon_name = "weather_sunny";
        return &weather_sunny;
    }
    if (weather_code == 1 || weather_code == 2)
    {
        *icon_name = "weather_cloudy";
        return &weather_cloudy;
    }
    if (weather_code == 3)
    {
        *icon_name = "weather_overcast";
        return &weather_overcast;
    }
    if (weather_code == 45 || weather_code == 48)
    {
        *icon_name = "weather_fog";
        return &weather_fog;
    }
    if ((weather_code >= 71 && weather_code <= 77) || weather_code == 85 || weather_code == 86)
    {
        *icon_name = "weather_snow";
        return &weather_snow;
    }
    if ((weather_code >= 51 && weather_code <= 67) ||
        (weather_code >= 80 && weather_code <= 82) ||
        (weather_code >= 95 && weather_code <= 99))
    {
        *icon_name = "weather_thunderstorm";
        return &weather_thunderstorm;
    }
    return &weather_cloudy;
}
