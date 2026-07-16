#include "homekit_accessory.h"

#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_ESP_BH1750_HOMEKIT_ENABLE
#include "hap.h"
#include "hap_apple_chars.h"
#include "hap_apple_servs.h"
#endif

static const char *TAG = "homekit_accessory";

#if CONFIG_ESP_BH1750_HOMEKIT_ENABLE

static hap_char_t *s_lux_char = NULL;
static bool s_homekit_started;
static bool s_homekit_pairing;
static bool s_homekit_pairing_timed_out;
static int s_homekit_connected_controllers;

static int light_sensor_identify(hap_acc_t *ha)
{
    (void)ha;
    ESP_LOGI(TAG, "Light Sensor identify requested");
    return HAP_SUCCESS;
}

static void homekit_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event, void *data)
{
    (void)arg;
    (void)event_base;

    switch (event) {
    case HAP_EVENT_PAIRING_STARTED:
        s_homekit_pairing = true;
        s_homekit_pairing_timed_out = false;
        ESP_LOGI(TAG, "HomeKit pairing started");
        break;
    case HAP_EVENT_PAIRING_ABORTED:
        s_homekit_pairing = false;
        ESP_LOGI(TAG, "HomeKit pairing aborted");
        break;
    case HAP_EVENT_CTRL_PAIRED:
        s_homekit_pairing = false;
        s_homekit_pairing_timed_out = false;
        ESP_LOGI(TAG, "HomeKit controller paired: %s", (char *)data);
        break;
    case HAP_EVENT_CTRL_UNPAIRED:
        ESP_LOGI(TAG, "HomeKit controller removed: %s", (char *)data);
        break;
    case HAP_EVENT_CTRL_CONNECTED:
        ++s_homekit_connected_controllers;
        ESP_LOGI(TAG, "HomeKit controller connected");
        break;
    case HAP_EVENT_CTRL_DISCONNECTED:
        if (s_homekit_connected_controllers > 0) {
            --s_homekit_connected_controllers;
        }
        ESP_LOGI(TAG, "HomeKit controller disconnected");
        break;
    case HAP_EVENT_PAIRING_MODE_TIMED_OUT:
        s_homekit_pairing = false;
        s_homekit_pairing_timed_out = true;
        ESP_LOGI(TAG, "HomeKit pairing mode timed out");
        break;
    default:
        break;
    }
}

esp_err_t homekit_accessory_start(void)
{
    hap_cfg_t hap_cfg = {};
    hap_get_config(&hap_cfg);
    hap_cfg.unique_param = UNIQUE_NAME;
    hap_set_config(&hap_cfg);

    ESP_RETURN_ON_FALSE(hap_init(HAP_TRANSPORT_WIFI) == HAP_SUCCESS,
                        ESP_FAIL, TAG, "hap_init failed");

    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char serial[18] = {};
    snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    hap_acc_cfg_t cfg = {
        .name = "ESP-BH1750",
        .manufacturer = "AZDelivery",
        .model = "BH1750",
        .serial_num = serial,
        .fw_rev = "0.1.0",
        .hw_rev = NULL,
        .pv = "1.1.0",
        .identify_routine = light_sensor_identify,
        .cid = HAP_CID_SENSOR,
    };

    hap_acc_t *accessory = hap_acc_create(&cfg);
    ESP_RETURN_ON_FALSE(accessory != NULL, ESP_ERR_NO_MEM, TAG, "failed to create accessory");

    uint8_t product_data[] = {'B', 'H', '1', '7', '5', '0', 'L', 'S'};
    hap_acc_add_product_data(accessory, product_data, sizeof(product_data));

    // Create Light Sensor service with initial ambient light level = 0.0001 (Apple minimum float)
    hap_serv_t *service = hap_serv_light_sensor_create(0.0001f);
    ESP_RETURN_ON_FALSE(service != NULL, ESP_ERR_NO_MEM, TAG, "failed to create Light Sensor service");

    hap_serv_add_char(service, hap_char_name_create("Light Sensor"));
    s_lux_char = hap_serv_get_char_by_uuid(service, HAP_CHAR_UUID_CURRENT_AMBIENT_LIGHT_LEVEL);

    hap_acc_add_serv(accessory, service);
    hap_add_accessory(accessory);

    hap_set_setup_code(CONFIG_ESP_BH1750_HOMEKIT_SETUP_CODE);
    hap_set_setup_id(CONFIG_ESP_BH1750_HOMEKIT_SETUP_ID);
    hap_enable_mfi_auth(HAP_MFI_AUTH_NONE);

    esp_event_handler_register(HAP_EVENT, ESP_EVENT_ANY_ID, homekit_event_handler, NULL);

    ESP_RETURN_ON_FALSE(hap_start() == HAP_SUCCESS, ESP_FAIL, TAG, "hap_start failed");
    s_homekit_started = true;
    s_homekit_pairing = false;
    s_homekit_pairing_timed_out = false;

    char *payload = esp_hap_get_setup_payload(CONFIG_ESP_BH1750_HOMEKIT_SETUP_CODE,
                                              CONFIG_ESP_BH1750_HOMEKIT_SETUP_ID,
                                              false,
                                              cfg.cid);
    ESP_LOGI(TAG, "HomeKit accessory ready on port %d", CONFIG_HAP_HTTP_SERVER_PORT);
    ESP_LOGI(TAG, "Pairing code: %s", CONFIG_ESP_BH1750_HOMEKIT_SETUP_CODE);
    if (payload != NULL) {
        ESP_LOGI(TAG, "Setup payload: %s", payload);
        free(payload);
    }
    return ESP_OK;
}

void homekit_accessory_get_status(homekit_accessory_status_t *status)
{
    if (status == NULL) {
        return;
    }

    memset(status, 0, sizeof(*status));
    status->compiled = true;
    status->started = s_homekit_started;
    status->pairing = s_homekit_pairing;
    status->pairing_timed_out = s_homekit_pairing_timed_out;
    status->connected_controller_count = s_homekit_connected_controllers;
    strlcpy(status->setup_code, CONFIG_ESP_BH1750_HOMEKIT_SETUP_CODE,
            sizeof(status->setup_code));
    strlcpy(status->setup_id, CONFIG_ESP_BH1750_HOMEKIT_SETUP_ID,
            sizeof(status->setup_id));
    if (s_homekit_started) {
        status->paired_controller_count = hap_get_paired_controller_count();
    }

    char *payload = esp_hap_get_setup_payload(CONFIG_ESP_BH1750_HOMEKIT_SETUP_CODE,
                                              CONFIG_ESP_BH1750_HOMEKIT_SETUP_ID,
                                              false,
                                              HAP_CID_SENSOR);
    if (payload != NULL) {
        strlcpy(status->setup_payload, payload, sizeof(status->setup_payload));
        free(payload);
    }
}

void homekit_accessory_update_lux(float lux)
{
    if (s_lux_char == NULL) {
        return;
    }
    // Apple HAP specs: float value range [0.0001, 100000.0]
    if (lux < 0.0001f) {
        lux = 0.0001f;
    } else if (lux > 100000.0f) {
        lux = 100000.0f;
    }
    hap_val_t val = {
        .f = lux
    };
    hap_char_update_val(s_lux_char, &val);
}

#else

esp_err_t homekit_accessory_start(void)
{
    ESP_LOGI(TAG, "HomeKit disabled in configuration");
    return ESP_OK;
}

void homekit_accessory_get_status(homekit_accessory_status_t *status)
{
    if (status != NULL) {
        memset(status, 0, sizeof(*status));
    }
}

void homekit_accessory_update_lux(float lux)
{
    (void)lux;
}

#endif
