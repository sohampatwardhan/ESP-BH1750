#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "nvs_flash.h"
#include "iot_remote.h"
#include "homekit_accessory.h"

static const char *TAG = "app_main";

#define RGB_LED_GPIO 2

#define I2C_MASTER_SDA_IO 21
#define I2C_MASTER_SCL_IO 22
#define I2C_MASTER_PORT 0
#define I2C_MASTER_FREQ_HZ 100000
#define I2C_TIMEOUT_MS 1000

#define BH1750_ADDR_LOW 0x23
#define BH1750_ADDR_HIGH 0x5C
#define BH1750_POWER_ON 0x01
#define BH1750_RESET 0x07
#define BH1750_CONTINUOUS_HIGH_RES_MODE 0x10
#define BH1750_MEASUREMENT_TIME_MS 180
#define PRINT_INTERVAL_MS 5000

static led_strip_handle_t s_rgb_led;

static esp_err_t rgb_led_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_LED_GPIO,
        .max_leds = 1,
    };
    led_strip_spi_config_t spi_config = {
        .spi_bus = SPI2_HOST,
        .flags.with_dma = true,
    };

    ESP_RETURN_ON_ERROR(led_strip_new_spi_device(&strip_config, &spi_config, &s_rgb_led),
                        TAG, "failed to create RGB LED strip");
    ESP_RETURN_ON_ERROR(led_strip_clear(s_rgb_led), TAG, "failed to clear RGB LED");
    ESP_LOGI(TAG, "RGB LED ready on GPIO %d", RGB_LED_GPIO);
    return ESP_OK;
}

static void rgb_led_blink_green_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 5; ++i) {
        led_strip_set_pixel(s_rgb_led, 0, 0, 24, 0);
        led_strip_refresh(s_rgb_led);
        vTaskDelay(pdMS_TO_TICKS(200));
        led_strip_clear(s_rgb_led);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    vTaskDelete(NULL);
}

static void on_wifi_connected(void)
{
    if (s_rgb_led != NULL) {
        xTaskCreate(rgb_led_blink_green_task, "led_blink", 2048, NULL, 5, NULL);
    }
}

static uint8_t find_bh1750_address(i2c_master_bus_handle_t bus_handle)
{
    uint8_t bh1750_address = 0;

    printf("\nScanning I2C bus on SDA GPIO %d, SCL GPIO %d...\n",
           I2C_MASTER_SDA_IO,
           I2C_MASTER_SCL_IO);

    for (uint8_t address = 0x08; address <= 0x77; address++) {
        esp_err_t err = i2c_master_probe(bus_handle, address, I2C_TIMEOUT_MS);
        if (err == ESP_OK) {
            printf("I2C device found at 0x%02X", address);

            if (address == BH1750_ADDR_LOW || address == BH1750_ADDR_HIGH) {
                printf(" (BH1750)");
                bh1750_address = address;
            }

            printf("\n");
        }
    }

    if (bh1750_address == 0) {
        printf("BH1750 not found. Expected address 0x%02X or 0x%02X.\n",
               BH1750_ADDR_LOW,
               BH1750_ADDR_HIGH);
    }

    return bh1750_address;
}

static esp_err_t bh1750_write_command(i2c_master_dev_handle_t bh1750_handle, uint8_t command)
{
    return i2c_master_transmit(bh1750_handle, &command, sizeof(command), I2C_TIMEOUT_MS);
}

static esp_err_t bh1750_read_lux_x10(i2c_master_dev_handle_t bh1750_handle, uint32_t *lux_x10)
{
    uint8_t data[2] = {0};
    esp_err_t err = i2c_master_receive(bh1750_handle, data, sizeof(data), I2C_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }

    uint16_t raw = ((uint16_t)data[0] << 8) | data[1];
    *lux_x10 = ((uint32_t)raw * 100U + 6U) / 12U;
    return ESP_OK;
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_err_t led_err = rgb_led_init();
    if (led_err != ESP_OK) {
        ESP_LOGW(TAG, "RGB LED init failed: %s", esp_err_to_name(led_err));
    }

    iot_remote_config_t remote_config = {
        .wifi_ssid = CONFIG_ESP_BH1750_WIFI_SSID,
        .wifi_password = CONFIG_ESP_BH1750_WIFI_PASSWORD,
        .on_wifi_connected = on_wifi_connected,
    };
    esp_err_t wifi_err = iot_remote_start(&remote_config);
    if (wifi_err != ESP_OK) {
        printf("Failed to start WiFi provisioning: %d\n", wifi_err);
    }

    if (iot_remote_homekit_enabled()) {
        esp_err_t hk_err = homekit_accessory_start();
        if (hk_err != ESP_OK) {
            printf("Failed to start HomeKit: %d\n", hk_err);
        }
    } else {
        printf("HomeKit is disabled in configuration\n");
    }

    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_MASTER_PORT,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    i2c_master_bus_handle_t bus_handle = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_config, &bus_handle);
    if (err != ESP_OK) {
        printf("Failed to initialize I2C master bus: %d\n", err);
        return;
    }

    uint8_t bh1750_address = 0;
    i2c_master_dev_handle_t bh1750_handle = NULL;

    while (bh1750_address == 0) {
        bh1750_address = find_bh1750_address(bus_handle);
        if (bh1750_address == 0) {
            printf("Retrying I2C scan in %d seconds...\n", PRINT_INTERVAL_MS / 1000);
            vTaskDelay(pdMS_TO_TICKS(PRINT_INTERVAL_MS));
        }
    }

    i2c_device_config_t bh1750_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = bh1750_address,
        .scl_speed_hz = I2C_MASTER_FREQ_HZ,
    };

    err = i2c_master_bus_add_device(bus_handle, &bh1750_config, &bh1750_handle);
    if (err != ESP_OK) {
        printf("Failed to add BH1750 at 0x%02X: %d\n", bh1750_address, err);
        return;
    }

    printf("Using BH1750 at I2C address 0x%02X.\n", bh1750_address);

    err = bh1750_write_command(bh1750_handle, BH1750_POWER_ON);
    if (err != ESP_OK) {
        printf("Failed to power on BH1750: %d\n", err);
        return;
    }

    err = bh1750_write_command(bh1750_handle, BH1750_RESET);
    if (err != ESP_OK) {
        printf("Failed to reset BH1750: %d\n", err);
        return;
    }

    err = bh1750_write_command(bh1750_handle, BH1750_CONTINUOUS_HIGH_RES_MODE);
    if (err != ESP_OK) {
        printf("Failed to start BH1750 continuous high-resolution mode: %d\n", err);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(BH1750_MEASUREMENT_TIME_MS));

    while (true) {
        uint32_t lux_x10 = 0;
        err = bh1750_read_lux_x10(bh1750_handle, &lux_x10);
        if (err == ESP_OK) {
            float lux = (float)lux_x10 / 10.0f;
            printf("Light sensitivity: %.1f lux\n", lux);
            iot_remote_set_lux(lux);
            homekit_accessory_update_lux(lux);
        } else {
            printf("Failed to read BH1750 light level: %d\n", err);
        }

        vTaskDelay(pdMS_TO_TICKS(PRINT_INTERVAL_MS));
    }
}
