#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"

/** @brief 初始化网络栈、Wi-Fi 驱动和三种配网方式共用的事件处理器。 */
esp_err_t wifi_manager_init(void);

/** @brief 启动 AP 热点和中文 captive portal 配网页面。 */
esp_err_t wifi_manager_start_ap_provisioning(void);

/** @brief 启动官方 BLE Provisioning；设备名为 PROV_xxxxxx，默认 PoP 为 esp32s3。 */
esp_err_t wifi_manager_start_ble_provisioning(void);

/** @brief 启动 ESPTouch/AirKiss SmartConfig 配网。 */
esp_err_t wifi_manager_start_smartconfig_provisioning(void);

/** @brief 停止当前配网方式并释放其协议资源。 */
esp_err_t wifi_manager_stop_provisioning(void);

/** @brief 兼容旧业务代码，等同于 wifi_manager_start_ap_provisioning()。 */
esp_err_t wifi_manager_start_provisioning(void);

/** @brief 从自定义 NVS 读取上次成功保存的凭据并连接。 */
esp_err_t wifi_manager_start_saved_connection(void);
bool wifi_manager_is_provisioning(void);
bool wifi_manager_has_time(void);

#endif
