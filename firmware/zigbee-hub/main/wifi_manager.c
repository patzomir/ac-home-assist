#include "wifi_manager.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include <string.h>
#include <stdlib.h>

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_MAX_RETRIES    10

/* NVS namespace (shared with nvs_store) */
#define NVS_NS    "hub"
#define KEY_SSID  "wifi_ssid"
#define KEY_PASS  "wifi_pass"

/* SoftAP provisioning network */
#define PROV_AP_SSID     "AC-Hub-Setup"
#define PROV_AP_CHANNEL  6
#define PROV_AP_MAX_CONN 4

static EventGroupHandle_t     s_wifi_event_group;
static int                    s_retry_count = 0;
static bool                   s_connected   = false;
static wifi_connected_cb_t    s_on_connected;
static wifi_disconnected_cb_t s_on_disconnected;

/* Survives soft reset (esp_restart) but is cleared on power-on.
 * Used to allow exactly one automatic restart on first boot to let the
 * RF calibration data settle before the first real connection attempt. */
static RTC_DATA_ATTR bool s_post_cal_restart_done;

/* ---- NVS helpers -------------------------------------------------------- */

esp_err_t wifi_manager_save_creds(const char *ssid, const char *password)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) return ret;

    ret = nvs_set_str(h, KEY_SSID, ssid);
    if (ret == ESP_OK) ret = nvs_set_str(h, KEY_PASS, password);
    if (ret == ESP_OK) ret = nvs_commit(h);
    nvs_close(h);
    return ret;
}

static esp_err_t load_creds(char *ssid, size_t ssid_len,
                             char *pass, size_t pass_len)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (ret != ESP_OK) return ret;

    ret = nvs_get_str(h, KEY_SSID, ssid, &ssid_len);
    if (ret == ESP_OK) ret = nvs_get_str(h, KEY_PASS, pass, &pass_len);
    nvs_close(h);
    return ret;
}

/* ---- STA event handler -------------------------------------------------- */

static void sta_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Disconnected, reason: %d", disc->reason);
        s_connected = false;
        if (s_on_disconnected) s_on_disconnected();

        if (s_retry_count < WIFI_MAX_RETRIES) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGI(TAG, "Reconnecting… attempt %d", s_retry_count);
        } else if (!s_post_cal_restart_done) {
            /* First-boot RF calibration leaves a high noise floor that hides
             * the AP (WIFI_REASON_NO_AP_FOUND).  One soft restart loads the
             * freshly-written calibration data and fixes reception.
             * s_post_cal_restart_done is RTC_DATA_ATTR so it survives this
             * restart but is cleared on the next power-on. */
            s_post_cal_restart_done = true;
            ESP_LOGW(TAG, "WiFi scan failed on first boot — restarting once for RF cal");
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        } else {
            ESP_LOGE(TAG, "WiFi connection failed after %d attempts", WIFI_MAX_RETRIES);
        }
    }

    if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry_count = 0;
        s_connected   = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (s_on_connected) s_on_connected();
    }
}

/* ---- STA connect -------------------------------------------------------- */

static void start_sta(const char *ssid, const char *password)
{
    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, sta_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, sta_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.sta.ssid,     ssid,     sizeof(wifi_cfg.sta.ssid)     - 1);
    strncpy((char *)wifi_cfg.sta.password, password, sizeof(wifi_cfg.sta.password) - 1);
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.sta.pmf_cfg.capable    = true;
    wifi_cfg.sta.pmf_cfg.required   = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Disable power save so the coex arbiter gives Zigbee predictable airtime.
       With power save on, the radio locks to WiFi beacon windows and starves
       the 802.15.4 receiver on this shared-radio chip (ESP32-C6). */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    esp_wifi_connect();

    ESP_LOGI(TAG, "WiFi STA started, connecting to '%s'", ssid);

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
}

/* ---- Captive portal HTML ------------------------------------------------ */

static const char PORTAL_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>AC Hub Setup</title>"
    "<style>"
    "body{font-family:sans-serif;max-width:360px;margin:40px auto;padding:0 16px}"
    "h1{font-size:1.4em}"
    "label{display:block;margin-top:12px;font-weight:bold}"
    "input{width:100%;padding:8px;box-sizing:border-box;margin-top:4px;font-size:1em}"
    "button{width:100%;padding:10px;margin-top:16px;background:#0070f3;"
    "color:#fff;border:none;border-radius:4px;font-size:1em;cursor:pointer}"
    "</style></head><body>"
    "<h1>AC Hub Wi-Fi Setup</h1>"
    "<p>Enter your Wi-Fi credentials to connect the hub to your network.</p>"
    "<form method='POST' action='/save'>"
    "<label>Network (SSID)"
    "<input name='ssid' type='text' required autocomplete='off'>"
    "</label>"
    "<label>Password"
    "<input name='pass' type='password' autocomplete='off'>"
    "</label>"
    "<button type='submit'>Connect</button>"
    "</form></body></html>";

static const char SAVED_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<title>Saved</title>"
    "</head><body>"
    "<h2>Credentials saved!</h2>"
    "<p>The hub will now restart and connect to your network.</p>"
    "</body></html>";

/* ---- URL-decode helpers ------------------------------------------------- */

static void url_decode(const char *src, size_t src_len, char *dst, size_t dst_len)
{
    size_t i = 0, j = 0;
    while (i < src_len && j < dst_len - 1) {
        if (src[i] == '+') {
            dst[j++] = ' ';
            i++;
        } else if (src[i] == '%' && i + 2 < src_len) {
            char hex[3] = { src[i + 1], src[i + 2], '\0' };
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

/*
 * Extract a URL-encoded form field from body.
 * Returns true and fills buf on success; returns false if key not found.
 */
static bool parse_form_field(const char *body, size_t body_len,
                              const char *key, char *buf, size_t buf_len)
{
    size_t klen = strlen(key);
    const char *p = body;
    const char *end = body + body_len;

    while (p < end) {
        if ((size_t)(end - p) > klen &&
            strncmp(p, key, klen) == 0 &&
            p[klen] == '=') {
            const char *val_start = p + klen + 1;
            const char *val_end   = val_start;
            while (val_end < end && *val_end != '&') val_end++;
            url_decode(val_start, (size_t)(val_end - val_start), buf, buf_len);
            return true;
        }
        /* advance to next field */
        while (p < end && *p != '&') p++;
        if (p < end) p++;
    }
    return false;
}

/* ---- HTTP handlers ------------------------------------------------------ */

static esp_err_t get_root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t post_save_handler(httpd_req_t *req)
{
    char body[512] = { 0 };
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char ssid[33] = { 0 };
    char pass[65] = { 0 };

    if (!parse_form_field(body, (size_t)received, "ssid", ssid, sizeof(ssid)) ||
        ssid[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "SSID is required");
        return ESP_FAIL;
    }
    parse_form_field(body, (size_t)received, "pass", pass, sizeof(pass));

    ESP_LOGI(TAG, "Portal: saving SSID '%s'", ssid);
    esp_err_t ret = wifi_manager_save_creds(ssid, pass);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save credentials: %s", esp_err_to_name(ret));
        httpd_resp_send_500(req);
        return ret;
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, SAVED_HTML, HTTPD_RESP_USE_STRLEN);

    /* Short delay so the response is delivered before restart */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

/* ---- SoftAP provisioning mode ------------------------------------------ */

static void start_provisioning_ap(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = PROV_AP_SSID,
            .ssid_len       = sizeof(PROV_AP_SSID) - 1,
            .channel        = PROV_AP_CHANNEL,
            .password       = "",
            .max_connection = PROV_AP_MAX_CONN,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP '%s' started — connect and open http://192.168.4.1",
             PROV_AP_SSID);

    /* Start HTTP server */
    httpd_handle_t server      = NULL;
    httpd_config_t http_cfg    = HTTPD_DEFAULT_CONFIG();
    http_cfg.lru_purge_enable  = true;
    ESP_ERROR_CHECK(httpd_start(&server, &http_cfg));

    static const httpd_uri_t root_uri = {
        .uri     = "/",
        .method  = HTTP_GET,
        .handler = get_root_handler,
    };
    httpd_register_uri_handler(server, &root_uri);

    static const httpd_uri_t save_uri = {
        .uri     = "/save",
        .method  = HTTP_POST,
        .handler = post_save_handler,
    };
    httpd_register_uri_handler(server, &save_uri);

    ESP_LOGI(TAG, "Captive portal ready — waiting for credentials");

    /* Block here; post_save_handler calls esp_restart() when done */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---- Public API --------------------------------------------------------- */

esp_err_t wifi_manager_start(wifi_connected_cb_t on_connected,
                              wifi_disconnected_cb_t on_disconnected)
{
    s_on_connected    = on_connected;
    s_on_disconnected = on_disconnected;
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Check NVS for saved credentials */
    char ssid[33] = { 0 };
    char pass[65] = { 0 };
    esp_err_t ret = load_creds(ssid, sizeof(ssid), pass, sizeof(pass));

    if (ret == ESP_OK && ssid[0] != '\0') {
        ESP_LOGI(TAG, "Loaded saved credentials for '%s', connecting in STA mode", ssid);
        start_sta(ssid, pass);
    } else {
        ESP_LOGW(TAG, "No saved WiFi credentials — starting provisioning AP");
        start_provisioning_ap();
        /* Does not return; device restarts after credentials are submitted */
    }

    return ESP_OK;
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}
