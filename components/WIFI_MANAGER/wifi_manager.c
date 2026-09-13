#include "wifi_manager.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "apps/esp_sntp.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#define WIFI_MANAGER_VALID_TIME_EPOCH 1704067200

static const char *TAG = "wifi_manager";

static bool s_initialized;
static bool s_wifi_started;
static bool s_connecting;
static bool s_connected;

static void start_sntp(void)
{
    if (esp_sntp_enabled()) {
        return;
    }

    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started");
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        (void)esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        s_connecting = true;
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting");
        (void)esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_connecting = false;
        s_connected = true;
        start_sntp();
        ESP_LOGI(TAG, "Wi-Fi connected");
    }
}

esp_err_t wifi_manager_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_FAIL;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "initialize Wi-Fi failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set Wi-Fi storage failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL), TAG,
                        "register Wi-Fi event failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL), TAG,
                        "register IP event failed");

    s_initialized = true;
    return ESP_OK;
}

esp_err_t wifi_manager_start_provisioning(void)
{
    if (CONFIG_SD_WEB_WIFI_SSID[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(wifi_manager_init(), TAG, "initialize Wi-Fi manager failed");
    if (s_connected || s_connecting) {
        return ESP_OK;
    }

    wifi_config_t sta_config = {0};
    strlcpy((char *)sta_config.sta.ssid, CONFIG_SD_WEB_WIFI_SSID, sizeof(sta_config.sta.ssid));
    strlcpy((char *)sta_config.sta.password, CONFIG_SD_WEB_WIFI_PASSWORD, sizeof(sta_config.sta.password));
    sta_config.sta.threshold.authmode = CONFIG_SD_WEB_WIFI_PASSWORD[0] == '\0'
                                             ? WIFI_AUTH_OPEN
                                             : WIFI_AUTH_WPA2_PSK;

    s_connecting = true;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set STA mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_config), TAG, "set STA configuration failed");
    if (!s_wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start Wi-Fi station failed");
        s_wifi_started = true;
    } else {
        ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "connect to router failed");
    }

    ESP_LOGI(TAG, "Connecting to configured Wi-Fi");
    return ESP_OK;
}

bool wifi_manager_is_provisioning(void)
{
    return s_connecting;
}

bool wifi_manager_has_time(void)
{
    time_t now;
    time(&now);
    return now >= WIFI_MANAGER_VALID_TIME_EPOCH;
}
