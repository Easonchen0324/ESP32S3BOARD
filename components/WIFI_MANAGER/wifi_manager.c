#include "wifi_manager.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "apps/esp_sntp.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_smartconfig.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"

#define WIFI_MANAGER_AP_SSID             "ESP_AP"
#define WIFI_MANAGER_NVS_NAMESPACE       "wifi_config"
#define WIFI_MANAGER_NVS_KEY_SSID        "ssid"
#define WIFI_MANAGER_NVS_KEY_PASSWORD    "password"
#define WIFI_MANAGER_VALID_TIME_EPOCH    1704067200
#define WIFI_MANAGER_FORM_MAX_LEN        160
#define WIFI_MANAGER_SSID_MAX_LEN        32
#define WIFI_MANAGER_PASSWORD_MAX_LEN    64
#define WIFI_MANAGER_MAX_SCAN_RESULTS     12
#define WIFI_MANAGER_DNS_PORT             53
#define WIFI_MANAGER_DNS_PACKET_MAX_LEN   256
#define WIFI_MANAGER_BLE_NAME_PREFIX      "PROV_"
#define WIFI_MANAGER_BLE_POP              "esp32s3"

typedef enum {
    WIFI_PROVISIONING_NONE,
    WIFI_PROVISIONING_AP,
    WIFI_PROVISIONING_BLE,
    WIFI_PROVISIONING_SMARTCONFIG,
} wifi_provisioning_mode_t;

typedef struct {
    char ssid[WIFI_MANAGER_SSID_MAX_LEN + 1];
    char password[WIFI_MANAGER_PASSWORD_MAX_LEN + 1];
} wifi_credentials_t;

static const char *TAG = "wifi_manager";

static bool s_initialized;
static bool s_wifi_started;
static bool s_connecting;
static bool s_connected;
static bool s_provisioning;
static bool s_sta_should_connect;
static bool s_smartconfig_started;
static bool s_ble_manager_initialized;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static httpd_handle_t s_http_server;
static TaskHandle_t s_apply_task;
static TaskHandle_t s_dns_task;
static TaskHandle_t s_sntp_monitor_task;
static int s_dns_socket = -1;
static wifi_credentials_t s_pending_credentials;
static wifi_ap_record_t s_scan_results[WIFI_MANAGER_MAX_SCAN_RESULTS];
static uint16_t s_scan_result_count;
static wifi_provisioning_mode_t s_provisioning_mode;

/* 链接器会把独立 HTML 文件嵌入固件，网页与 C 状态机可以分开维护。 */
extern const uint8_t ap_provision_html_start[] asm("_binary_ap_provision_html_start");
extern const uint8_t ap_provision_html_end[] asm("_binary_ap_provision_html_end");

static void start_sntp(void);
static void time_sync_notification_cb(struct timeval *timeval);
static void sntp_monitor_task(void *argument);
static esp_err_t start_http_server(void);
static esp_err_t start_station(const wifi_credentials_t *credentials);
static esp_err_t save_credentials(const wifi_credentials_t *credentials);
static esp_err_t load_credentials(wifi_credentials_t *credentials);
static void credentials_to_sta_config(const wifi_credentials_t *credentials, wifi_config_t *config);
static void apply_credentials_task(void *argument);
static void scan_available_networks(void);
static esp_err_t start_dns_server(void);
static void stop_dns_server(void);
static void dns_server_task(void *argument);
static void smartconfig_event_handler(void *arg, esp_event_base_t event_base,
                                      int32_t event_id, void *event_data);
static void ble_provisioning_event_handler(void *arg, esp_event_base_t event_base,
                                           int32_t event_id, void *event_data);
static esp_err_t start_smartconfig_service(void);
static void stop_ap_services(void);

static void start_sntp(void)
{
    if (esp_sntp_enabled()) {
        return;
    }

    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_init();
    if (xTaskCreatePinnedToCore(sntp_monitor_task, "sntp_monitor", 3072, NULL, 3,
                                &s_sntp_monitor_task, 1) != pdPASS) {
        s_sntp_monitor_task = NULL;
        ESP_LOGE(TAG, "Create SNTP monitor task failed");
    }
    ESP_LOGI(TAG, "SNTP started");
}

static void time_sync_notification_cb(struct timeval *timeval)
{
    time_t now = timeval->tv_sec;
    struct tm local_time;
    char time_text[20];
    localtime_r(&now, &local_time);
    strftime(time_text, sizeof(time_text), "%Y-%m-%d %H:%M:%S", &local_time);
    ESP_LOGI(TAG, "Time synchronized: %s", time_text);
}

static void sntp_monitor_task(void *argument)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        for (int elapsed_seconds = 0; elapsed_seconds < 15; elapsed_seconds++) {
            if (wifi_manager_has_time()) {
                s_sntp_monitor_task = NULL;
                vTaskDelete(NULL);
                return;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

        if (attempt == 0) {
            ESP_LOGW(TAG, "SNTP did not synchronize; retrying with pool.ntp.org");
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_restart();
        }
    }

    ESP_LOGE(TAG, "SNTP synchronization failed after both servers");
    s_sntp_monitor_task = NULL;
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        /* BLE manager 也可能启动 Wi-Fi，同步本地标志可避免后续重复调用 esp_wifi_start。 */
        s_wifi_started = true;
        if (s_provisioning_mode == WIFI_PROVISIONING_SMARTCONFIG && !s_smartconfig_started) {
            esp_err_t ret = start_smartconfig_service();
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Start SmartConfig failed: %s", esp_err_to_name(ret));
            }
        } else if (s_sta_should_connect) {
            (void)esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_sta_should_connect) {
            s_connecting = true;
            ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting");
            (void)esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_connecting = false;
        s_connected = true;
        s_provisioning = false;
        s_sta_should_connect = true;
        start_sntp();
        ESP_LOGI(TAG, "Wi-Fi connected");
    }
}

static void smartconfig_event_handler(void *arg, esp_event_base_t event_base,
                                      int32_t event_id, void *event_data)
{
    if (event_base != SC_EVENT || s_provisioning_mode != WIFI_PROVISIONING_SMARTCONFIG) {
        return;
    }

    if (event_id == SC_EVENT_SCAN_DONE) {
        ESP_LOGI(TAG, "SmartConfig scan completed");
    } else if (event_id == SC_EVENT_FOUND_CHANNEL) {
        ESP_LOGI(TAG, "SmartConfig channel found");
    } else if (event_id == SC_EVENT_GOT_SSID_PSWD) {
        const smartconfig_event_got_ssid_pswd_t *event = event_data;
        wifi_credentials_t credentials = {0};

        /* SmartConfig 字段长度是固定数组，显式补零后再保存，防止越界读取。 */
        memcpy(credentials.ssid, event->ssid, WIFI_MANAGER_SSID_MAX_LEN);
        memcpy(credentials.password, event->password, WIFI_MANAGER_PASSWORD_MAX_LEN);
        esp_err_t ret = save_credentials(&credentials);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Save SmartConfig credentials failed: %s", esp_err_to_name(ret));
            return;
        }

        wifi_config_t sta_config;
        credentials_to_sta_config(&credentials, &sta_config);
        sta_config.sta.bssid_set = event->bssid_set;
        if (event->bssid_set) {
            memcpy(sta_config.sta.bssid, event->bssid, sizeof(sta_config.sta.bssid));
        }

        s_sta_should_connect = true;
        s_connecting = true;
        (void)esp_wifi_disconnect();
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
        ESP_LOGI(TAG, "SmartConfig credentials received; connecting to %s", credentials.ssid);
    } else if (event_id == SC_EVENT_SEND_ACK_DONE) {
        /* 手机已收到 ESP32 的确认包，此时才停止协议可避免手机端误判超时。 */
        (void)esp_smartconfig_stop();
        s_smartconfig_started = false;
        s_provisioning = false;
        s_provisioning_mode = WIFI_PROVISIONING_NONE;
        ESP_LOGI(TAG, "SmartConfig completed");
    }
}

static void ble_provisioning_event_handler(void *arg, esp_event_base_t event_base,
                                           int32_t event_id, void *event_data)
{
    if (event_base != WIFI_PROV_EVENT) {
        return;
    }

    switch (event_id) {
    case WIFI_PROV_START:
        ESP_LOGI(TAG, "BLE provisioning started, PoP: %s", WIFI_MANAGER_BLE_POP);
        break;
    case WIFI_PROV_CRED_RECV: {
        const wifi_sta_config_t *sta = event_data;
        wifi_credentials_t credentials = {0};
        memcpy(credentials.ssid, sta->ssid, WIFI_MANAGER_SSID_MAX_LEN);
        memcpy(credentials.password, sta->password, WIFI_MANAGER_PASSWORD_MAX_LEN);

        /* 先暂存，等官方 manager 验证连接成功后再写 NVS，避免保存错误密码。 */
        s_pending_credentials = credentials;
        ESP_LOGI(TAG, "BLE credentials received for %s", credentials.ssid);
        break;
    }
    case WIFI_PROV_CRED_FAIL:
        s_connected = false;
        ESP_LOGW(TAG, "BLE provisioning credentials rejected; waiting for retry");
        break;
    case WIFI_PROV_CRED_SUCCESS:
        /* 官方 BLE manager 会暂时使用自身的 Wi-Fi NVS，这里同步到工程统一的命名空间。 */
        if (save_credentials(&s_pending_credentials) != ESP_OK) {
            ESP_LOGE(TAG, "Save verified BLE credentials failed");
        }
        s_sta_should_connect = true;
        ESP_LOGI(TAG, "BLE provisioning succeeded");
        break;
    case WIFI_PROV_END:
        /* Manager 自动停止后必须 deinit，释放 GATT、Protocomm 和配网状态机资源。 */
        if (s_ble_manager_initialized) {
            s_ble_manager_initialized = false;
            wifi_prov_mgr_deinit();
        }
        /* BLE manager 临时切到 FLASH 存储，结束后恢复本工程统一使用的 RAM 配置。 */
        (void)esp_wifi_set_storage(WIFI_STORAGE_RAM);
        s_provisioning = false;
        s_provisioning_mode = WIFI_PROVISIONING_NONE;
        ESP_LOGI(TAG, "BLE provisioning stopped");
        break;
    default:
        break;
    }
}

static esp_err_t save_credentials(const wifi_credentials_t *credentials)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(WIFI_MANAGER_NVS_NAMESPACE, NVS_READWRITE, &handle), TAG,
                        "open Wi-Fi NVS failed");

    esp_err_t ret = nvs_set_str(handle, WIFI_MANAGER_NVS_KEY_SSID, credentials->ssid);
    if (ret == ESP_OK) {
        ret = nvs_set_str(handle, WIFI_MANAGER_NVS_KEY_PASSWORD, credentials->password);
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(handle);
    }
    nvs_close(handle);
    return ret;
}

static esp_err_t load_credentials(wifi_credentials_t *credentials)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(WIFI_MANAGER_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (ret != ESP_OK) {
        return ret;
    }

    size_t ssid_len = sizeof(credentials->ssid);
    size_t password_len = sizeof(credentials->password);
    ret = nvs_get_str(handle, WIFI_MANAGER_NVS_KEY_SSID, credentials->ssid, &ssid_len);
    if (ret == ESP_OK) {
        ret = nvs_get_str(handle, WIFI_MANAGER_NVS_KEY_PASSWORD, credentials->password, &password_len);
    }
    nvs_close(handle);

    if (ret == ESP_OK && credentials->ssid[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    return ret;
}

static void credentials_to_sta_config(const wifi_credentials_t *credentials, wifi_config_t *config)
{
    memset(config, 0, sizeof(*config));
    memcpy(config->sta.ssid, credentials->ssid, strnlen(credentials->ssid, sizeof(config->sta.ssid)));
    memcpy(config->sta.password, credentials->password,
           strnlen(credentials->password, sizeof(config->sta.password)));
    config->sta.threshold.authmode = credentials->password[0] == '\0'
                                       ? WIFI_AUTH_OPEN
                                       : WIFI_AUTH_WPA2_PSK;
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
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) {
        return ESP_FAIL;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_config), TAG, "initialize Wi-Fi failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set Wi-Fi storage failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL), TAG,
                        "register Wi-Fi event failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL), TAG,
                        "register IP event failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, smartconfig_event_handler, NULL), TAG,
                        "register SmartConfig event failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID,
                                                   ble_provisioning_event_handler, NULL), TAG,
                        "register BLE provisioning event failed");

    s_initialized = true;
    return ESP_OK;
}

static esp_err_t start_station(const wifi_credentials_t *credentials)
{
    wifi_config_t sta_config;
    credentials_to_sta_config(credentials, &sta_config);

    s_connected = false;
    s_connecting = true;
    s_sta_should_connect = true;
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "set Wi-Fi RAM storage failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set STA mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_config), TAG, "set STA configuration failed");
    if (!s_wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start Wi-Fi station failed");
        s_wifi_started = true;
    } else {
        ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "connect to router failed");
    }

    ESP_LOGI(TAG, "Connecting to saved Wi-Fi");
    return ESP_OK;
}

static int hex_to_int(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static bool url_decode(const char *source, char *destination, size_t destination_size)
{
    size_t written = 0;
    while (*source != '\0') {
        char value = *source++;
        if (value == '+') {
            value = ' ';
        } else if (value == '%') {
            if (source[0] == '\0' || source[1] == '\0') {
                return false;
            }
            int high = hex_to_int(source[0]);
            int low = hex_to_int(source[1]);
            if (high < 0 || low < 0) {
                return false;
            }
            value = (char)((high << 4) | low);
            source += 2;
        }
        if (written + 1 >= destination_size) {
            return false;
        }
        destination[written++] = value;
    }
    destination[written] = '\0';
    return true;
}

static bool form_get_value(char *form, const char *key, char *value, size_t value_size)
{
    char *save_pointer = NULL;
    for (char *field = strtok_r(form, "&", &save_pointer); field != NULL;
         field = strtok_r(NULL, "&", &save_pointer)) {
        char *separator = strchr(field, '=');
        if (separator == NULL) {
            continue;
        }
        *separator = '\0';
        if (strcmp(field, key) == 0) {
            return url_decode(separator + 1, value, value_size);
        }
    }
    return false;
}

static void scan_available_networks(void)
{
    s_scan_result_count = 0;
    esp_err_t ret = esp_wifi_scan_start(NULL, true);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan failed: %s", esp_err_to_name(ret));
        return;
    }

    uint16_t result_count = WIFI_MANAGER_MAX_SCAN_RESULTS;
    ret = esp_wifi_scan_get_ap_records(&result_count, s_scan_results);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Read Wi-Fi scan results failed: %s", esp_err_to_name(ret));
        return;
    }
    s_scan_result_count = result_count;
}

static size_t json_escape_ssid(const uint8_t *ssid, char *escaped, size_t escaped_size)
{
    size_t written = 0;
    for (size_t index = 0; index < WIFI_MANAGER_SSID_MAX_LEN && ssid[index] != '\0'; index++) {
        unsigned char value = ssid[index];
        const char *replacement = NULL;
        char unicode_escape[7];
        if (value == '"') {
            replacement = "\\\"";
        } else if (value == '\\') {
            replacement = "\\\\";
        } else if (value < 0x20) {
            snprintf(unicode_escape, sizeof(unicode_escape), "\\u%04x", value);
            replacement = unicode_escape;
        }

        if (replacement != NULL) {
            size_t length = strlen(replacement);
            if (written + length >= escaped_size) {
                break;
            }
            memcpy(escaped + written, replacement, length);
            written += length;
        } else if (written + 1 < escaped_size) {
            escaped[written++] = (char)value;
        }
    }
    escaped[written] = '\0';
    return written;
}

static esp_err_t root_get_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, (const char *)ap_provision_html_start,
                           ap_provision_html_end - ap_provision_html_start);
}

static esp_err_t scan_get_handler(httpd_req_t *request)
{
    scan_available_networks();
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, "{\"networks\":[", HTTPD_RESP_USE_STRLEN),
                        TAG, "send scan response failed");

    bool first = true;
    for (uint16_t index = 0; index < s_scan_result_count; index++) {
        char escaped_ssid[WIFI_MANAGER_SSID_MAX_LEN * 6 + 1];
        char item[sizeof(escaped_ssid) + 64];
        json_escape_ssid(s_scan_results[index].ssid, escaped_ssid, sizeof(escaped_ssid));
        if (escaped_ssid[0] == '\0') {
            continue;
        }
        snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
                 first ? "" : ",", escaped_ssid, s_scan_results[index].rssi,
                 s_scan_results[index].authmode == WIFI_AUTH_OPEN ? "false" : "true");
        first = false;
        ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, item, HTTPD_RESP_USE_STRLEN), TAG,
                            "send scan item failed");
    }
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, "]}", HTTPD_RESP_USE_STRLEN), TAG,
                        "send scan response ending failed");

    /*
     * 分块响应必须再发送一个长度为 0 的结束块。
     * 如果遗漏，浏览器会一直等待响应结束，网页就会持续显示“正在扫描”。
     */
    return httpd_resp_send_chunk(request, NULL, 0);
}

static esp_err_t status_get_handler(httpd_req_t *request)
{
    esp_netif_ip_info_t ip_info = {0};
    const char *mode = s_provisioning_mode == WIFI_PROVISIONING_AP ? "ap" :
                       s_provisioning_mode == WIFI_PROVISIONING_BLE ? "ble" :
                       s_provisioning_mode == WIFI_PROVISIONING_SMARTCONFIG ? "smartconfig" : "idle";
    if (s_sta_netif != NULL) {
        (void)esp_netif_get_ip_info(s_sta_netif, &ip_info);
    }

    char response[192];
    snprintf(response, sizeof(response),
             "{\"mode\":\"%s\",\"provisioning\":%s,\"connecting\":%s,\"connected\":%s,"
             "\"ip\":\"" IPSTR "\"}", mode, s_provisioning ? "true" : "false",
             s_connecting ? "true" : "false", s_connected ? "true" : "false",
             IP2STR(&ip_info.ip));
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, response);
}

static esp_err_t send_json_message(httpd_req_t *request, const char *status,
                                   const char *message, bool success)
{
    char response[160];
    snprintf(response, sizeof(response), "{\"ok\":%s,\"message\":\"%s\"}",
             success ? "true" : "false", message);
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    return httpd_resp_sendstr(request, response);
}

static esp_err_t configure_post_handler(httpd_req_t *request)
{
    if (request->content_len == 0 || request->content_len > WIFI_MANAGER_FORM_MAX_LEN) {
        return send_json_message(request, "400 Bad Request", "表单数据无效", false);
    }

    char form[WIFI_MANAGER_FORM_MAX_LEN + 1] = {0};
    int received = 0;
    while (received < request->content_len) {
        int ret = httpd_req_recv(request, form + received, request->content_len - received);
        if (ret <= 0) {
            return send_json_message(request, "400 Bad Request", "接收表单失败", false);
        }
        received += ret;
    }

    char password_form[WIFI_MANAGER_FORM_MAX_LEN + 1];
    memcpy(password_form, form, sizeof(password_form));
    wifi_credentials_t credentials = {0};
    if (!form_get_value(form, "ssid", credentials.ssid, sizeof(credentials.ssid)) ||
        credentials.ssid[0] == '\0' ||
        !form_get_value(password_form, "password", credentials.password, sizeof(credentials.password))) {
        return send_json_message(request, "400 Bad Request", "Wi-Fi 名称或密码无效", false);
    }
    if (s_apply_task != NULL) {
        return send_json_message(request, "409 Conflict", "正在应用上一份配置", false);
    }

    esp_err_t ret = save_credentials(&credentials);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Save Wi-Fi credentials failed: %s", esp_err_to_name(ret));
        return send_json_message(request, "500 Internal Server Error", "保存配置失败", false);
    }
    s_pending_credentials = credentials;
    if (xTaskCreatePinnedToCore(apply_credentials_task, "wifi_apply", 4096, NULL, 3,
                                &s_apply_task, 1) != pdPASS) {
        s_apply_task = NULL;
        return send_json_message(request, "500 Internal Server Error", "创建连接任务失败", false);
    }

    return send_json_message(request, "200 OK", "配置已保存，正在连接路由器", true);
}

static esp_err_t start_http_server(void)
{
    if (s_http_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;
    ESP_RETURN_ON_ERROR(httpd_start(&s_http_server, &config), TAG, "start provisioning server failed");

    /* 通配首页必须最后注册，否则它会先匹配并吞掉 /api 下的 GET 请求。 */
    const httpd_uri_t handlers[] = {
        {.uri = "/api/scan", .method = HTTP_GET, .handler = scan_get_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler},
        {.uri = "/api/configure", .method = HTTP_POST, .handler = configure_post_handler},
        {.uri = "/configure", .method = HTTP_POST, .handler = configure_post_handler},
        {.uri = "/*", .method = HTTP_GET, .handler = root_get_handler},
    };

    esp_err_t ret = ESP_OK;
    for (size_t index = 0; index < sizeof(handlers) / sizeof(handlers[0]); index++) {
        ret = httpd_register_uri_handler(s_http_server, &handlers[index]);
        if (ret != ESP_OK) {
            break;
        }
    }
    if (ret != ESP_OK) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }
    return ret;
}

static void dns_server_task(void *argument)
{
    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0) {
        ESP_LOGE(TAG, "Create captive DNS socket failed");
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(WIFI_MANAGER_DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    struct timeval timeout = {
        .tv_sec = 0,
        .tv_usec = 500000,
    };
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        ESP_LOGE(TAG, "Bind captive DNS socket failed");
        close(socket_fd);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    s_dns_socket = socket_fd;
    while (s_provisioning && s_dns_socket == socket_fd) {
        uint8_t query[WIFI_MANAGER_DNS_PACKET_MAX_LEN];
        struct sockaddr_in client_address;
        socklen_t client_address_len = sizeof(client_address);
        int query_len = recvfrom(socket_fd, query, sizeof(query), 0,
                                 (struct sockaddr *)&client_address, &client_address_len);
        if (query_len < 12) {
            continue;
        }

        uint8_t response[WIFI_MANAGER_DNS_PACKET_MAX_LEN];
        if (query_len + 16 > sizeof(response)) {
            continue;
        }
        memcpy(response, query, query_len);
        response[2] = (query[2] & 0x79U) | 0x80U;
        response[3] = 0x80U;
        response[6] = 0;
        response[7] = 1;
        response[8] = 0;
        response[9] = 0;
        response[10] = 0;
        response[11] = 0;

        uint8_t *answer = response + query_len;
        const uint8_t ap_address[] = {192, 168, 4, 1};
        const uint8_t answer_header[] = {
            0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x3c, 0x00, 0x04,
        };
        memcpy(answer, answer_header, sizeof(answer_header));
        memcpy(answer + sizeof(answer_header), ap_address, sizeof(ap_address));
        (void)sendto(socket_fd, response, query_len + 16, 0,
                     (struct sockaddr *)&client_address, client_address_len);
    }

    if (s_dns_socket == socket_fd) {
        s_dns_socket = -1;
        close(socket_fd);
    }
    /* 若 stop_dns_server 已关闭套接字，这里不能再次 close，避免误关复用后的文件描述符。 */
    s_dns_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t start_dns_server(void)
{
    if (s_dns_task != NULL) {
        return ESP_OK;
    }
    if (xTaskCreatePinnedToCore(dns_server_task, "captive_dns", 4096, NULL, 3,
                                &s_dns_task, 1) != pdPASS) {
        s_dns_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void stop_dns_server(void)
{
    if (s_dns_socket >= 0) {
        shutdown(s_dns_socket, SHUT_RDWR);
        close(s_dns_socket);
        s_dns_socket = -1;
    }
}

static void stop_ap_services(void)
{
    stop_dns_server();
    if (s_http_server != NULL) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }
}

static void apply_credentials_task(void *argument)
{
    vTaskDelay(pdMS_TO_TICKS(300));
    stop_ap_services();
    s_provisioning = false;
    s_provisioning_mode = WIFI_PROVISIONING_NONE;

    esp_err_t ret = start_station(&s_pending_credentials);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Apply Wi-Fi credentials failed: %s", esp_err_to_name(ret));
    }
    s_apply_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t wifi_manager_start_ap_provisioning(void)
{
    ESP_RETURN_ON_ERROR(wifi_manager_init(), TAG, "initialize Wi-Fi manager failed");
    if (s_provisioning) {
        return s_provisioning_mode == WIFI_PROVISIONING_AP ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) {
            return ESP_FAIL;
        }
    }

    wifi_config_t ap_config = {0};
    memcpy(ap_config.ap.ssid, WIFI_MANAGER_AP_SSID, strlen(WIFI_MANAGER_AP_SSID));
    ap_config.ap.ssid_len = strlen(WIFI_MANAGER_AP_SSID);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    s_provisioning = true;
    s_provisioning_mode = WIFI_PROVISIONING_AP;
    s_sta_should_connect = false;
    s_connecting = false;
    (void)esp_wifi_disconnect();
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set AP mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG, "set AP configuration failed");
    if (!s_wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start AP failed");
        s_wifi_started = true;
    }
    scan_available_networks();
    ESP_RETURN_ON_ERROR(start_http_server(), TAG, "start provisioning page failed");
    ESP_RETURN_ON_ERROR(start_dns_server(), TAG, "start captive DNS failed");

    ESP_LOGI(TAG, "Provisioning AP ready: %s, captive portal at http://192.168.4.1", WIFI_MANAGER_AP_SSID);
    return ESP_OK;
}

esp_err_t wifi_manager_start_provisioning(void)
{
    /* 保留旧接口，已有业务代码无需修改即可继续启动 AP 配网。 */
    return wifi_manager_start_ap_provisioning();
}

esp_err_t wifi_manager_start_ble_provisioning(void)
{
    ESP_RETURN_ON_ERROR(wifi_manager_init(), TAG, "initialize Wi-Fi manager failed");
    if (s_provisioning || s_ble_manager_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_ble,
        /* 不永久释放蓝牙内存，后续业务仍可再次进入 BLE 配网。 */
        .scheme_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
        .app_event_handler = WIFI_PROV_EVENT_HANDLER_NONE,
    };
    ESP_RETURN_ON_ERROR(wifi_prov_mgr_init(config), TAG, "initialize BLE provisioning failed");
    s_ble_manager_initialized = true;

    uint8_t mac[6];
    char service_name[12];
    esp_err_t ret = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (ret != ESP_OK) {
        wifi_prov_mgr_deinit();
        s_ble_manager_initialized = false;
        return ret;
    }
    snprintf(service_name, sizeof(service_name), WIFI_MANAGER_BLE_NAME_PREFIX "%02X%02X%02X",
             mac[3], mac[4], mac[5]);

    s_provisioning = true;
    s_provisioning_mode = WIFI_PROVISIONING_BLE;
    s_sta_should_connect = false;
    s_connected = false;
    s_connecting = false;
    (void)esp_wifi_disconnect();

    /* Security 1 使用 X25519 + AES-CTR，手机端需要输入下方 PoP。 */
    ret = wifi_prov_mgr_start_provisioning(WIFI_PROV_SECURITY_1,
                                           WIFI_MANAGER_BLE_POP,
                                           service_name, NULL);
    if (ret != ESP_OK) {
        wifi_prov_mgr_deinit();
        s_ble_manager_initialized = false;
        s_provisioning = false;
        s_provisioning_mode = WIFI_PROVISIONING_NONE;
        return ret;
    }

    ESP_LOGI(TAG, "BLE provisioning ready: name=%s, PoP=%s", service_name, WIFI_MANAGER_BLE_POP);
    return ESP_OK;
}

static esp_err_t start_smartconfig_service(void)
{
    if (s_smartconfig_started) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_smartconfig_set_type(SC_TYPE_ESPTOUCH_AIRKISS), TAG,
                        "set SmartConfig protocol failed");
    smartconfig_start_config_t config = SMARTCONFIG_START_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_smartconfig_start(&config), TAG, "start SmartConfig failed");
    s_smartconfig_started = true;
    ESP_LOGI(TAG, "SmartConfig is waiting for ESPTouch/AirKiss data");
    return ESP_OK;
}

esp_err_t wifi_manager_start_smartconfig_provisioning(void)
{
    ESP_RETURN_ON_ERROR(wifi_manager_init(), TAG, "initialize Wi-Fi manager failed");
    if (s_provisioning) {
        return ESP_ERR_INVALID_STATE;
    }

    s_provisioning = true;
    s_provisioning_mode = WIFI_PROVISIONING_SMARTCONFIG;
    s_sta_should_connect = false;
    s_connected = false;
    s_connecting = false;
    (void)esp_wifi_disconnect();
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set SmartConfig STA mode failed");

    if (!s_wifi_started) {
        esp_err_t ret = esp_wifi_start();
        if (ret != ESP_OK) {
            s_provisioning = false;
            s_provisioning_mode = WIFI_PROVISIONING_NONE;
            return ret;
        }
        s_wifi_started = true;
        /* Wi-Fi START 事件到达后再启动 SmartConfig，避免底层尚未就绪。 */
        return ESP_OK;
    }

    esp_err_t ret = start_smartconfig_service();
    if (ret != ESP_OK) {
        s_provisioning = false;
        s_provisioning_mode = WIFI_PROVISIONING_NONE;
    }
    return ret;
}

esp_err_t wifi_manager_stop_provisioning(void)
{
    if (!s_provisioning && s_provisioning_mode == WIFI_PROVISIONING_NONE) {
        return ESP_OK;
    }

    if (s_provisioning_mode == WIFI_PROVISIONING_AP) {
        stop_ap_services();
        (void)esp_wifi_set_mode(WIFI_MODE_STA);
    } else if (s_provisioning_mode == WIFI_PROVISIONING_SMARTCONFIG) {
        if (s_smartconfig_started) {
            (void)esp_smartconfig_stop();
            s_smartconfig_started = false;
        }
    } else if (s_provisioning_mode == WIFI_PROVISIONING_BLE && s_ble_manager_initialized) {
        /* BLE stop 是异步的，WIFI_PROV_END 回调中再安全释放 manager。 */
        wifi_prov_mgr_stop_provisioning();
        return ESP_OK;
    }

    s_provisioning = false;
    s_provisioning_mode = WIFI_PROVISIONING_NONE;
    return ESP_OK;
}

esp_err_t wifi_manager_start_saved_connection(void)
{
    wifi_credentials_t credentials = {0};
    esp_err_t ret = load_credentials(&credentials);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_RETURN_ON_ERROR(wifi_manager_init(), TAG, "initialize Wi-Fi manager failed");
    return start_station(&credentials);
}

bool wifi_manager_is_provisioning(void)
{
    return s_provisioning;
}

bool wifi_manager_has_time(void)
{
    time_t now;
    time(&now);
    return now >= WIFI_MANAGER_VALID_TIME_EPOCH;
}
