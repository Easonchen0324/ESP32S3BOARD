#ifndef WIFI_OTA_H
#define WIFI_OTA_H

#include "esp_err.h"

/**
 * @brief 启动 Wi-Fi OTA 网页服务。
 *
 * 已连接 Wi-Fi 时立即监听 8080 端口；尚未获取 IP 时会等待 IP_EVENT_STA_GOT_IP。
 * 本函数不会初始化或连接 Wi-Fi，调用前应先由业务层完成 Wi-Fi 初始化。
 */
esp_err_t wifi_ota_start(void);

/** @brief 停止 OTA 网页服务并取消尚未完成的升级会话。 */
esp_err_t wifi_ota_stop(void);

#endif
