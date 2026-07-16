# MQTT / Home Assistant Integration — Design

Date: 2026-07-15

## Goal

Expose the BH1750 illuminance reading to Home Assistant via MQTT, using Home
Assistant's MQTT Discovery convention so the sensor entity appears
automatically with no YAML configuration required on the HA side.

## Non-goals

- No TLS/`mqtts://` support in this iteration (plain `mqtt://` only).
- No MQTT-controlled entities beyond the lux sensor (no reboot button, no
  HomeKit toggle over MQTT). HomeKit and the captive-portal settings page
  remain the only controls for those.
- No explicit MQTT on/off toggle in the UI — MQTT is enabled simply by saving
  a non-empty broker host, and disabled by clearing it (mirrors how an empty
  WiFi SSID puts the device into captive-portal mode).
- No broader refactor of `iot_remote.c` (WiFi/AP lifecycle, captive portal
  HTTP server, DNS, settings persistence) in this change. That modularization
  (splitting into e.g. `wifi_manager.c/h`, `captive_portal.c/h`,
  `device_settings.c/h`) is a separate follow-up project with its own design.

## Architecture

New module: `main/mqtt_bridge.c` / `main/mqtt_bridge.h`, added to
`main/CMakeLists.txt`'s `SRCS`, with `mqtt` added to `REQUIRES` (ESP-IDF's
built-in `esp-mqtt` component — no new managed dependency needed).

Public API, mirroring the existing `homekit_accessory.h` shape:

```c
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

Buffer size constants (`MQTT_HOST_MAX_LEN`, `MQTT_USERNAME_MAX_LEN`,
`MQTT_PASSWORD_MAX_LEN`) are defined in `mqtt_bridge.h` so `iot_remote.c` can
size its own form-parsing buffers from the same source of truth that owns
storage.

**Ownership:** `mqtt_bridge.c` owns everything MQTT-specific — the
`esp_mqtt_client` handle, its own `mqtt_cfg` NVS namespace, connect/reconnect
handling, and the HA discovery payload. It registers its own
`IP_EVENT_STA_GOT_IP` handler inside `mqtt_bridge_start()` (ESP-IDF's event
loop supports multiple independent subscribers to the same event, so this
does not interfere with `iot_remote.c`'s existing WiFi event handler). This
means MQTT only attempts a connection once the station actually has an IP,
and never while the device is in captive-portal/AP-only setup mode.

`main.c` changes:
- Call `mqtt_bridge_start()` once at boot, unconditionally (it's a no-op if
  no broker host is configured).
- Call `mqtt_bridge_update_lux(lux)` next to the existing
  `iot_remote_set_lux(lux)` / `homekit_accessory_update_lux(lux)` calls in the
  sensor loop.

`iot_remote.c` changes:
- Two new HTTP handlers, `GET /mqtt` and `POST /mqtt`, following the exact
  shape of the existing `wifi_get_handler`/`homekit_get_handler` pair —
  parsing/validation lives in `iot_remote.c`, business logic delegates to
  `mqtt_bridge`'s public API.
- A new "Home Assistant / MQTT" section in the settings page HTML/JS,
  styled like the existing HomeKit card.

## Topics & Discovery Payload

Uses the existing user-editable `hostname` device setting as the identifier,
avoiding introducing a second "device name" concept.

| Purpose | Topic | Payload | Retained |
|---|---|---|---|
| Availability (LWT) | `<hostname>/status` | `online` / `offline` | yes |
| Lux state | `<hostname>/lux` | e.g. `123.4` | no |
| HA discovery config | `homeassistant/sensor/<hostname>/lux/config` | JSON below | yes |

Discovery JSON, published once immediately after `MQTT_EVENT_CONNECTED`:

```json
{
  "name": "Illuminance",
  "state_topic": "<hostname>/lux",
  "availability_topic": "<hostname>/status",
  "unit_of_measurement": "lx",
  "device_class": "illuminance",
  "unique_id": "<hostname>_lux",
  "device": {
    "identifiers": ["<hostname>"],
    "name": "<hostname>",
    "manufacturer": "AZDelivery",
    "model": "BH1750"
  }
}
```

## Lifecycle

1. `mqtt_bridge_start()` loads the `mqtt_cfg` NVS namespace (host, port,
   username, password). An empty host logs a message and returns `ESP_OK`
   without creating an MQTT client — this is the "disabled" state.
2. Otherwise, it registers its `IP_EVENT_STA_GOT_IP` handler and configures
   `esp_mqtt_client` with the LWT set on `<hostname>/status` (`offline`,
   retained, QoS 1).
3. On `MQTT_EVENT_CONNECTED`: publish `online` (retained) to the status
   topic, then publish the discovery config (retained) once.
4. `mqtt_bridge_update_lux(lux)`, called roughly every 5s from the sensor
   loop, publishes to `<hostname>/lux` only while an internal "connected"
   flag (set/cleared on `MQTT_EVENT_CONNECTED`/`MQTT_EVENT_DISCONNECTED`) is
   true. This avoids unbounded queuing while offline; esp-mqtt's built-in
   reconnect-with-backoff handles the rest — no custom retry logic needed.

## Settings UI & HTTP API

New "Home Assistant / MQTT" section on the settings page:

- **Status** (read-only): `Not configured` / `Disconnected` / `Connected`,
  from `GET /mqtt`.
- **Broker Host** (text input, required to enable).
- **Broker Port** (number input, default `1883`).
- **Username** / **Password** (optional; password is write-only and never
  echoed back by `GET /mqtt`, matching the existing WiFi password field).
- **Save MQTT** button → `POST /mqtt` → `mqtt_bridge_save_settings()` →
  device reboots (identical pattern to Save Hostname / Save Wi-Fi / Save
  HomeKit).

`GET /mqtt` response: `{"ok":true,"configured":bool,"connected":bool,"host":"...","port":1883}`

`POST /mqtt` form fields: `host`, `port`, `username`, `password`. An empty or
missing `port` field defaults to `1883`; if present, it is validated as
numeric, 1–65535 (same `strtol` pattern as the existing WiFi slot-delete
validator).

## Error Handling

- Empty host → not an error; `configured:false` in `GET /mqtt`.
- Non-numeric or out-of-range port → `400 Bad Request`,
  `{"ok":false,"error":"port must be between 1 and 65535"}`.
- NVS write failure → `500 Internal Server Error`, matching the existing
  WiFi/HomeKit/hostname handlers.
- Broker-level connect/auth failures are not surfaced as HTTP errors (the
  settings save already succeeded and the device already rebooted) — they
  are logged via `MQTT_EVENT_ERROR` and visible through `GET /mqtt`'s
  `connected:false`. esp-mqtt retries automatically with backoff.
- Password is write-only, consistent with the existing WiFi password field.

## Testing / Verification

This project has no unit test infrastructure (pure ESP-IDF firmware). This
change is verified by build + manual hardware checks:

1. `idf.py build` succeeds with the new component.
2. Flash the device; confirm the serial log shows MQTT connecting once WiFi
   is up, and confirm it does *not* attempt to connect while in
   captive-portal/AP-only mode.
3. Point it at a local Mosquitto broker; use `mosquitto_sub -t '#' -v` to
   confirm the retained discovery config, retained `online`/`offline` on
   connect/disconnect, and periodic `lux` updates.
4. Add the broker to a real Home Assistant instance and confirm the
   illuminance sensor entity auto-appears via MQTT Discovery with the
   correct manufacturer (`AZDelivery`) and model (`BH1750`).
5. Toggle the broker host on/off from the Settings page and confirm the
   device reboots and MQTT starts/stops accordingly.

## Follow-up (out of scope for this spec)

Modularize `iot_remote.c`'s connectivity responsibilities into separate
files, e.g.:
- `wifi_manager.c/h` — STA/AP lifecycle
- `captive_portal.c/h` — HTTP server + DNS + pages
- `device_settings.c/h` — NVS-backed persistence (hostname, HomeKit enabled,
  WiFi credentials, and eventually MQTT broker settings)

This is a separate project with its own design/plan, to be done after MQTT
ships.
