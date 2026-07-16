#include "iot_remote.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "homekit_accessory.h"

static const char *TAG = "iot_remote";

#define COMMAND_MAX_LEN 96
#define RESPONSE_MAX_LEN 640
#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASSWORD_MAX_LEN 64
#define WIFI_FORM_MAX_LEN 384
#define WIFI_CREDENTIAL_SLOTS 3
#define DEVICE_HOSTNAME_MAX_LEN 32
#define DEVICE_HOSTNAME_DEFAULT "esp-bh1750"
#define HTTP_URI_HANDLER_SLOTS 12
#define DNS_PORT 53
#define WIFI_SCAN_MAX_APS 20
#define DNS_PACKET_MAX_LEN 512

typedef struct {
    char ssid[WIFI_SSID_MAX_LEN + 1];
    char password[WIFI_PASSWORD_MAX_LEN + 1];
} wifi_credential_t;

static iot_remote_config_t s_config;
static httpd_handle_t s_httpd;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static char s_device_hostname[DEVICE_HOSTNAME_MAX_LEN + 1] = DEVICE_HOSTNAME_DEFAULT;
static wifi_credential_t s_wifi_credentials[WIFI_CREDENTIAL_SLOTS];
static size_t s_wifi_credential_count;
static size_t s_current_wifi_credential;
static bool s_ap_started;
static bool s_sta_connected;
static int s_sta_retries;
static TaskHandle_t s_dns_task;
static float s_current_lux = 0.0f;

typedef struct {
    char ssid[WIFI_SSID_MAX_LEN + 1];
    int8_t rssi;
    bool secure;
} wifi_scan_result_t;

static wifi_scan_result_t s_scan_results[WIFI_SCAN_MAX_APS];
static size_t s_scan_result_count;

static esp_err_t start_provisioning_ap(void);
static esp_err_t start_http_server(void);
static bool load_homekit_enabled_setting(void);

static const char s_control_page[] =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>ESP-BH1750 Dashboard</title><style>"
    "body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;margin:0;background:linear-gradient(135deg,#0f172a,#1e1b4b);color:#f8fafc;min-height:100vh;display:flex;flex-direction:column;justify-content:center;align-items:center}"
    "main{width:90%;max-width:500px;background:rgba(30,41,59,0.7);backdrop-filter:blur(12px);-webkit-backdrop-filter:blur(12px);border:1px solid rgba(255,255,255,0.1);border-radius:24px;padding:30px;box-shadow:0 20px 40px rgba(0,0,0,0.4);text-align:center;box-sizing:border-box}"
    "header{display:flex;justify-content:space-between;align-items:center;margin-bottom:30px;border-bottom:1px solid rgba(255,255,255,0.1);padding-bottom:15px}"
    "h1{font-size:20px;margin:0;background:linear-gradient(to right,#38bdf8,#818cf8);-webkit-background-clip:text;-webkit-text-fill-color:transparent}"
    "a{color:#38bdf8;text-decoration:none;font-size:14px;font-weight:500;transition:color 0.2s}a:hover{color:#818cf8}"
    ".lux-container{margin:40px 0;position:relative;display:inline-flex;flex-direction:column;align-items:center;justify-content:center}"
    ".lux-value{font-size:72px;font-weight:800;line-height:1;background:radial-gradient(circle,#fef08a 0%,#eab308 100%);-webkit-background-clip:text;-webkit-text-fill-color:transparent;text-shadow:0 0 40px rgba(234,179,8,0.2);transition:all 0.3s ease}"
    ".lux-label{font-size:16px;color:#94a3b8;margin-top:10px;text-transform:uppercase;letter-spacing:2px}"
    ".status-card{background:rgba(15, 23, 42, 0.4);border-radius:16px;padding:18px;margin-top:20px;display:grid;grid-template-columns:1fr 1fr;gap:12px;text-align:left;font-size:13px}"
    ".metric{display:flex;flex-direction:column;gap:4px}.metric span{color:#64748b;font-size:11px;text-transform:uppercase;letter-spacing:0.5px}.metric b{color:#e2e8f0;font-weight:600}"
    "</style></head><body><main><header><h1 id=hostTitle>ESP-BH1750</h1><a href=/settings>Settings</a></header>"
    "<div class=lux-container><div class=lux-value id=luxVal>--</div><div class=lux-label>Ambient Light</div></div>"
    "<div class=status-card><div class=metric><span>WiFi Status</span><b id=wifiStatus>Connecting...</b></div>"
    "<div class=metric><span>HomeKit</span><b id=hkStatus>--</b></div></div></main><script>"
    "const $=id=>document.getElementById(id);"
    "async function updateStatus(){"
    "try{let r=await fetch('/status');let j=await r.json();if(j.ok){"
    "$('luxVal').textContent=typeof j.lux==='number'?j.lux.toFixed(1)+' lx':'--';"
    "$('wifiStatus').textContent=j.wifi_connected?'Connected':'Disconnected';"
    "if(j.hostname){$('hostTitle').textContent=j.hostname;document.title=j.hostname;}"
    "}}catch(e){}"
    "try{let r=await fetch('/homekit');let j=await r.json();if(j.ok){"
    "$('hkStatus').textContent=j.started?(j.connected_controller_count>0?'Connected':'Ready'):'Off';"
    "}}catch(e){}"
    "}"
    "setInterval(updateStatus,3000);updateStatus();"
    "</script></body></html>";

static const char s_settings_page[] =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Settings</title><style>"
    "body{font-family:system-ui,-apple-system,Segoe UI,sans-serif;margin:0;background:linear-gradient(135deg,#0f172a,#1e1b4b);color:#f8fafc;min-height:100vh;display:flex;flex-direction:column;align-items:center;padding:20px}"
    "main{width:100%;max-width:500px;background:rgba(30,41,59,0.7);backdrop-filter:blur(12px);-webkit-backdrop-filter:blur(12px);border:1px solid rgba(255,255,255,0.1);border-radius:24px;padding:30px;box-shadow:0 20px 40px rgba(0,0,0,0.4);box-sizing:border-box}"
    "header{display:flex;align-items:center;justify-content:space-between;margin-bottom:30px;border-bottom:1px solid rgba(255,255,255,0.1);padding-bottom:15px}"
    "h1{font-size:20px;margin:0;background:linear-gradient(to right,#38bdf8,#818cf8);-webkit-background-clip:text;-webkit-text-fill-color:transparent}"
    "h2{font-size:16px;margin:0 0 15px 0;color:#f1f5f9}a{color:#38bdf8;text-decoration:none;font-size:14px;font-weight:500}"
    "section{background:rgba(15,23,42,0.3);border:1px solid rgba(255,255,255,0.05);border-radius:16px;padding:20px;margin-bottom:20px}"
    ".grid{display:grid;gap:12px}label{display:grid;gap:6px;color:#94a3b8;font-size:13px}"
    "input,select{font:inherit;padding:10px;border:1px solid rgba(255,255,255,0.1);border-radius:8px;background:rgba(15,23,42,0.5);color:#f8fafc;box-sizing:border-box;width:100%}"
    "button{font:inherit;padding:10px 16px;border:0;border-radius:8px;background:#3b82f6;color:white;cursor:pointer;font-weight:500;transition:background 0.2s}"
    "button:hover{background:#2563eb}button.secondary{background:#475569}button.secondary:hover{background:#334155}"
    ".row{display:flex;gap:8px;margin-top:14px}.cardHead{display:flex;justify-content:space-between;align-items:center}"
    ".toggle{display:flex;gap:4px;background:rgba(15,23,42,0.5);padding:3px;border-radius:8px;border:1px solid rgba(255,255,255,0.1)}"
    ".toggle button{padding:6px 12px;background:transparent;color:#94a3b8;border-radius:6px}.toggle button.active{background:#3b82f6;color:white}"
    ".wifiSummary, .homekitSummary{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:15px}"
    ".metric{background:rgba(15,23,42,0.4);border-radius:8px;padding:10px;font-size:12px;color:#94a3b8}.metric b{display:block;margin-top:4px;color:#f8fafc}"
    ".slot{display:flex;justify-content:space-between;align-items:center;padding:8px 0;border-top:1px solid rgba(255,255,255,0.05)}.slot:first-of-type{border-top:0}"
    ".scanItem{cursor:pointer;padding:10px;border-radius:8px;transition:background 0.15s}.scanItem:hover{background:rgba(56,189,248,0.08)}"
    ".toast{position:fixed;left:50%;bottom:20px;transform:translate(-50%,20px);opacity:0;background:#10b981;color:white;border-radius:8px;padding:10px 16px;box-shadow:0 10px 25px rgba(0,0,0,0.3);pointer-events:none;transition:opacity 0.2s,transform 0.2s;font-size:14px}"
    ".toast.show{opacity:1;transform:translate(-50%,0)}"
    "</style></head><body><main><header><h1>Settings</h1><a href=/>Dashboard</a></header>"
    "<section><h2>Wi-Fi Status</h2><div id=wifiStatus></div>"
    "<form id=hostnameForm><div class=grid><label>Hostname<input id=hostname name=hostname maxlength=32 required></label></div>"
    "<div class=row><button>Save Hostname</button></div></form>"
    "<h2 style='margin-top:20px'>Saved Wi-Fi Networks</h2><div id=wifiSlots></div>"
    "<h2 style='margin-top:20px'>Nearby Networks</h2><div id=scanResults><div class=slot>Scanning...</div></div>"
    "<div class=row style='margin-top:10px'><button class=secondary type=button onclick=rescan()>Rescan</button></div>"
    "<h2 style='margin-top:20px'>Add Wi-Fi Network</h2><form id=wifiForm><div class=grid>"
    "<label>SSID<input id=ssid name=ssid maxlength=32 required></label>"
    "<label>Password<input id=password name=password maxlength=64 type=password></label></div>"
    "<div class=row><button>Save Wi-Fi</button></div></form></section>"
    "<section><div class=cardHead><h2>HomeKit</h2><div class=toggle id=homekitToggle><button type=button data-v=1>On</button><button type=button data-v=0>Off</button></div></div>"
    "<div id=homekitDetails style='margin-top:15px'></div><div class=row><button type=button onclick=saveHomeKit()>Save HomeKit</button></div>"
    "<p id=homekitStatus style='font-size:12px;color:#10b981;margin:8px 0 0 0'></p></section>"
    "<section><h2>Device</h2><div class=row><button class=secondary type=button onclick=rebootDevice()>Reboot</button></div></section></main>"
    "<div id=toast class=toast>Saved successfully</div><script>"
    "const $=id=>document.getElementById(id);"
    "async function post(path,body,type='application/x-www-form-urlencoded'){"
    "let r=await fetch(path,{method:'POST',headers:{'Content-Type':type},body});"
    "let t=await r.text();let j;try{j=JSON.parse(t)}catch(e){j={ok:r.ok,message:t}}if(!r.ok)j.ok=false;return j;"
    "}"
    "let toastTimer=0;function showToast(msg){"
    "let t=$('toast');t.textContent=msg||'Saved successfully';t.classList.add('show');"
    "clearTimeout(toastTimer);toastTimer=setTimeout(()=>t.classList.remove('show'),2000);"
    "}"
    "async function saveHomeKit(){"
    "let val=document.querySelector('#homekitToggle button.active').dataset.v;"
    "let j=await post('/homekit','enabled='+encodeURIComponent(val));"
    "if(j.ok){showToast('HomeKit configuration saved. Rebooting...');setTimeout(rebootDeviceSilent,1000);}"
    "else{showToast('Error: '+(j.error||j.message));}"
    "}"
    "async function rebootDevice(){if(!confirm('Reboot device now?'))return;rebootDeviceSilent();}"
    "async function rebootDeviceSilent(){await post('/command','reboot','text/plain');}"
    "$('homekitToggle').onclick=e=>{"
    "if(e.target.dataset.v){document.querySelectorAll('#homekitToggle button').forEach(b=>b.classList.toggle('active',b===e.target));}"
    "};"
    "$('hostnameForm').onsubmit=async e=>{"
    "e.preventDefault();let j=await post('/wifi','action=hostname&hostname='+encodeURIComponent($('hostname').value));"
    "if(j.ok){showToast('Hostname saved. Rebooting...');setTimeout(rebootDeviceSilent,1000);}"
    "else{showToast('Error: '+(j.error||j.message));}"
    "};"
    "$('wifiForm').onsubmit=async e=>{"
    "e.preventDefault();let j=await post('/wifi','ssid='+encodeURIComponent($('ssid').value)+'&password='+encodeURIComponent($('password').value));"
    "if(j.ok){showToast('WiFi credentials saved. Rebooting...');setTimeout(rebootDeviceSilent,1000);}"
    "else{showToast('Error: '+(j.error||j.message));}"
    "};"
    "let wifiList=[];"
    "async function loadWifi(){"
    "try{let r=await fetch('/wifi');let j=await r.json();if(j.ok){"
    "$('hostname').value=j.hostname||'';$('wifiStatus').innerHTML=`<div class=wifiSummary><div class=metric>Network<b>${j.connected?j.ssid:'Disconnected'}</b></div><div class=metric>IP Address<b>${j.ip||'--'}</b></div></div>`;"
    "wifiList=j.slots||[];"
    "$('wifiSlots').innerHTML=wifiList.length?wifiList.map((x,i)=>`<div class=slot><span class=slotName>${x.ssid}</span><button class=secondary type=button onclick=deleteWifi(${i})>Delete</button></div>`).join(''):'<div class=slot>No saved networks</div>';"
    "}}catch(e){}"
    "}"
    "async function deleteWifi(i){"
    "if(!confirm('Delete this Wi-Fi network?'))return;"
    "let j=await post('/wifi','action=delete&slot='+i);"
    "if(j.ok){showToast('WiFi network deleted');loadWifi();}"
    "else{showToast('Error: '+(j.error||j.message));}"
    "}"
    "function homeKitStatus(j){"
    "if(!j.available)return 'Not compiled';"
    "if(!j.enabled)return 'Off';"
    "if(!j.started)return 'Starting...';"
    "if(j.pairing)return 'Pairing...';"
    "if(j.connected_controller_count>0)return 'Connected';"
    "if(j.paired_controller_count>0)return 'Paired';"
    "if(j.pairing_timed_out)return 'Pairing timed out';"
    "return 'Ready to pair';"
    "}"
    "async function loadHomeKit(){"
    "try{let r=await fetch('/homekit');let j=await r.json();if(j.ok){"
    "document.querySelectorAll('#homekitToggle button').forEach(b=>b.classList.toggle('active',b.dataset.v===(j.enabled?'1':'0')));"
    "if(j.enabled){$('homekitDetails').innerHTML=`<div class=homekitSummary><div class=metric>Status<b>${homeKitStatus(j)}</b></div><div class=metric>Pairing Code<b style=font-family:monospace>${j.setup_code||'--'}</b></div></div>`;}"
    "else{$('homekitDetails').innerHTML='';}"
    "}}catch(e){}"
    "}"
    "$('scanResults').onclick=e=>{"
    "let el=e.target.closest('[data-ssid]');"
    "if(el){$('ssid').value=el.dataset.ssid;$('password').value='';$('password').focus();}"
    "};"
    "async function loadScan(){"
    "$('scanResults').innerHTML='<div class=slot>Scanning...</div>';"
    "try{let r=await fetch('/wifi/scan');let j=await r.json();if(j.ok){"
    "let c=$('scanResults');c.innerHTML='';"
    "if(!j.networks||!j.networks.length){c.innerHTML='<div class=slot>No networks found</div>';return;}"
    "j.networks.forEach(n=>{"
    "let d=document.createElement('div');d.className='slot scanItem';d.dataset.ssid=n.ssid;"
    "let s=document.createElement('span');s.className='slotName';s.textContent=n.ssid;"
    "let r=document.createElement('span');r.style.cssText='color:#64748b;font-size:12px';"
    "r.textContent=n.rssi+' dBm \\xb7 '+(n.secure?'Secured':'Open');"
    "d.append(s,r);c.append(d);"
    "});}}catch(e){$('scanResults').innerHTML='<div class=slot>Scan failed</div>';}"
    "}"
    "function rescan(){loadScan();}"
    "loadWifi();loadScan();loadHomeKit();"
    "</script></body></html>";

static bool has_text(const char *value)
{
    return value != NULL && value[0] != '\0';
}

static void url_decode(char *value)
{
    char *read = value;
    char *write = value;

    while (*read != '\0') {
        if (*read == '+') {
            *write++ = ' ';
            read++;
        } else if (*read == '%' && isxdigit((unsigned char)read[1]) && isxdigit((unsigned char)read[2])) {
            char hex[3] = { read[1], read[2], '\0' };
            *write++ = (char)strtol(hex, NULL, 16);
            read += 3;
        } else {
            *write++ = *read++;
        }
    }
    *write = '\0';
}

static bool form_get_value(const char *form, const char *key, char *out, size_t out_size)
{
    if (form == NULL || key == NULL || out == NULL || out_size == 0) {
        return false;
    }

    size_t key_len = strlen(key);
    const char *p = form;

    while (p != NULL) {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *val = p + key_len + 1;
            const char *end = strchr(val, '&');
            size_t val_len = end ? (size_t)(end - val) : strlen(val);

            if (val_len >= out_size) {
                return false;
            }

            memcpy(out, val, val_len);
            out[val_len] = '\0';
            url_decode(out);
            return true;
        }

        p = strchr(p, '&');
        if (p != NULL) {
            p++;
        }
    }

    return false;
}

static esp_err_t read_form_body(httpd_req_t *req, char *form, size_t form_size)
{
    size_t remaining = req->content_len;
    size_t offset = 0;

    if (remaining == 0 || remaining >= form_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    while (remaining > 0) {
        int received = httpd_req_recv(req, form + offset, remaining);
        if (received <= 0) {
            return ESP_FAIL;
        }
        offset += (size_t)received;
        remaining -= (size_t)received;
    }
    form[offset] = '\0';
    return ESP_OK;
}

static void json_escape_string(const char *value, char *out, size_t out_size)
{
    size_t offset = 0;
    for (const unsigned char *ch = (const unsigned char *)value;
         *ch != '\0' && offset + 1 < out_size; ++ch) {
        if (*ch == '"' || *ch == '\\') {
            if (offset + 2 >= out_size) {
                break;
            }
            out[offset++] = '\\';
            out[offset++] = (char)*ch;
        } else if (*ch < 0x20) {
            if (offset + 6 >= out_size) {
                break;
            }
            offset += snprintf(out + offset, out_size - offset, "\\u%04x", *ch);
        } else {
            out[offset++] = (char)*ch;
        }
    }
    out[offset] = '\0';
}

static bool normalize_hostname(const char *input, char *out, size_t out_size)
{
    size_t len = 0;

    if (input == NULL || out_size == 0) {
        return false;
    }

    while (isspace((unsigned char)*input)) {
        ++input;
    }

    for (const unsigned char *ch = (const unsigned char *)input;
         *ch != '\0' && !isspace(*ch); ++ch) {
        if (len + 1 >= out_size || len >= DEVICE_HOSTNAME_MAX_LEN) {
            return false;
        }
        if (isalnum(*ch)) {
            out[len++] = (char)tolower(*ch);
        } else if (*ch == '-') {
            out[len++] = '-';
        } else {
            return false;
        }
    }

    out[len] = '\0';
    return len > 0 && out[0] != '-' && out[len - 1] != '-';
}

static void load_device_hostname_setting(void)
{
    char hostname[DEVICE_HOSTNAME_MAX_LEN + 1] = DEVICE_HOSTNAME_DEFAULT;
    nvs_handle_t nvs = 0;

    strlcpy(s_device_hostname, DEVICE_HOSTNAME_DEFAULT, sizeof(s_device_hostname));
    if (nvs_open("app_cfg", NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }

    size_t len = sizeof(hostname);
    if (nvs_get_str(nvs, "hostname", hostname, &len) == ESP_OK) {
        char normalized[DEVICE_HOSTNAME_MAX_LEN + 1] = {};
        if (normalize_hostname(hostname, normalized, sizeof(normalized))) {
            strlcpy(s_device_hostname, normalized, sizeof(s_device_hostname));
        }
    }
    nvs_close(nvs);
}

static esp_err_t save_device_hostname_setting(const char *hostname)
{
    char normalized[DEVICE_HOSTNAME_MAX_LEN + 1] = {};
    nvs_handle_t nvs = 0;

    if (!normalize_hostname(hostname, normalized, sizeof(normalized))) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(nvs_open("app_cfg", NVS_READWRITE, &nvs),
                        TAG, "failed to open app settings NVS namespace");

    esp_err_t err = nvs_set_str(nvs, "hostname", normalized);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    if (err == ESP_OK) {
        strlcpy(s_device_hostname, normalized, sizeof(s_device_hostname));
    }
    nvs_close(nvs);
    return err;
}

static bool load_homekit_enabled_setting(void)
{
    bool enabled = CONFIG_ESP_BH1750_HOMEKIT_ENABLE;
    nvs_handle_t nvs = 0;
    if (nvs_open("app_cfg", NVS_READONLY, &nvs) != ESP_OK) {
        return enabled;
    }

    uint8_t saved = enabled ? 1 : 0;
    if (nvs_get_u8(nvs, "hk_en", &saved) == ESP_OK) {
        enabled = saved != 0;
    }
    nvs_close(nvs);
    return enabled;
}

static esp_err_t save_homekit_enabled_setting(bool enabled)
{
    nvs_handle_t nvs = 0;
    ESP_RETURN_ON_ERROR(nvs_open("app_cfg", NVS_READWRITE, &nvs),
                        TAG, "failed to open app settings NVS namespace");

    esp_err_t err = nvs_set_u8(nvs, "hk_en", enabled ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

bool iot_remote_homekit_enabled(void)
{
    return load_homekit_enabled_setting();
}

static bool read_wifi_slot(nvs_handle_t nvs, uint8_t slot, wifi_credential_t *credential)
{
    char ssid_key[8] = {};
    char password_key[8] = {};
    snprintf(ssid_key, sizeof(ssid_key), "ssid%u", (unsigned)slot);
    snprintf(password_key, sizeof(password_key), "pass%u", (unsigned)slot);

    size_t ssid_len = sizeof(credential->ssid);
    esp_err_t err = nvs_get_str(nvs, ssid_key, credential->ssid, &ssid_len);
    if (err != ESP_OK || !has_text(credential->ssid)) {
        return false;
    }

    size_t password_len = sizeof(credential->password);
    err = nvs_get_str(nvs, password_key, credential->password, &password_len);
    if (err != ESP_OK) {
        credential->password[0] = '\0';
    }
    return true;
}

static size_t load_saved_wifi_credentials(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open("wifi_cfg", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return 0;
    }

    uint8_t count = 0;
    nvs_get_u8(nvs, "count", &count);
    if (count > WIFI_CREDENTIAL_SLOTS) {
        count = WIFI_CREDENTIAL_SLOTS;
    }

    size_t loaded = 0;
    for (uint8_t attempt = 0; attempt < count && loaded < WIFI_CREDENTIAL_SLOTS; ++attempt) {
        wifi_credential_t credential = {};
        if (read_wifi_slot(nvs, attempt, &credential)) {
            s_wifi_credentials[loaded++] = credential;
        }
    }

    nvs_close(nvs);
    return loaded;
}

static void load_station_credentials(void)
{
    memset(s_wifi_credentials, 0, sizeof(s_wifi_credentials));
    s_wifi_credential_count = load_saved_wifi_credentials();
    s_current_wifi_credential = 0;

    if (s_wifi_credential_count > 0) {
        ESP_LOGI(TAG, "Loaded %u saved WiFi network(s); first SSID \"%s\"",
                 (unsigned)s_wifi_credential_count, s_wifi_credentials[0].ssid);
        return;
    }

    if (has_text(s_config.wifi_ssid)) {
        strlcpy(s_wifi_credentials[0].ssid, s_config.wifi_ssid,
                sizeof(s_wifi_credentials[0].ssid));
        strlcpy(s_wifi_credentials[0].password, s_config.wifi_password,
                sizeof(s_wifi_credentials[0].password));
        s_wifi_credential_count = 1;
        ESP_LOGI(TAG, "Using build-configured WiFi SSID \"%s\"",
                 s_wifi_credentials[0].ssid);
        return;
    }
}

static esp_err_t apply_current_station_config(void)
{
    if (s_current_wifi_credential >= s_wifi_credential_count) {
        return ESP_ERR_NOT_FOUND;
    }

    const wifi_credential_t *credential = &s_wifi_credentials[s_current_wifi_credential];
    wifi_config_t wifi_config = {};
    strlcpy((char *)wifi_config.sta.ssid, credential->ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, credential->password,
            sizeof(wifi_config.sta.password));
    wifi_config.sta.failure_retry_cnt = 1;
    return esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
}

static void connect_next_station_or_start_portal(void)
{
    if (s_current_wifi_credential + 1 < s_wifi_credential_count) {
        ++s_current_wifi_credential;
        s_sta_retries = 0;
        ESP_LOGW(TAG, "Trying saved WiFi SSID \"%s\"",
                 s_wifi_credentials[s_current_wifi_credential].ssid);
        if (apply_current_station_config() == ESP_OK) {
            esp_wifi_connect();
            return;
        }
    }

    ESP_LOGW(TAG, "No known WiFi networks connected; starting setup portal");
    if (start_provisioning_ap() != ESP_OK) {
        ESP_LOGW(TAG, "Setup portal failed to start");
    }
}

static esp_err_t delete_wifi_credential(uint8_t slot)
{
    if (slot >= s_wifi_credential_count) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t nvs = 0;
    ESP_RETURN_ON_ERROR(nvs_open("wifi_cfg", NVS_READWRITE, &nvs),
                        TAG, "failed to open WiFi NVS namespace");

    wifi_credential_t credentials[WIFI_CREDENTIAL_SLOTS] = {};
    memcpy(credentials, s_wifi_credentials, sizeof(credentials));

    // Shift left to overwrite the deleted slot
    for (uint8_t i = slot; i + 1 < WIFI_CREDENTIAL_SLOTS; ++i) {
        credentials[i] = credentials[i + 1];
    }
    memset(&credentials[WIFI_CREDENTIAL_SLOTS - 1], 0, sizeof(wifi_credential_t));

    s_wifi_credential_count--;

    esp_err_t err = ESP_OK;
    for (uint8_t i = 0; i < WIFI_CREDENTIAL_SLOTS; ++i) {
        char ssid_key[8] = {};
        char password_key[8] = {};
        snprintf(ssid_key, sizeof(ssid_key), "ssid%u", (unsigned)i);
        snprintf(password_key, sizeof(password_key), "pass%u", (unsigned)i);

        if (i < s_wifi_credential_count) {
            err = nvs_set_str(nvs, ssid_key, credentials[i].ssid);
            if (err == ESP_OK) {
                err = nvs_set_str(nvs, password_key, credentials[i].password);
            }
        } else {
            nvs_erase_key(nvs, ssid_key);
            nvs_erase_key(nvs, password_key);
        }
    }

    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "count", (uint8_t)s_wifi_credential_count);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);
    if (err == ESP_OK) {
        memcpy(s_wifi_credentials, credentials, sizeof(s_wifi_credentials));
    }
    return err;
}

static esp_err_t save_wifi_credentials(const char *ssid, const char *password)
{
    wifi_credential_t credentials[WIFI_CREDENTIAL_SLOTS] = {};
    uint8_t count = (uint8_t)s_wifi_credential_count;
    memcpy(credentials, s_wifi_credentials, sizeof(credentials));

    int slot = -1;
    for (uint8_t i = 0; i < count; ++i) {
        if (strcmp(credentials[i].ssid, ssid) == 0) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        if (count < WIFI_CREDENTIAL_SLOTS) {
            slot = count;
            count++;
        } else {
            slot = WIFI_CREDENTIAL_SLOTS - 1;
        }
    }

    strlcpy(credentials[slot].ssid, ssid, sizeof(credentials[slot].ssid));
    strlcpy(credentials[slot].password, password, sizeof(credentials[slot].password));

    nvs_handle_t nvs = 0;
    ESP_RETURN_ON_ERROR(nvs_open("wifi_cfg", NVS_READWRITE, &nvs),
                        TAG, "failed to open WiFi NVS namespace");

    esp_err_t err = ESP_OK;
    for (uint8_t i = 0; i < count; ++i) {
        char ssid_key[8] = {};
        char password_key[8] = {};
        snprintf(ssid_key, sizeof(ssid_key), "ssid%u", (unsigned)i);
        snprintf(password_key, sizeof(password_key), "pass%u", (unsigned)i);

        err = nvs_set_str(nvs, ssid_key, credentials[i].ssid);
        if (err == ESP_OK) {
            err = nvs_set_str(nvs, password_key, credentials[i].password);
        }
        if (err != ESP_OK) {
            break;
        }
    }

    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "count", count);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err == ESP_OK) {
        s_wifi_credential_count = count;
        memcpy(s_wifi_credentials, credentials, sizeof(s_wifi_credentials));
    }
    return err;
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_html(httpd_req_t *req, const char *html)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    return httpd_resp_sendstr(req, html);
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    return send_html(req, s_control_page);
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    return send_html(req, s_settings_page);
}

static esp_err_t health_get_handler(httpd_req_t *req)
{
    return send_json(req, "{\"ok\":true,\"device\":\"esp-bh1750\"}");
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char response[256] = {};
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"lux\":%.1f,\"wifi_connected\":%s,\"hostname\":\"%s\"}",
             s_current_lux,
             s_sta_connected ? "true" : "false",
             s_device_hostname);
    return send_json(req, response);
}

static esp_err_t command_post_handler(httpd_req_t *req)
{
    char body[32] = {};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, "{\"ok\":false,\"error\":\"empty request body\"}");
    }
    body[received] = '\0';

    if (strcmp(body, "reboot") == 0) {
        ESP_LOGI(TAG, "Rebooting device via HTTP request");
        send_json(req, "{\"ok\":true,\"message\":\"Rebooting...\"}");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        return ESP_OK;
    }

    httpd_resp_set_status(req, "400 Bad Request");
    return send_json(req, "{\"ok\":false,\"error\":\"unknown command\"}");
}

static esp_err_t perform_wifi_scan(void)
{
    wifi_scan_config_t scan_config = {
        .show_hidden = false,
    };

    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan failed to start: %s", esp_err_to_name(err));
        return err;
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > WIFI_SCAN_MAX_APS) {
        ap_count = WIFI_SCAN_MAX_APS;
    }

    wifi_ap_record_t *ap_records = calloc(ap_count, sizeof(wifi_ap_record_t));
    if (ap_records == NULL) {
        return ESP_ERR_NO_MEM;
    }

    err = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (err == ESP_OK) {
        s_scan_result_count = 0;
        for (uint16_t i = 0; i < ap_count; ++i) {
            if (ap_records[i].ssid[0] == '\0') {
                continue;
            }
            bool duplicate = false;
            for (size_t j = 0; j < s_scan_result_count; ++j) {
                if (strcmp(s_scan_results[j].ssid, (char *)ap_records[i].ssid) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            strlcpy(s_scan_results[s_scan_result_count].ssid,
                    (char *)ap_records[i].ssid,
                    sizeof(s_scan_results[0].ssid));
            s_scan_results[s_scan_result_count].rssi = ap_records[i].rssi;
            s_scan_results[s_scan_result_count].secure =
                ap_records[i].authmode != WIFI_AUTH_OPEN;
            s_scan_result_count++;
        }
    }

    free(ap_records);
    ESP_LOGI(TAG, "Wi-Fi scan found %u network(s)", (unsigned)s_scan_result_count);
    return err;
}

static esp_err_t wifi_scan_get_handler(httpd_req_t *req)
{
    perform_wifi_scan();

    char *json = calloc(1, 4096);
    if (json == NULL) {
        return send_json(req, "{\"ok\":false,\"error\":\"out of memory\"}");
    }

    size_t offset = 0;
    offset += snprintf(json + offset, 4096 - offset, "{\"ok\":true,\"networks\":[");

    for (size_t i = 0; i < s_scan_result_count; ++i) {
        char escaped_ssid[WIFI_SSID_MAX_LEN * 6 + 1] = {};
        json_escape_string(s_scan_results[i].ssid, escaped_ssid, sizeof(escaped_ssid));
        offset += snprintf(json + offset, 4096 - offset,
                           "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
                           i == 0 ? "" : ",",
                           escaped_ssid,
                           s_scan_results[i].rssi,
                           s_scan_results[i].secure ? "true" : "false");
    }

    snprintf(json + offset, 4096 - offset, "]}");
    esp_err_t err = send_json(req, json);
    free(json);
    return err;
}

static esp_err_t wifi_get_handler(httpd_req_t *req)
{
    char json[1024] = {};
    size_t offset = 0;
    wifi_ap_record_t ap_info = {};
    esp_netif_ip_info_t ip_info = {};
    bool connected = s_sta_connected && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
    bool has_ip = connected && s_sta_netif != NULL &&
                  esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK;
    char connected_ssid[WIFI_SSID_MAX_LEN * 6 + 1] = {};
    char hostname[DEVICE_HOSTNAME_MAX_LEN * 6 + 1] = {};
    char ip_addr[16] = {};

    if (connected) {
        json_escape_string((const char *)ap_info.ssid, connected_ssid, sizeof(connected_ssid));
    }
    json_escape_string(s_device_hostname, hostname, sizeof(hostname));
    if (has_ip) {
        snprintf(ip_addr, sizeof(ip_addr), IPSTR, IP2STR(&ip_info.ip));
    }

    offset += snprintf(json + offset, sizeof(json) - offset,
                       "{\"ok\":true,\"connected\":%s,\"ssid\":\"%s\","
                       "\"rssi\":%d,\"ip\":\"%s\",\"hostname\":\"%s\",\"slots\":[",
                       connected ? "true" : "false",
                       connected_ssid,
                       connected ? ap_info.rssi : 0,
                       ip_addr,
                       hostname);
    for (size_t i = 0; i < s_wifi_credential_count; ++i) {
        char escaped_ssid[WIFI_SSID_MAX_LEN * 6 + 1] = {};
        json_escape_string(s_wifi_credentials[i].ssid, escaped_ssid, sizeof(escaped_ssid));
        offset += snprintf(json + offset, sizeof(json) - offset,
                           "%s{\"slot\":%u,\"ssid\":\"%s\"}",
                           i == 0 ? "" : ",", (unsigned)i, escaped_ssid);
    }
    offset += snprintf(json + offset, sizeof(json) - offset, "]}");
    return send_json(req, json);
}

static bool parse_enabled_value(const char *value, bool *out)
{
    if (strcmp(value, "1") == 0 || strcmp(value, "true") == 0 || strcmp(value, "on") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(value, "0") == 0 || strcmp(value, "false") == 0 || strcmp(value, "off") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    char form[WIFI_FORM_MAX_LEN] = {};
    char action[16] = {};
    char slot_value[8] = {};
    char hostname[DEVICE_HOSTNAME_MAX_LEN + 1] = {};
    char ssid[WIFI_SSID_MAX_LEN + 1] = {};
    char password[WIFI_PASSWORD_MAX_LEN + 1] = {};

    if (read_form_body(req, form, sizeof(form)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_html(req, "<html><body>WiFi form is too large.</body></html>");
    }

    if (form_get_value(form, "action", action, sizeof(action)) &&
        strcmp(action, "hostname") == 0) {
        if (!form_get_value(form, "hostname", hostname, sizeof(hostname)) ||
            !has_text(hostname)) {
            httpd_resp_set_status(req, "400 Bad Request");
            return send_json(req, "{\"ok\":false,\"error\":\"hostname is required\"}");
        }

        esp_err_t err = save_device_hostname_setting(hostname);
        if (err == ESP_ERR_INVALID_ARG) {
            httpd_resp_set_status(req, "400 Bad Request");
            return send_json(req, "{\"ok\":false,\"error\":\"hostname must use letters, numbers, or hyphens\"}");
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save hostname: %s", esp_err_to_name(err));
            httpd_resp_set_status(req, "500 Internal Server Error");
            return send_json(req, "{\"ok\":false,\"error\":\"failed to save hostname\"}");
        }

        ESP_LOGI(TAG, "Saved hostname \"%s\"; restarting", s_device_hostname);
        esp_err_t send_err = send_json(req, "{\"ok\":true,\"message\":\"Hostname saved; restarting\"}");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        return send_err;
    }

    if (strcmp(action, "delete") == 0) {
        if (!form_get_value(form, "slot", slot_value, sizeof(slot_value))) {
            httpd_resp_set_status(req, "400 Bad Request");
            return send_json(req, "{\"ok\":false,\"error\":\"slot is required\"}");
        }
        char *end = NULL;
        long slot = strtol(slot_value, &end, 10);
        if (end == slot_value || *end != '\0' || slot < 0 ||
            slot >= (long)s_wifi_credential_count) {
            httpd_resp_set_status(req, "400 Bad Request");
            return send_json(req, "{\"ok\":false,\"error\":\"invalid WiFi slot\"}");
        }

        char deleted_ssid[WIFI_SSID_MAX_LEN + 1] = {};
        strlcpy(deleted_ssid, s_wifi_credentials[slot].ssid, sizeof(deleted_ssid));
        esp_err_t err = delete_wifi_credential((uint8_t)slot);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to delete WiFi credential: %s", esp_err_to_name(err));
            httpd_resp_set_status(req, "500 Internal Server Error");
            return send_json(req, "{\"ok\":false,\"error\":\"failed to delete WiFi network\"}");
        }

        ESP_LOGI(TAG, "Deleted WiFi credentials for SSID \"%s\"", deleted_ssid);
        return send_json(req, "{\"ok\":true,\"message\":\"WiFi network deleted\"}");
    }

    if (!form_get_value(form, "ssid", ssid, sizeof(ssid)) || !has_text(ssid)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, "{\"ok\":false,\"error\":\"SSID is required\"}");
    }
    form_get_value(form, "password", password, sizeof(password));

    esp_err_t err = save_wifi_credentials(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save WiFi credentials: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json(req, "{\"ok\":false,\"error\":\"failed to save WiFi settings\"}");
    }

    ESP_LOGI(TAG, "Saved WiFi credentials for SSID \"%s\"; restarting", ssid);
    esp_err_t send_err = send_json(req, "{\"ok\":true,\"message\":\"WiFi saved; restarting\"}");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return send_err;
}

static esp_err_t homekit_get_handler(httpd_req_t *req)
{
    homekit_accessory_status_t status = {};
    char setup_code[sizeof(status.setup_code) * 6 + 1] = {};
    char setup_id[sizeof(status.setup_id) * 6 + 1] = {};
    char setup_payload[sizeof(status.setup_payload) * 6 + 1] = {};
    char response[640] = {};

    homekit_accessory_get_status(&status);
    json_escape_string(status.setup_code, setup_code, sizeof(setup_code));
    json_escape_string(status.setup_id, setup_id, sizeof(setup_id));
    json_escape_string(status.setup_payload, setup_payload, sizeof(setup_payload));

    snprintf(response, sizeof(response),
             "{\"ok\":true,\"enabled\":%s,\"available\":%s,"
             "\"started\":%s,\"pairing\":%s,\"pairing_timed_out\":%s,"
             "\"paired_controller_count\":%d,\"connected_controller_count\":%d,"
             "\"setup_code\":\"%s\",\"setup_id\":\"%s\",\"setup_payload\":\"%s\","
             "\"hap_port\":%d}",
             load_homekit_enabled_setting() ? "true" : "false",
             status.compiled ? "true" : "false",
             status.started ? "true" : "false",
             status.pairing ? "true" : "false",
             status.pairing_timed_out ? "true" : "false",
             status.paired_controller_count,
             status.connected_controller_count,
             setup_code,
             setup_id,
             setup_payload,
             80); // Default HTTP port
    return send_json(req, response);
}

static esp_err_t homekit_post_handler(httpd_req_t *req)
{
    char form[WIFI_FORM_MAX_LEN] = {};
    char enabled_value[16] = {};
    bool enabled = false;

    if (read_form_body(req, form, sizeof(form)) != ESP_OK ||
        !form_get_value(form, "enabled", enabled_value, sizeof(enabled_value)) ||
        !parse_enabled_value(enabled_value, &enabled)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, "{\"ok\":false,\"error\":\"HomeKit enabled value is required\"}");
    }

    esp_err_t err = save_homekit_enabled_setting(enabled);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save HomeKit enabled setting: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json(req, "{\"ok\":false,\"error\":\"failed to save HomeKit enabled setting\"}");
    }

    ESP_LOGI(TAG, "Saved HomeKit enabled setting: %d; restarting", enabled);
    esp_err_t send_err = send_json(req, "{\"ok\":true,\"message\":\"HomeKit setting saved; restarting\"}");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return send_err;
}

static esp_err_t start_http_server(void)
{
    if (s_httpd != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = HTTP_URI_HANDLER_SLOTS;
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &config), TAG, "failed to start HTTP server");

    const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    const httpd_uri_t health_uri = {
        .uri = "/health",
        .method = HTTP_GET,
        .handler = health_get_handler,
    };
    const httpd_uri_t settings_uri = {
        .uri = "/settings",
        .method = HTTP_GET,
        .handler = settings_get_handler,
    };
    const httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
    };
    const httpd_uri_t command_uri = {
        .uri = "/command",
        .method = HTTP_POST,
        .handler = command_post_handler,
    };
    const httpd_uri_t wifi_get_uri = {
        .uri = "/wifi",
        .method = HTTP_GET,
        .handler = wifi_get_handler,
    };
    const httpd_uri_t wifi_uri = {
        .uri = "/wifi",
        .method = HTTP_POST,
        .handler = wifi_post_handler,
    };
    const httpd_uri_t wifi_scan_uri = {
        .uri = "/wifi/scan",
        .method = HTTP_GET,
        .handler = wifi_scan_get_handler,
    };
    const httpd_uri_t homekit_get_uri = {
        .uri = "/homekit",
        .method = HTTP_GET,
        .handler = homekit_get_handler,
    };
    const httpd_uri_t homekit_uri = {
        .uri = "/homekit",
        .method = HTTP_POST,
        .handler = homekit_post_handler,
    };
    const httpd_uri_t captive_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = settings_get_handler,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &root_uri), TAG, "failed to register /");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &health_uri), TAG, "failed to register /health");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &settings_uri), TAG, "failed to register /settings");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &status_uri), TAG, "failed to register /status");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &command_uri), TAG, "failed to register /command");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &wifi_get_uri), TAG, "failed to register GET /wifi");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &wifi_uri), TAG, "failed to register POST /wifi");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &wifi_scan_uri), TAG, "failed to register GET /wifi/scan");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &homekit_get_uri), TAG, "failed to register GET /homekit");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &homekit_uri), TAG, "failed to register POST /homekit");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &captive_uri), TAG, "failed to register /*");

    return ESP_OK;
}

static void dns_server_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGW(TAG, "Failed to create DNS socket: errno=%d", errno);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGW(TAG, "Failed to bind DNS socket: errno=%d", errno);
        close(sock);
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Captive portal DNS server started");
    uint8_t request[DNS_PACKET_MAX_LEN];
    uint8_t response[DNS_PACKET_MAX_LEN];

    while (true) {
        struct sockaddr_in source_addr = {};
        socklen_t source_len = sizeof(source_addr);
        int len = recvfrom(sock, request, sizeof(request), 0,
                           (struct sockaddr *)&source_addr, &source_len);
        if (len < 17) {
            continue;
        }

        size_t question_end = 12;
        while (question_end < (size_t)len && request[question_end] != 0) {
            question_end += (size_t)request[question_end] + 1;
        }
        question_end += 5;
        if (question_end > (size_t)len || question_end + 16 > sizeof(response)) {
            continue;
        }

        esp_netif_ip_info_t ip_info = {};
        if (s_ap_netif == NULL || esp_netif_get_ip_info(s_ap_netif, &ip_info) != ESP_OK) {
            continue;
        }

        memcpy(response, request, question_end);
        response[2] = 0x81;
        response[3] = 0x80;
        response[4] = 0x00;
        response[5] = 0x01;
        response[6] = 0x00;
        response[7] = 0x01;
        response[8] = 0x00;
        response[9] = 0x00;
        response[10] = 0x00;
        response[11] = 0x00;

        size_t offset = question_end;
        response[offset++] = 0xC0;
        response[offset++] = 0x0C;
        response[offset++] = 0x00;
        response[offset++] = 0x01;
        response[offset++] = 0x00;
        response[offset++] = 0x01;
        response[offset++] = 0x00;
        response[offset++] = 0x00;
        response[offset++] = 0x00;
        response[offset++] = 0x00;
        response[offset++] = 0x00;
        response[offset++] = 0x04;
        memcpy(response + offset, &ip_info.ip.addr, 4);
        offset += 4;

        sendto(sock, response, offset, 0, (struct sockaddr *)&source_addr, source_len);
    }
}

static esp_err_t start_dns_server(void)
{
    if (s_dns_task != NULL) {
        return ESP_OK;
    }
    BaseType_t ok = xTaskCreate(dns_server_task, "captive_dns", 3072, NULL, 4, &s_dns_task);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

static esp_err_t configure_setup_ap(void)
{
    wifi_config_t ap_config = {};
    strlcpy((char *)ap_config.ap.ssid, CONFIG_ESP_BH1750_SETUP_AP_SSID,
            sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(CONFIG_ESP_BH1750_SETUP_AP_SSID);
    ap_config.ap.channel = CONFIG_ESP_BH1750_SETUP_AP_CHANNEL;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    if (has_text(CONFIG_ESP_BH1750_SETUP_AP_PASSWORD)) {
        strlcpy((char *)ap_config.ap.password, CONFIG_ESP_BH1750_SETUP_AP_PASSWORD,
                sizeof(ap_config.ap.password));
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    return esp_wifi_set_config(WIFI_IF_AP, &ap_config);
}

static esp_err_t start_provisioning_services(void)
{
    if (s_ap_started) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(start_http_server(), TAG, "failed to start setup HTTP server");
    ESP_RETURN_ON_ERROR(start_dns_server(), TAG, "failed to start captive DNS server");

    s_ap_started = true;
    ESP_LOGW(TAG, "Setup AP active: SSID \"%s\", open http://192.168.4.1/",
             CONFIG_ESP_BH1750_SETUP_AP_SSID);
    return ESP_OK;
}

static esp_err_t start_provisioning_ap(void)
{
    if (s_ap_started) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "failed to enable setup AP mode");
    ESP_RETURN_ON_ERROR(configure_setup_ap(), TAG, "failed to configure setup AP");
    return start_provisioning_services();
}

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_wifi_credential_count > 0) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        if (s_sta_retries < CONFIG_ESP_BH1750_WIFI_MAX_RETRIES) {
            ++s_sta_retries;
            ESP_LOGW(TAG, "WiFi disconnected; reconnecting (%d/%d)",
                     s_sta_retries, CONFIG_ESP_BH1750_WIFI_MAX_RETRIES);
            esp_wifi_connect();
        } else {
            connect_next_station_or_start_portal();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "Setup AP started");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        s_sta_connected = true;
        s_sta_retries = 0;
        ESP_LOGI(TAG, "WiFi connected, IP " IPSTR, IP2STR(&event->ip_info.ip));
        if (start_http_server() != ESP_OK) {
            ESP_LOGW(TAG, "HTTP server failed to start");
        }
        if (s_config.on_wifi_connected != NULL) {
            s_config.on_wifi_connected();
        }
    }
}

esp_err_t iot_remote_start(const iot_remote_config_t *config)
{
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_INVALID_ARG, TAG, "config is required");

    s_config = *config;

    load_device_hostname_setting();
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "failed to initialize netif");

    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "failed to create default event loop");
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(s_sta_netif != NULL, ESP_ERR_NO_MEM, TAG, "failed to create station netif");
    ESP_RETURN_ON_ERROR(esp_netif_set_hostname(s_sta_netif, s_device_hostname),
                        TAG, "failed to set station hostname");
    ESP_LOGI(TAG, "WiFi hostname set to \"%s\"", s_device_hostname);

    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_init), TAG, "failed to initialize WiFi");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                   wifi_event_handler, NULL),
                        TAG, "failed to register WiFi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                   wifi_event_handler, NULL),
                        TAG, "failed to register IP handler");

    load_station_credentials();

    if (s_wifi_credential_count > 0) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "failed to set WiFi mode");
        ESP_RETURN_ON_ERROR(apply_current_station_config(), TAG, "failed to set WiFi config");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "failed to start WiFi");

        ESP_LOGI(TAG, "WiFi connecting to SSID \"%s\"",
                 s_wifi_credentials[s_current_wifi_credential].ssid);
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "failed to set APSTA mode");
    ESP_RETURN_ON_ERROR(configure_setup_ap(), TAG, "failed to configure setup AP");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "failed to start WiFi AP");
    return start_provisioning_services();
}

void iot_remote_set_lux(float lux)
{
    s_current_lux = lux;
}
