#pragma once
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool compiled;
    bool started;
    bool pairing;
    bool pairing_timed_out;
    int paired_controller_count;
    int connected_controller_count;
    char setup_code[16];
    char setup_id[8];
    char setup_payload[32];
} homekit_accessory_status_t;

esp_err_t homekit_accessory_start(void);
void homekit_accessory_get_status(homekit_accessory_status_t *status);
void homekit_accessory_update_lux(float lux);
