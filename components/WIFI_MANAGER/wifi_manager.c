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
#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"

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
static esp_netif_t *s_ap_netif;
static httpd_handle_t s_http_server;
static TaskHandle_t s_apply_task;
static TaskHandle_t s_dns_task;
static TaskHandle_t s_sntp_monitor_task;
static int s_dns_socket = -1;
static wifi_credentials_t s_pending_credentials;
static wifi_ap_record_t s_scan_results[WIFI_MANAGER_MAX_SCAN_RESULTS];
static uint16_t s_scan_result_count;

static void start_sntp(void);
static void time_sync_notification_cb(struct timeval *timeval);
static void sntp_monitor_task(void *argument);
static esp_err_t start_http_server(void);
static esp_err_t start_station(const wifi_credentials_t *credentials);
static esp_err_t save_credentials(const wifi_credentials_t *credentials);
static esp_err_t load_credentials(wifi_credentials_t *credentials);
static void apply_credentials_task(void *argument);
static void scan_available_networks(void);
static esp_err_t start_dns_server(void);
static void stop_dns_server(void);
static void dns_server_task(void *argument);

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
        if (s_sta_should_connect) {
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
        start_sntp();
        ESP_LOGI(TAG, "Wi-Fi connected");
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

static esp_err_t start_station(const wifi_credentials_t *credentials)
{
    wifi_config_t sta_config;
    credentials_to_sta_config(credentials, &sta_config);

    s_connected = false;
    s_connecting = true;
    s_sta_should_connect = true;
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

static void html_escape_ssid(const uint8_t *ssid, char *escaped, size_t escaped_size)
{
    size_t written = 0;
    for (size_t index = 0; index < WIFI_MANAGER_SSID_MAX_LEN && ssid[index] != '\0'; index++) {
        const char *replacement = NULL;
        switch (ssid[index]) {
        case '&': replacement = "&amp;"; break;
        case '<': replacement = "&lt;"; break;
        case '>': replacement = "&gt;"; break;
        case '\"': replacement = "&quot;"; break;
        case '\'': replacement = "&#39;"; break;
        default: break;
        }
        if (replacement != NULL) {
            size_t length = strlen(replacement);
            if (written + length >= escaped_size) {
                break;
            }
            memcpy(escaped + written, replacement, length);
            written += length;
        } else if (ssid[index] >= 0x20 && written + 1 < escaped_size) {
            escaped[written++] = (char)ssid[index];
        }
    }
    escaped[written] = '\0';
}

static esp_err_t root_get_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request,
                        "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
                        "<title>ESP_AP 配网</title><style>body{margin:0;background:#f2f6fc;font-family:Arial,sans-serif;color:#172033}"
                        ".card{max-width:360px;margin:8vh auto;padding:28px;background:#fff;border-radius:20px;box-shadow:0 10px 30px #ccd6e6}"
                        "h2{margin:0 0 8px}p{color:#667085;font-size:14px}label{display:block;margin-top:18px;font-size:14px}"
                        "select,input,button{box-sizing:border-box;width:100%;margin-top:7px;padding:13px;border-radius:10px;border:1px solid #d0d9e8;font-size:16px}"
                        "button{border:0;background:#2563eb;color:white;font-weight:bold;margin-top:24px}.hint{font-size:12px;color:#8a94a6}</style></head>"
                        "<body><main class=card><h2>连接 Wi-Fi</h2><p>请选择要连接的无线网络</p><form method=post action=/configure>"
                        "<label>附近 Wi-Fi<select name=ssid required><option value=''>请选择 Wi-Fi</option>",
                        HTTPD_RESP_USE_STRLEN), TAG, "send provisioning page failed");

    for (uint16_t index = 0; index < s_scan_result_count; index++) {
        char escaped_ssid[WIFI_MANAGER_SSID_MAX_LEN * 6 + 1];
        char option[WIFI_MANAGER_SSID_MAX_LEN * 12 + 64];
        html_escape_ssid(s_scan_results[index].ssid, escaped_ssid, sizeof(escaped_ssid));
        if (escaped_ssid[0] == '\0') {
            continue;
        }
        snprintf(option, sizeof(option), "<option value=\"%s\">%s (%d dBm)</option>",
                 escaped_ssid, escaped_ssid, s_scan_results[index].rssi);
        ESP_RETURN_ON_ERROR(httpd_resp_send_chunk(request, option, HTTPD_RESP_USE_STRLEN), TAG,
                            "send Wi-Fi option failed");
    }

    return httpd_resp_send_chunk(request,
        "</select></label><label>Wi-Fi 密码<input name=password type=password maxlength=64 placeholder='开放网络可留空'></label>"
        "<button type=submit>保存并连接</button></form><p class=hint>列表仅显示附近的 2.4GHz Wi-Fi。</p></main></body></html>",
        HTTPD_RESP_USE_STRLEN);
}

static esp_err_t configure_post_handler(httpd_req_t *request)
{
    if (request->content_len == 0 || request->content_len > WIFI_MANAGER_FORM_MAX_LEN) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid form data");
        return ESP_FAIL;
    }

    char form[WIFI_MANAGER_FORM_MAX_LEN + 1] = {0};
    int received = 0;
    while (received < request->content_len) {
        int ret = httpd_req_recv(request, form + received, request->content_len - received);
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += ret;
    }

    char password_form[WIFI_MANAGER_FORM_MAX_LEN + 1];
    memcpy(password_form, form, sizeof(password_form));
    wifi_credentials_t credentials = {0};
    if (!form_get_value(form, "ssid", credentials.ssid, sizeof(credentials.ssid)) ||
        credentials.ssid[0] == '\0' ||
        !form_get_value(password_form, "password", credentials.password, sizeof(credentials.password))) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid Wi-Fi name or password");
        return ESP_FAIL;
    }
    if (s_apply_task != NULL) {
        httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Configuration is already being applied");
        return ESP_FAIL;
    }

    esp_err_t ret = save_credentials(&credentials);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Save Wi-Fi credentials failed: %s", esp_err_to_name(ret));
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Save failed");
        return ret;
    }
    s_pending_credentials = credentials;
    if (xTaskCreatePinnedToCore(apply_credentials_task, "wifi_apply", 4096, NULL, 3,
                                &s_apply_task, 1) != pdPASS) {
        s_apply_task = NULL;
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Apply task failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_sendstr(request, "<p>已保存，正在连接 Wi-Fi。</p>");
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

    const httpd_uri_t root_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t configure_uri = {
        .uri = "/configure",
        .method = HTTP_POST,
        .handler = configure_post_handler,
        .user_ctx = NULL,
    };
    esp_err_t ret = httpd_register_uri_handler(s_http_server, &root_uri);
    if (ret == ESP_OK) {
        ret = httpd_register_uri_handler(s_http_server, &configure_uri);
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
    }
    close(socket_fd);
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

static void apply_credentials_task(void *argument)
{
    vTaskDelay(pdMS_TO_TICKS(300));
    stop_dns_server();
    if (s_http_server != NULL) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }

    esp_err_t ret = start_station(&s_pending_credentials);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Apply Wi-Fi credentials failed: %s", esp_err_to_name(ret));
    }
    s_apply_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t wifi_manager_start_provisioning(void)
{
    ESP_RETURN_ON_ERROR(wifi_manager_init(), TAG, "initialize Wi-Fi manager failed");

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
