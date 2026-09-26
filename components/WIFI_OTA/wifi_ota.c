#include "wifi_ota.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"

#define WIFI_OTA_SERVER_PORT       8080
#define WIFI_OTA_CONTROL_PORT      32769
#define WIFI_OTA_PACKET_SIZE       (16 * 1024)
#define WIFI_OTA_RECEIVE_BUFFER    2048

typedef enum {
    OTA_STATE_IDLE,
    OTA_STATE_RECEIVING,
    OTA_STATE_COMPLETE,
    OTA_STATE_ABORTED,
    OTA_STATE_ERROR,
} ota_state_t;

typedef struct {
    esp_ota_handle_t handle;
    const esp_partition_t *target_partition;
    size_t total_size;
    size_t received_size;
    ota_state_t state;
} ota_session_t;

static const char *TAG = "wifi_ota";
static httpd_handle_t s_http_server;
static SemaphoreHandle_t s_session_mutex;
static ota_session_t s_session;
static bool s_service_initialized;

/* OTA 页面直接嵌入固件，不依赖 SD 卡或外部网络。 */
extern const uint8_t ota_html_start[] asm("_binary_ota_html_start");
extern const uint8_t ota_html_end[] asm("_binary_ota_html_end");

static esp_err_t start_http_server(void);

static const char *state_to_text(ota_state_t state)
{
    switch (state) {
    case OTA_STATE_RECEIVING: return "receiving";
    case OTA_STATE_COMPLETE: return "complete";
    case OTA_STATE_ABORTED: return "aborted";
    case OTA_STATE_ERROR: return "error";
    default: return "idle";
    }
}

static esp_err_t send_json_message(httpd_req_t *request, const char *status,
                                   bool success, const char *message)
{
    char response[224];
    snprintf(response, sizeof(response), "{\"ok\":%s,\"message\":\"%s\"}",
             success ? "true" : "false", message);
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, response);
}

static bool parse_size_header(httpd_req_t *request, const char *name, size_t *value)
{
    char text[24];
    if (httpd_req_get_hdr_value_str(request, name, text, sizeof(text)) != ESP_OK) {
        return false;
    }

    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0 || parsed > SIZE_MAX) {
        return false;
    }
    *value = (size_t)parsed;
    return true;
}

static bool parse_query_offset(httpd_req_t *request, size_t *offset)
{
    char query[64];
    char value[24];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "offset", value, sizeof(value)) != ESP_OK) {
        return false;
    }

    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed > SIZE_MAX) {
        return false;
    }
    *offset = (size_t)parsed;
    return true;
}

static void abort_session_locked(ota_state_t final_state)
{
    if (s_session.state == OTA_STATE_RECEIVING && s_session.handle != 0) {
        /* 未完成的句柄必须 abort，否则 OTA 库会一直占用目标分区。 */
        (void)esp_ota_abort(s_session.handle);
    }
    s_session.handle = 0;
    s_session.target_partition = NULL;
    s_session.total_size = 0;
    s_session.received_size = 0;
    s_session.state = final_state;
}

static esp_err_t index_get_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, (const char *)ota_html_start,
                           ota_html_end - ota_html_start);
}

static esp_err_t status_get_handler(httpd_req_t *request)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info = {0};
    wifi_ap_record_t ap_info = {0};
    if (sta_netif != NULL) {
        (void)esp_netif_get_ip_info(sta_netif, &ip_info);
    }
    int rssi = esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK ? ap_info.rssi : 0;

    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    ota_state_t state = s_session.state;
    size_t received = s_session.received_size;
    size_t total = s_session.total_size;
    const char *target_label = s_session.target_partition != NULL
                                   ? s_session.target_partition->label : "--";
    char response[384];
    snprintf(response, sizeof(response),
             "{\"version\":\"%s\",\"project\":\"%s\",\"running_partition\":\"%s\","
             "\"target_partition\":\"%s\",\"state\":\"%s\",\"received\":%u,\"total\":%u,"
             "\"ip\":\"" IPSTR "\",\"rssi\":%d}",
             app->version, app->project_name, running->label, target_label, state_to_text(state),
             (unsigned)received, (unsigned)total, IP2STR(&ip_info.ip), rssi);
    xSemaphoreGive(s_session_mutex);

    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_sendstr(request, response);
}

static esp_err_t ota_start_handler(httpd_req_t *request)
{
    size_t firmware_size;
    if (!parse_size_header(request, "X-Firmware-Size", &firmware_size)) {
        return send_json_message(request, "400 Bad Request", false, "固件大小参数无效");
    }

    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    if (s_session.state == OTA_STATE_RECEIVING) {
        xSemaphoreGive(s_session_mutex);
        return send_json_message(request, "409 Conflict", false, "已有升级任务正在进行");
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL || firmware_size > target->size) {
        xSemaphoreGive(s_session_mutex);
        return send_json_message(request, "400 Bad Request", false, "固件超过 OTA 分区容量");
    }

    esp_ota_handle_t handle = 0;
    esp_err_t ret = esp_ota_begin(target, firmware_size, &handle);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_session_mutex);
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(ret));
        return send_json_message(request, "500 Internal Server Error", false, "无法擦除目标分区");
    }

    s_session.handle = handle;
    s_session.target_partition = target;
    s_session.total_size = firmware_size;
    s_session.received_size = 0;
    s_session.state = OTA_STATE_RECEIVING;
    char response[160];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"partition\":\"%s\",\"size\":%u,\"packet_size\":%u}",
             target->label, (unsigned)firmware_size, WIFI_OTA_PACKET_SIZE);
    xSemaphoreGive(s_session_mutex);

    ESP_LOGI(TAG, "OTA started: partition=%s, size=%u", target->label, (unsigned)firmware_size);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    return httpd_resp_sendstr(request, response);
}

static esp_err_t ota_chunk_handler(httpd_req_t *request)
{
    size_t requested_offset;
    if (!parse_query_offset(request, &requested_offset) || request->content_len <= 0 ||
        request->content_len > WIFI_OTA_PACKET_SIZE) {
        return send_json_message(request, "400 Bad Request", false, "数据帧参数无效");
    }

    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    if (s_session.state != OTA_STATE_RECEIVING || requested_offset != s_session.received_size ||
        s_session.received_size + request->content_len > s_session.total_size) {
        xSemaphoreGive(s_session_mutex);
        return send_json_message(request, "409 Conflict", false, "数据帧顺序或长度不正确");
    }

    /* 小缓冲循环接收 HTTP 数据，避免为了 16 KiB 帧长期占用一块连续内部 RAM。 */
    uint8_t buffer[WIFI_OTA_RECEIVE_BUFFER];
    size_t frame_received = 0;
    while (frame_received < (size_t)request->content_len) {
        size_t remaining = request->content_len - frame_received;
        int received = httpd_req_recv(request, (char *)buffer,
                                      remaining < sizeof(buffer) ? remaining : sizeof(buffer));
        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (received <= 0 || esp_ota_write(s_session.handle, buffer, received) != ESP_OK) {
            abort_session_locked(OTA_STATE_ERROR);
            xSemaphoreGive(s_session_mutex);
            ESP_LOGE(TAG, "Receive or write OTA frame failed at offset %u", (unsigned)requested_offset);
            return send_json_message(request, "500 Internal Server Error", false, "固件帧写入失败");
        }
        frame_received += (size_t)received;
    }
    s_session.received_size += frame_received;
    size_t total_received = s_session.received_size;
    size_t total_size = s_session.total_size;
    xSemaphoreGive(s_session_mutex);

    char response[160];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"offset\":%u,\"frame_size\":%u,\"received\":%u,\"total\":%u}",
             (unsigned)requested_offset, (unsigned)frame_received,
             (unsigned)total_received, (unsigned)total_size);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    return httpd_resp_sendstr(request, response);
}

static esp_err_t ota_finish_handler(httpd_req_t *request)
{
    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    if (s_session.state != OTA_STATE_RECEIVING ||
        s_session.received_size != s_session.total_size) {
        xSemaphoreGive(s_session_mutex);
        return send_json_message(request, "409 Conflict", false, "固件尚未完整接收");
    }

    const esp_partition_t *target = s_session.target_partition;
    /* esp_ota_end 会校验 ESP 镜像头、段结构和校验和，失败时绝不切换启动分区。 */
    esp_err_t ret = esp_ota_end(s_session.handle);
    s_session.handle = 0;
    if (ret == ESP_OK) {
        ret = esp_ota_set_boot_partition(target);
    }
    if (ret != ESP_OK) {
        s_session.state = OTA_STATE_ERROR;
        xSemaphoreGive(s_session_mutex);
        ESP_LOGE(TAG, "Finalize OTA failed: %s", esp_err_to_name(ret));
        return send_json_message(request, "400 Bad Request", false, "固件校验失败，启动分区未改变");
    }

    s_session.state = OTA_STATE_COMPLETE;
    xSemaphoreGive(s_session_mutex);
    ESP_LOGI(TAG, "OTA image verified; next boot partition is %s", target->label);
    return send_json_message(request, "200 OK", true, "固件校验成功，可重启进入新版本");
}

static esp_err_t ota_abort_handler(httpd_req_t *request)
{
    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    abort_session_locked(OTA_STATE_ABORTED);
    xSemaphoreGive(s_session_mutex);
    ESP_LOGW(TAG, "OTA aborted by browser");
    return send_json_message(request, "200 OK", true, "升级已取消");
}

static void reboot_task(void *argument)
{
    /* 先留出时间发送 HTTP 响应，避免浏览器把成功重启误判为请求失败。 */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t reboot_handler(httpd_req_t *request)
{
    xSemaphoreTake(s_session_mutex, portMAX_DELAY);
    bool ready = s_session.state == OTA_STATE_COMPLETE;
    xSemaphoreGive(s_session_mutex);
    if (!ready) {
        return send_json_message(request, "409 Conflict", false, "没有已完成的固件可供重启");
    }
    if (xTaskCreate(reboot_task, "ota_reboot", 2048, NULL, 5, NULL) != pdPASS) {
        return send_json_message(request, "500 Internal Server Error", false, "创建重启任务失败");
    }
    return send_json_message(request, "200 OK", true, "设备即将重启");
}

static esp_err_t start_http_server(void)
{
    if (s_http_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WIFI_OTA_SERVER_PORT;
    /* 多个 HTTP server 的控制端口也必须不同，否则会在 bind 阶段失败。 */
    config.ctrl_port = WIFI_OTA_CONTROL_PORT;
    config.stack_size = 8192;
    config.max_uri_handlers = 10;
    config.lru_purge_enable = true;
    ESP_RETURN_ON_ERROR(httpd_start(&s_http_server, &config), TAG, "start OTA HTTP server failed");

    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler},
        {.uri = "/api/ota/start", .method = HTTP_POST, .handler = ota_start_handler},
        {.uri = "/api/ota/chunk", .method = HTTP_POST, .handler = ota_chunk_handler},
        {.uri = "/api/ota/finish", .method = HTTP_POST, .handler = ota_finish_handler},
        {.uri = "/api/ota/abort", .method = HTTP_POST, .handler = ota_abort_handler},
        {.uri = "/api/ota/reboot", .method = HTTP_POST, .handler = reboot_handler},
    };
    for (size_t index = 0; index < sizeof(handlers) / sizeof(handlers[0]); index++) {
        esp_err_t ret = httpd_register_uri_handler(s_http_server, &handlers[index]);
        if (ret != ESP_OK) {
            httpd_stop(s_http_server);
            s_http_server = NULL;
            return ret;
        }
    }

    ESP_LOGI(TAG, "OTA page ready at http://<device-ip>:%d", WIFI_OTA_SERVER_PORT);
    return ESP_OK;
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        ESP_LOGI(TAG, "OTA page: http://" IPSTR ":%d/",
                 IP2STR(&event->ip_info.ip), WIFI_OTA_SERVER_PORT);
        if (start_http_server() != ESP_OK) {
            ESP_LOGE(TAG, "OTA web service is unavailable");
        }
    }
}

esp_err_t wifi_ota_start(void)
{
    if (s_service_initialized) {
        return ESP_OK;
    }

    s_session_mutex = xSemaphoreCreateMutex();
    if (s_session_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_session.state = OTA_STATE_IDLE;

    esp_err_t ret = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               ip_event_handler, NULL);
    if (ret != ESP_OK) {
        vSemaphoreDelete(s_session_mutex);
        s_session_mutex = NULL;
        return ret;
    }
    s_service_initialized = true;

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        ret = start_http_server();
        if (ret != ESP_OK) {
            (void)wifi_ota_stop();
            return ret;
        }
    } else {
        ESP_LOGI(TAG, "Waiting for Wi-Fi IP before starting OTA page");
    }
    return ESP_OK;
}

esp_err_t wifi_ota_stop(void)
{
    if (!s_service_initialized) {
        return ESP_OK;
    }

    (void)esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler);
    if (s_http_server != NULL) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }
    if (s_session_mutex != NULL) {
        xSemaphoreTake(s_session_mutex, portMAX_DELAY);
        abort_session_locked(OTA_STATE_IDLE);
        xSemaphoreGive(s_session_mutex);
        vSemaphoreDelete(s_session_mutex);
        s_session_mutex = NULL;
    }
    s_service_initialized = false;
    return ESP_OK;
}
