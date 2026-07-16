#pragma once
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

typedef void (*iot_remote_wifi_connected_cb_t)(void);

typedef struct {
    const char *wifi_ssid;
    const char *wifi_password;
    iot_remote_wifi_connected_cb_t on_wifi_connected;
} iot_remote_config_t;

esp_err_t iot_remote_start(const iot_remote_config_t *config);
void iot_remote_set_lux(float lux);
bool iot_remote_homekit_enabled(void);
