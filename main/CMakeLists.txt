idf_component_register(
    SRCS "main.c" "iot_remote.c" "homekit_accessory.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c freertos esp_event esp_netif esp_wifi esp_http_server esp_hap_apple_profiles esp_hap_core nvs_flash lwip led_strip
)
