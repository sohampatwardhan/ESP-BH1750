# MQTT / Home Assistant Integration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Publish the BH1750 illuminance reading over MQTT with Home Assistant MQTT Discovery, so it appears in HA automatically with no YAML.

**Architecture:** A new, self-contained `mqtt_bridge.c/h` module (mirroring `homekit_accessory.c/h`'s role) owns the `esp_mqtt_client` lifecycle, its own `mqtt_cfg` NVS namespace, and the HA discovery payload. `iot_remote.c` gains two thin HTTP handlers (`GET`/`POST /mqtt`) and a new Settings page section, following the exact shape of its existing `/wifi` and `/homekit` handlers. `main.c` gains two calls: `mqtt_bridge_start()` at boot and `mqtt_bridge_update_lux()` in the sensor loop.

**Tech Stack:** ESP-IDF 5.x, the built-in `mqtt` component (`esp-mqtt`, `mqtt_client.h`), NVS for settings persistence, the existing captive-portal HTTP server (`esp_http_server`).

**Reference spec:** `docs/superpowers/specs/2026-07-15-mqtt-home-assistant-integration-design.md`

## Global Constraints

- Plain `mqtt://` transport only — no TLS/`mqtts://` in this iteration.
- No MQTT-controlled entities beyond the lux sensor — no reboot button, no HomeKit toggle over MQTT.
- No explicit MQTT enable/disable UI toggle — a non-empty broker host means enabled; an empty host means disabled.
- No refactor of `iot_remote.c`'s WiFi/AP/HTTP/DNS structure in this change — that's a separate follow-up project.
- The MQTT topic/device identifier is the existing user-editable `hostname` setting — no second "device name" concept.
- HA MQTT Discovery convention: retained config at `homeassistant/sensor/<hostname>/lux/config`.
- Discovery `device` block: `"manufacturer": "AZDelivery"`, `"model": "BH1750"` (matches the HomeKit accessory info fixed in Task 1).
- This project has no unit test framework (pure ESP-IDF firmware). Every task's verification is `idf.py build` (must succeed) plus, where the spec calls for it, a manual hardware/broker check. Steps say so explicitly instead of pretending there's a test suite.

---

### Task 1: Commit the HomeKit manufacturer/model fix

**Files:**
- Modify: `main/homekit_accessory.c:96-97`

**Interfaces:** None — isolated data fix, no API changes.

- [ ] **Step 1: Confirm the working-tree change is in place**

`main/homekit_accessory.c:96-97` inside the `hap_acc_cfg_t cfg` initializer in `homekit_accessory_start()` should read:

```c
        .manufacturer = "AZDelivery",
        .model = "BH1750",
```

If it doesn't (e.g. this plan is being run in a fresh checkout), make that edit now.

- [ ] **Step 2: Build to verify no regression**

Run: `idf.py build`
Expected: `Project build complete.` with no errors.

- [ ] **Step 3: Commit**

```bash
git add main/homekit_accessory.c
git commit -m "fix: correct HomeKit accessory manufacturer/model to AZDelivery/BH1750"
```

---

### Task 2: Hostname accessor + mqtt_bridge settings skeleton

**Files:**
- Modify: `main/iot_remote.h`
- Modify: `main/iot_remote.c` (add `iot_remote_get_hostname()`)
- Create: `main/mqtt_bridge.h`
- Create: `main/mqtt_bridge.c` (NVS-backed settings only — no MQTT client yet)
- Modify: `main/CMakeLists.txt` (add `mqtt_bridge.c` to `SRCS`)

**Interfaces:**
- Consumes: nothing new.
- Produces:
  - `const char *iot_remote_get_hostname(void);` — returns the device's current hostname string (never NULL, always null-terminated).
  - `main/mqtt_bridge.h` full public API (all of it declared now; `mqtt_bridge_start`/`mqtt_bridge_update_lux` are stubs until Task 3/4):
    ```c
    #define MQTT_HOST_MAX_LEN 63
    #define MQTT_USERNAME_MAX_LEN 32
    #define MQTT_PASSWORD_MAX_LEN 64

    typedef struct {
        bool configured;
        bool connected;
        char host[MQTT_HOST_MAX_LEN + 1];
        uint16_t port;
    } mqtt_bridge_status_t;

    esp_err_t mqtt_bridge_start(void);
    void mqtt_bridge_update_lux(float lux);
    esp_err_t mqtt_bridge_save_settings(const char *host, uint16_t port,
                                         const char *username, const char *password);
    void mqtt_bridge_get_status(mqtt_bridge_status_t *status);
    ```

- [ ] **Step 1: Add the hostname accessor to `iot_remote.h`**

Add this line to `main/iot_remote.h`, after the existing `iot_remote_homekit_enabled` declaration:

```c
const char *iot_remote_get_hostname(void);
```

The full file should now read:

```c
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
const char *iot_remote_get_hostname(void);
```

- [ ] **Step 2: Implement the accessor in `iot_remote.c`**

Add this function to `main/iot_remote.c`, right after `iot_remote_homekit_enabled(void)` (currently ending around line 435):

```c
const char *iot_remote_get_hostname(void)
{
    return s_device_hostname;
}
```

- [ ] **Step 3: Build to verify it compiles**

Run: `idf.py build`
Expected: `Project build complete.` (this only adds a getter; nothing calls it yet, which is fine).

- [ ] **Step 4: Create `main/mqtt_bridge.h`**

```c
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define MQTT_HOST_MAX_LEN 63
#define MQTT_USERNAME_MAX_LEN 32
#define MQTT_PASSWORD_MAX_LEN 64

typedef struct {
    bool configured;
    bool connected;
    char host[MQTT_HOST_MAX_LEN + 1];
    uint16_t port;
} mqtt_bridge_status_t;

esp_err_t mqtt_bridge_start(void);
void mqtt_bridge_update_lux(float lux);
esp_err_t mqtt_bridge_save_settings(const char *host, uint16_t port,
                                     const char *username, const char *password);
void mqtt_bridge_get_status(mqtt_bridge_status_t *status);
```

- [ ] **Step 5: Create `main/mqtt_bridge.c` with settings persistence only**

```c
#include "mqtt_bridge.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "mqtt_bridge";

static char s_host[MQTT_HOST_MAX_LEN + 1];
static uint16_t s_port = 1883;
static char s_username[MQTT_USERNAME_MAX_LEN + 1];
static char s_password[MQTT_PASSWORD_MAX_LEN + 1];
static bool s_connected;

static bool has_text(const char *value)
{
    return value != NULL && value[0] != '\0';
}

static void load_mqtt_settings(void)
{
    s_host[0] = '\0';
    s_port = 1883;
    s_username[0] = '\0';
    s_password[0] = '\0';

    nvs_handle_t nvs = 0;
    if (nvs_open("mqtt_cfg", NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    size_t len = sizeof(s_host);
    nvs_get_str(nvs, "host", s_host, &len);

    uint16_t port = 0;
    if (nvs_get_u16(nvs, "port", &port) == ESP_OK && port > 0) {
        s_port = port;
    }

    len = sizeof(s_username);
    nvs_get_str(nvs, "user", s_username, &len);

    len = sizeof(s_password);
    nvs_get_str(nvs, "pass", s_password, &len);

    nvs_close(nvs);
}

esp_err_t mqtt_bridge_save_settings(const char *host, uint16_t port,
                                     const char *username, const char *password)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("mqtt_cfg", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, "host", host != NULL ? host : "");
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "port", port);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "user", username != NULL ? username : "");
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "pass", password != NULL ? password : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

void mqtt_bridge_get_status(mqtt_bridge_status_t *status)
{
    if (status == NULL) {
        return;
    }
    memset(status, 0, sizeof(*status));
    status->configured = has_text(s_host);
    status->connected = s_connected;
    strlcpy(status->host, s_host, sizeof(status->host));
    status->port = s_port;
}

esp_err_t mqtt_bridge_start(void)
{
    load_mqtt_settings();

    if (!has_text(s_host)) {
        ESP_LOGI(TAG, "MQTT disabled (no broker configured)");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "MQTT configured for broker \"%s:%u\" (client not started yet)",
             s_host, (unsigned)s_port);
    return ESP_OK;
}

void mqtt_bridge_update_lux(float lux)
{
    (void)lux;
}
```

- [ ] **Step 6: Add `mqtt_bridge.c` to the build**

Modify `main/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "main.c" "iot_remote.c" "homekit_accessory.c" "mqtt_bridge.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c freertos esp_event esp_netif esp_wifi esp_http_server esp_hap_apple_profiles esp_hap_core nvs_flash lwip led_strip
)
```

- [ ] **Step 7: Build to verify it compiles**

Run: `idf.py build`
Expected: `Project build complete.` `mqtt_bridge.c` compiles; nothing calls its functions yet, which is fine and expected at this stage.

- [ ] **Step 8: Commit**

```bash
git add main/iot_remote.h main/iot_remote.c main/mqtt_bridge.h main/mqtt_bridge.c main/CMakeLists.txt
git commit -m "feat: add hostname accessor and mqtt_bridge settings skeleton"
```

---

### Task 3: MQTT client connection lifecycle

**Files:**
- Modify: `main/mqtt_bridge.c` (add client init/connect/disconnect handling)
- Modify: `main/CMakeLists.txt` (add `mqtt` to `REQUIRES`)

**Interfaces:**
- Consumes: `iot_remote_get_hostname()` (Task 2); `mqtt_bridge.h`'s `s_host`/`s_port`/`s_username`/`s_password` (already loaded by `load_mqtt_settings()` from Task 2).
- Produces: an internal `s_connected` flag kept accurate via `MQTT_EVENT_CONNECTED`/`MQTT_EVENT_DISCONNECTED`, and an internal `esp_mqtt_client_handle_t s_client` that Task 4 will publish through.

- [ ] **Step 1: Add the MQTT includes and topic buffers**

At the top of `main/mqtt_bridge.c`, replace the includes block with:

```c
#include "mqtt_bridge.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "mqtt_client.h"
#include "nvs.h"

#include "iot_remote.h"
```

Add these static globals right after the existing `s_connected` declaration:

```c
static esp_mqtt_client_handle_t s_client;
static char s_status_topic[80];
static char s_lux_topic[80];
```

- [ ] **Step 2: Add topic construction**

Add this function after `load_mqtt_settings()`:

```c
static void build_topics(void)
{
    const char *hostname = iot_remote_get_hostname();
    snprintf(s_status_topic, sizeof(s_status_topic), "%s/status", hostname);
    snprintf(s_lux_topic, sizeof(s_lux_topic), "%s/lux", hostname);
}
```

- [ ] **Step 3: Add the MQTT event handler**

Add this function after `build_topics()`:

```c
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    (void)event_data;

    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "MQTT connected to broker");
        esp_mqtt_client_publish(s_client, s_status_topic, "online", 0, 1, true);
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected from broker");
        break;
    default:
        break;
    }
}
```

(Task 4 will insert the discovery-config publish into the `MQTT_EVENT_CONNECTED` case.)

- [ ] **Step 4: Add the WiFi-connected handler that starts the client**

Add this function after `mqtt_event_handler()`:

```c
static void wifi_got_ip_handler(void *arg, esp_event_base_t base,
                                 int32_t event_id, void *event_data)
{
    (void)arg;
    (void)base;
    (void)event_id;
    (void)event_data;

    if (s_client != NULL) {
        return;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.hostname = s_host,
        .broker.address.port = s_port,
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        .session.last_will.topic = s_status_topic,
        .session.last_will.msg = "offline",
        .session.last_will.msg_len = 0,
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
    };
    if (has_text(s_username)) {
        mqtt_cfg.credentials.username = s_username;
    }
    if (has_text(s_password)) {
        mqtt_cfg.credentials.authentication.password = s_password;
    }

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize MQTT client");
        return;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
    }
}
```

- [ ] **Step 5: Wire it up in `mqtt_bridge_start()`**

Replace the body of `mqtt_bridge_start()` with:

```c
esp_err_t mqtt_bridge_start(void)
{
    load_mqtt_settings();

    if (!has_text(s_host)) {
        ESP_LOGI(TAG, "MQTT disabled (no broker configured)");
        return ESP_OK;
    }

    build_topics();

    return esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                       wifi_got_ip_handler, NULL);
}
```

- [ ] **Step 6: Add `mqtt` to the build requirements**

Modify `main/CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "main.c" "iot_remote.c" "homekit_accessory.c" "mqtt_bridge.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c freertos esp_event esp_netif esp_wifi esp_http_server esp_hap_apple_profiles esp_hap_core nvs_flash lwip led_strip mqtt
)
```

- [ ] **Step 7: Build to verify it compiles**

Run: `idf.py build`
Expected: `Project build complete.` (still nothing calls `mqtt_bridge_start()` from `main.c` yet — that's Task 5 — so this only validates the module compiles standalone).

- [ ] **Step 8: Commit**

```bash
git add main/mqtt_bridge.c main/CMakeLists.txt
git commit -m "feat: add MQTT client connect/disconnect lifecycle to mqtt_bridge"
```

---

### Task 4: HA discovery payload + lux publishing

**Files:**
- Modify: `main/mqtt_bridge.c`

**Interfaces:**
- Consumes: `s_client`, `s_status_topic`, `s_lux_topic`, `s_connected` (Task 3).
- Produces: `mqtt_bridge_update_lux(float lux)` now actually publishes (signature unchanged from Task 2/`mqtt_bridge.h`).

- [ ] **Step 1: Add the discovery payload builder**

Add this function after `build_topics()` (before `mqtt_event_handler`):

```c
static void publish_discovery_config(void)
{
    const char *hostname = iot_remote_get_hostname();
    char config_topic[112];
    snprintf(config_topic, sizeof(config_topic),
             "homeassistant/sensor/%s/lux/config", hostname);

    char payload[512];
    snprintf(payload, sizeof(payload),
             "{\"name\":\"Illuminance\",\"state_topic\":\"%s\","
             "\"availability_topic\":\"%s\",\"unit_of_measurement\":\"lx\","
             "\"device_class\":\"illuminance\",\"unique_id\":\"%s_lux\","
             "\"device\":{\"identifiers\":[\"%s\"],\"name\":\"%s\","
             "\"manufacturer\":\"AZDelivery\",\"model\":\"BH1750\"}}",
             s_lux_topic, s_status_topic, hostname, hostname, hostname);

    esp_mqtt_client_publish(s_client, config_topic, payload, 0, 1, true);
}
```

- [ ] **Step 2: Call it from the connected event**

In `mqtt_event_handler()`, update the `MQTT_EVENT_CONNECTED` case to:

```c
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "MQTT connected to broker");
        esp_mqtt_client_publish(s_client, s_status_topic, "online", 0, 1, true);
        publish_discovery_config();
        break;
```

- [ ] **Step 3: Implement lux publishing**

Replace the `mqtt_bridge_update_lux` stub with:

```c
void mqtt_bridge_update_lux(float lux)
{
    if (!s_connected || s_client == NULL) {
        return;
    }
    char payload[16] = {};
    snprintf(payload, sizeof(payload), "%.1f", lux);
    esp_mqtt_client_publish(s_client, s_lux_topic, payload, 0, 0, false);
}
```

- [ ] **Step 4: Build to verify it compiles**

Run: `idf.py build`
Expected: `Project build complete.`

- [ ] **Step 5: Commit**

```bash
git add main/mqtt_bridge.c
git commit -m "feat: publish HA discovery config and lux readings over MQTT"
```

---

### Task 5: Wire mqtt_bridge into main.c

**Files:**
- Modify: `main/main.c`

**Interfaces:**
- Consumes: `mqtt_bridge_start(void) -> esp_err_t` and `mqtt_bridge_update_lux(float) -> void` (Task 2/4, `mqtt_bridge.h`).

- [ ] **Step 1: Include the header**

Add to the includes in `main/main.c`, after `#include "homekit_accessory.h"`:

```c
#include "mqtt_bridge.h"
```

- [ ] **Step 2: Start the bridge at boot**

In `app_main()`, immediately after the existing HomeKit start block:

```c
    if (iot_remote_homekit_enabled()) {
        esp_err_t hk_err = homekit_accessory_start();
        if (hk_err != ESP_OK) {
            printf("Failed to start HomeKit: %d\n", hk_err);
        }
    } else {
        printf("HomeKit is disabled in configuration\n");
    }

    esp_err_t mqtt_err = mqtt_bridge_start();
    if (mqtt_err != ESP_OK) {
        printf("Failed to start MQTT bridge: %d\n", mqtt_err);
    }
```

- [ ] **Step 3: Publish lux on every reading**

In the main sensor `while (true)` loop, change:

```c
        if (err == ESP_OK) {
            float lux = (float)lux_x10 / 10.0f;
            printf("Light sensitivity: %.1f lux\n", lux);
            iot_remote_set_lux(lux);
            homekit_accessory_update_lux(lux);
        } else {
```

to:

```c
        if (err == ESP_OK) {
            float lux = (float)lux_x10 / 10.0f;
            printf("Light sensitivity: %.1f lux\n", lux);
            iot_remote_set_lux(lux);
            homekit_accessory_update_lux(lux);
            mqtt_bridge_update_lux(lux);
        } else {
```

- [ ] **Step 4: Build to verify it compiles**

Run: `idf.py build`
Expected: `Project build complete.`

- [ ] **Step 5: Manual verification (requires hardware + a local broker)**

Flash the device (`idf.py flash monitor`), point its Settings page (once Task 6/7 land) or, for now, temporarily hardcode `s_host`/`s_port` is *not* needed — skip full manual verification until Task 8, where the Settings UI exists to configure a broker. This step only confirms the build is clean; do not skip ahead to hardware testing yet.

- [ ] **Step 6: Commit**

```bash
git add main/main.c
git commit -m "feat: start MQTT bridge and publish lux readings from the sensor loop"
```

---

### Task 6: HTTP settings endpoints (GET/POST /mqtt)

**Files:**
- Modify: `main/iot_remote.c`

**Interfaces:**
- Consumes: `mqtt_bridge_get_status(mqtt_bridge_status_t *)`, `mqtt_bridge_save_settings(const char *host, uint16_t port, const char *username, const char *password) -> esp_err_t`, `MQTT_HOST_MAX_LEN`/`MQTT_USERNAME_MAX_LEN`/`MQTT_PASSWORD_MAX_LEN` (all from `mqtt_bridge.h`, Task 2).
- Produces: `GET /mqtt` → `{"ok":true,"configured":bool,"connected":bool,"host":"...","port":N}`; `POST /mqtt` (form fields `host`, `port`, `username`, `password`) → saves settings and reboots. Task 7 (Settings page JS) consumes these two routes.

- [ ] **Step 1: Include the header**

Add to the includes in `main/iot_remote.c`, after `#include "homekit_accessory.h"`:

```c
#include "mqtt_bridge.h"
```

- [ ] **Step 2: Bump the URI handler slot count**

The server currently registers 11 URI handlers (root, health, settings, status, command, wifi GET/POST, wifi/scan, homekit GET/POST, wildcard captive) against a limit of 12. Adding 2 more (`mqtt` GET/POST) requires more headroom. Change:

```c
#define HTTP_URI_HANDLER_SLOTS 12
```

to:

```c
#define HTTP_URI_HANDLER_SLOTS 14
```

- [ ] **Step 3: Add the GET handler**

Add this function after `homekit_post_handler()` (before `start_http_server()`):

```c
static esp_err_t mqtt_get_handler(httpd_req_t *req)
{
    mqtt_bridge_status_t status = {};
    mqtt_bridge_get_status(&status);

    char escaped_host[MQTT_HOST_MAX_LEN * 6 + 1] = {};
    json_escape_string(status.host, escaped_host, sizeof(escaped_host));

    char response[256] = {};
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"configured\":%s,\"connected\":%s,\"host\":\"%s\",\"port\":%u}",
             status.configured ? "true" : "false",
             status.connected ? "true" : "false",
             escaped_host,
             (unsigned)status.port);
    return send_json(req, response);
}
```

- [ ] **Step 4: Add the POST handler**

Add this function after `mqtt_get_handler()`:

```c
static esp_err_t mqtt_post_handler(httpd_req_t *req)
{
    char form[WIFI_FORM_MAX_LEN] = {};
    char host[MQTT_HOST_MAX_LEN + 1] = {};
    char port_value[8] = {};
    char username[MQTT_USERNAME_MAX_LEN + 1] = {};
    char password[MQTT_PASSWORD_MAX_LEN + 1] = {};
    uint16_t port = 1883;

    if (read_form_body(req, form, sizeof(form)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, "{\"ok\":false,\"error\":\"MQTT form is too large\"}");
    }

    form_get_value(form, "host", host, sizeof(host));
    form_get_value(form, "username", username, sizeof(username));
    form_get_value(form, "password", password, sizeof(password));

    if (form_get_value(form, "port", port_value, sizeof(port_value)) && has_text(port_value)) {
        char *end = NULL;
        long parsed = strtol(port_value, &end, 10);
        if (end == port_value || *end != '\0' || parsed < 1 || parsed > 65535) {
            httpd_resp_set_status(req, "400 Bad Request");
            return send_json(req, "{\"ok\":false,\"error\":\"port must be between 1 and 65535\"}");
        }
        port = (uint16_t)parsed;
    }

    esp_err_t err = mqtt_bridge_save_settings(host, port, username, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save MQTT settings: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json(req, "{\"ok\":false,\"error\":\"failed to save MQTT settings\"}");
    }

    ESP_LOGI(TAG, "Saved MQTT settings for host \"%s\"; restarting", host);
    esp_err_t send_err = send_json(req, "{\"ok\":true,\"message\":\"MQTT settings saved; restarting\"}");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return send_err;
}
```

- [ ] **Step 5: Register the routes**

In `start_http_server()`, after the `homekit_uri` declaration and before the `captive_uri` declaration, add:

```c
    const httpd_uri_t mqtt_get_uri = {
        .uri = "/mqtt",
        .method = HTTP_GET,
        .handler = mqtt_get_handler,
    };
    const httpd_uri_t mqtt_post_uri = {
        .uri = "/mqtt",
        .method = HTTP_POST,
        .handler = mqtt_post_handler,
    };
```

And after the `homekit_uri` registration line and before the `captive_uri` registration line, add:

```c
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &mqtt_get_uri), TAG, "failed to register GET /mqtt");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &mqtt_post_uri), TAG, "failed to register POST /mqtt");
```

- [ ] **Step 6: Build to verify it compiles**

Run: `idf.py build`
Expected: `Project build complete.`

- [ ] **Step 7: Manual verification (requires flashed hardware)**

Flash the device and, once it's on WiFi, run:

```bash
curl http://<device-ip>/mqtt
```
Expected: `{"ok":true,"configured":false,"connected":false,"host":"","port":1883}`

```bash
curl -X POST http://<device-ip>/mqtt -d 'host=192.168.1.50&port=1883&username=&password='
```
Expected: `{"ok":true,"message":"MQTT settings saved; restarting"}`, then the device reboots and (per Task 3's `wifi_got_ip_handler`) attempts to connect to `192.168.1.50:1883` once WiFi reconnects.

- [ ] **Step 8: Commit**

```bash
git add main/iot_remote.c
git commit -m "feat: add GET/POST /mqtt HTTP endpoints for broker configuration"
```

---

### Task 7: Settings page UI section

**Files:**
- Modify: `main/iot_remote.c` (the `s_settings_page` HTML/JS string constant)

**Interfaces:**
- Consumes: `GET /mqtt` and `POST /mqtt` (Task 6).
- Produces: none (leaf UI task).

- [ ] **Step 1: Add the HTML section**

In `main/iot_remote.c`, inside the `s_settings_page` string constant, insert a new section between the closing `</section>` of the HomeKit `<section>` block and the `<section><h2>Device</h2>` block:

```c
    "<section><div class=cardHead><h2>Home Assistant / MQTT</h2></div>"
    "<div id=mqttStatus style='margin-bottom:15px'></div>"
    "<form id=mqttForm><div class=grid>"
    "<label>Broker Host<input id=mqttHost name=host maxlength=63></label>"
    "<label>Broker Port<input id=mqttPort name=port type=number min=1 max=65535 placeholder=1883></label>"
    "<label>Username<input id=mqttUsername name=username maxlength=32></label>"
    "<label>Password<input id=mqttPassword name=password maxlength=64 type=password></label></div>"
    "<div class=row><button>Save MQTT</button></div></form></section>"
```

- [ ] **Step 2: Add the JS load/save functions**

In the same string constant, inside the `<script>` block, add these two functions right after `loadHomeKit()`'s closing brace and before the `$('scanResults').onclick=...` line:

```c
    "async function loadMqtt(){"
    "try{let r=await fetch('/mqtt');let j=await r.json();if(j.ok){"
    "$('mqttHost').value=j.host||'';$('mqttPort').value=j.configured?j.port:'';"
    "$('mqttStatus').innerHTML=`<div class=metric>Status<b>${j.configured?(j.connected?'Connected':'Disconnected'):'Not configured'}</b></div>`;"
    "}}catch(e){}"
    "}"
    "$('mqttForm').onsubmit=async e=>{"
    "e.preventDefault();let j=await post('/mqtt','host='+encodeURIComponent($('mqttHost').value)+'&port='+encodeURIComponent($('mqttPort').value)+'&username='+encodeURIComponent($('mqttUsername').value)+'&password='+encodeURIComponent($('mqttPassword').value));"
    "if(j.ok){showToast('MQTT settings saved. Rebooting...');setTimeout(rebootDeviceSilent,1000);}"
    "else{showToast('Error: '+(j.error||j.message));}"
    "};"
```

**Do not escape the `$` before `${`.** This is a plain `$=id=>...` C string that must emit a JavaScript template literal — the earlier bug fixed in this codebase (`iot_remote.c`'s `loadWifi`/`loadHomeKit`) was exactly a stray `\\$` turning into a literal `\$` in the browser and breaking interpolation. Keep `${...}` unescaped, as written above.

- [ ] **Step 3: Call `loadMqtt()` at page init**

Change the final init line in the script from:

```c
    "loadWifi();loadScan();loadHomeKit();"
```

to:

```c
    "loadWifi();loadScan();loadHomeKit();loadMqtt();"
```

- [ ] **Step 4: Build to verify it compiles**

Run: `idf.py build`
Expected: `Project build complete.`

- [ ] **Step 5: Manual verification (requires flashed hardware)**

Flash the device, open `http://<device-ip>/settings` in a browser, confirm:
- A "Home Assistant / MQTT" section appears with Host/Port/Username/Password fields and a status line reading "Not configured" (fresh device).
- Entering a broker host and clicking "Save MQTT" shows the "Saved... Rebooting..." toast and the device restarts.
- After reboot, reloading `/settings` shows the saved host/port and an updated status (`Disconnected` or `Connected` depending on broker reachability).

- [ ] **Step 6: Commit**

```bash
git add main/iot_remote.c
git commit -m "feat: add Home Assistant/MQTT section to the settings page"
```

---

### Task 8: Full hardware verification

**Files:** none (verification only, no code changes expected).

**Interfaces:** none.

- [ ] **Step 1: Build and flash**

```bash
idf.py build flash monitor
```
Expected: builds clean, flashes, and the serial log shows normal boot (WiFi connect, HTTP server start).

- [ ] **Step 2: Confirm MQTT stays quiet during captive-portal/AP-only mode**

If the device has no saved WiFi credentials yet, confirm the serial log shows the captive-portal AP starting and does **not** show any MQTT connection attempt (because `wifi_got_ip_handler` only fires on `IP_EVENT_STA_GOT_IP`, which only happens once STA has an IP).

- [ ] **Step 3: Verify against a local Mosquitto broker**

Start a local broker (e.g. `brew services start mosquitto` or `docker run -p 1883:1883 eclipse-mosquitto`). Subscribe to everything:

```bash
mosquitto_sub -h <broker-ip> -t '#' -v
```

Via the device's `/settings` page, save that broker's host/port (leave username/password blank for an open broker). After the reboot, confirm `mosquitto_sub` shows:
- A retained `homeassistant/sensor/<hostname>/lux/config` message with the JSON payload from Task 4, with `"manufacturer":"AZDelivery"` and `"model":"BH1750"`.
- A retained `<hostname>/status` message of `online`.
- Periodic `<hostname>/lux` messages (roughly every 5s) with a numeric value matching the serial log's printed lux reading.

- [ ] **Step 4: Verify in a real Home Assistant instance**

Add the same broker to Home Assistant's MQTT integration (or confirm it's already configured, if HA is the broker host itself via the Mosquitto add-on). Confirm a `sensor.<hostname>_illuminance`-style entity auto-appears with device manufacturer `AZDelivery` and model `BH1750`, and that its value updates roughly every 5 seconds while HA shows the device as available.

- [ ] **Step 5: Verify the disable path**

From `/settings`, clear the Broker Host field and save. Confirm the device reboots, `GET /mqtt` now returns `"configured":false`, and `mosquitto_sub` shows the retained `<hostname>/status` topic's last value going stale with no new publishes (LWT `offline` should also appear if the broker detects the now-absent client, though this may take the broker's keepalive timeout to trigger — note the delay rather than treating it as a failure).

- [ ] **Step 6: Confirm HomeKit still works unaffected**

Since Task 1 changed HomeKit's accessory metadata, re-pair or re-check the device in the Home app: confirm the accessory info now shows manufacturer "AZDelivery" and model "BH1750", and that the lux reading still updates normally.

No commit for this task — it's verification only. If any step fails, file it as a bug against the specific task above whose code is responsible, fix there, and re-run this task's checklist from the top.
