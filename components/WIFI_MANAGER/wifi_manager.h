#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>

#include "esp_err.h"

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_start_provisioning(void);
bool wifi_manager_is_provisioning(void);
bool wifi_manager_has_time(void);

#endif
