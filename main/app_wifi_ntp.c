#include "app_wifi_ntp.h"
#include "app_settings.h"
#include "app_http_server.h"
#include "bsp_rtc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_check.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "app_wifi_ntp";

static SemaphoreHandle_t s_net_mux = NULL;
static SemaphoreHandle_t s_scan_mux = NULL;
static net_status_t s_status;
static esp_timer_handle_t s_reconnect_timer = NULL;
static bool s_portal_active = false;
static bool s_wifi_started = false;
static int s_retry_cnt = 0;

static char s_sntp_srv0[64] = "ntp.aliyun.com";

static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    ESP_LOGI(TAG, "SNTP time synchronized!");
    bsp_rtc_sync_from_system();

    xSemaphoreTake(s_net_mux, portMAX_DELAY);
    s_status.ntp_synced = true;
    s_status.ntp_syncing = false;
    time(&s_status.last_sync_epoch);
    xSemaphoreGive(s_net_mux);
}

static void start_or_restart_sntp(void)
{
    app_settings_t cfg;
    app_settings_get(&cfg);
    strlcpy(s_sntp_srv0, cfg.ntp_server[0] ? cfg.ntp_server : "ntp.aliyun.com", sizeof(s_sntp_srv0));

    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, s_sntp_srv0);
    esp_sntp_setservername(1, "ntp.tencent.com");
    esp_sntp_setservername(2, "cn.pool.ntp.org");
    esp_sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    esp_sntp_set_sync_interval(6UL * 3600UL * 1000UL);
    esp_sntp_set_time_sync_notification_cb(sntp_sync_cb);

    xSemaphoreTake(s_net_mux, portMAX_DELAY);
    s_status.ntp_syncing = true;
    xSemaphoreGive(s_net_mux);

    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started (primary=%s)", s_sntp_srv0);
}

static void reconnect_timer_cb(void *arg)
{
    (void)arg;
    if (s_portal_active) return;

    app_settings_t cfg;
    app_settings_get(&cfg);
    if (cfg.wifi_ssid[0] == '\0') return;

    ESP_LOGI(TAG, "Attempting Wi-Fi reconnect to '%s' (attempt %d)", cfg.wifi_ssid, s_retry_cnt + 1);
    esp_wifi_connect();
}

static void schedule_reconnect(void)
{
    static const uint32_t backoff_ms[] = {3000, 10000, 30000, 60000};
    int idx = s_retry_cnt;
    if (idx > 3) idx = 3;
    s_retry_cnt++;

    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
        esp_timer_start_once(s_reconnect_timer, (uint64_t)backoff_ms[idx] * 1000ULL);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            if (!s_portal_active) {
                app_settings_t cfg;
                app_settings_get(&cfg);
                if (cfg.wifi_ssid[0] != '\0') {
                    xSemaphoreTake(s_net_mux, portMAX_DELAY);
                    s_status.wifi_state = NET_CONNECTING;
                    strlcpy(s_status.ssid, cfg.wifi_ssid, sizeof(s_status.ssid));
                    xSemaphoreGive(s_net_mux);
                    esp_wifi_connect();
                }
            }
            break;

        case WIFI_EVENT_STA_DISCONNECTED:
            xSemaphoreTake(s_net_mux, portMAX_DELAY);
            s_status.ip_str[0] = '\0';
            s_status.rssi = 0;
            s_status.ntp_syncing = false;
            if (s_portal_active) {
                s_status.wifi_state = NET_PORTAL_AP;
            } else {
                app_settings_t cfg;
                app_settings_get(&cfg);
                s_status.wifi_state = (cfg.wifi_ssid[0] != '\0') ? NET_CONNECTING : NET_IDLE;
            }
            xSemaphoreGive(s_net_mux);

            if (!s_portal_active) {
                schedule_reconnect();
            }
            break;

        case WIFI_EVENT_AP_STACONNECTED:
            xSemaphoreTake(s_net_mux, portMAX_DELAY);
            s_status.ap_sta_count++;
            xSemaphoreGive(s_net_mux);
            break;

        case WIFI_EVENT_AP_STADISCONNECTED:
            xSemaphoreTake(s_net_mux, portMAX_DELAY);
            if (s_status.ap_sta_count > 0) s_status.ap_sta_count--;
            xSemaphoreGive(s_net_mux);
            break;

        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_retry_cnt = 0;
        if (s_reconnect_timer) {
            esp_timer_stop(s_reconnect_timer);
        }

        wifi_ap_record_t ap_info;
        int8_t rssi = -50;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            rssi = ap_info.rssi;
        }

        xSemaphoreTake(s_net_mux, portMAX_DELAY);
        s_status.wifi_state = NET_CONNECTED;
        s_status.rssi = rssi;
        snprintf(s_status.ip_str, sizeof(s_status.ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        xSemaphoreGive(s_net_mux);

        ESP_LOGI(TAG, "Wi-Fi connected, IP=%s, RSSI=%d", s_status.ip_str, rssi);
        start_or_restart_sntp();
    }
}

esp_err_t app_wifi_ntp_init(void)
{
    s_net_mux = xSemaphoreCreateMutex();
    s_scan_mux = xSemaphoreCreateMutex();
    memset(&s_status, 0, sizeof(s_status));

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop failed");

    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_status.ap_ssid, sizeof(s_status.ap_ssid), "AI-Terminal-%02X%02X", mac[4], mac[5]);

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL),
                        TAG, "reg wifi evt failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL),
                        TAG, "reg ip evt failed");

    esp_timer_create_args_t t_args = {
        .callback = reconnect_timer_cb,
        .name = "wifi_reconn",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&t_args, &s_reconnect_timer), TAG, "reconn timer failed");

    app_settings_t settings;
    app_settings_get(&settings);
    if (settings.wifi_ssid[0] != '\0') {
        wifi_config_t sta_cfg = {0};
        strlcpy((char *)sta_cfg.sta.ssid, settings.wifi_ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char *)sta_cfg.sta.password, settings.wifi_pass, sizeof(sta_cfg.sta.password));
        sta_cfg.sta.threshold.authmode = (settings.wifi_pass[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set STA failed");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg), TAG, "set STA cfg failed");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
        s_wifi_started = true;
        ESP_LOGI(TAG, "Auto-connecting to saved SSID '%s'", settings.wifi_ssid);
    } else {
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set STA failed");
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
        s_wifi_started = true;
        ESP_LOGI(TAG, "No saved Wi-Fi; standing by (AP SSID=%s)", s_status.ap_ssid);
    }

    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    app_http_server_start();
    return ESP_OK;
}

void app_wifi_get_status(net_status_t *out)
{
    if (!out || !s_net_mux) return;
    xSemaphoreTake(s_net_mux, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_net_mux);
}

esp_err_t app_wifi_start_portal(void)
{
    if (s_portal_active) return ESP_OK;

    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
    }
    s_portal_active = true;
    esp_wifi_disconnect();

    xSemaphoreTake(s_net_mux, portMAX_DELAY);
    s_status.wifi_state = NET_PORTAL_AP;
    s_status.ap_sta_count = 0;
    s_status.ip_str[0] = '\0';
    xSemaphoreGive(s_net_mux);

    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid, s_status.ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = (uint8_t)strlen(s_status.ap_ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set APSTA failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "set AP cfg failed");
    if (!s_wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
        s_wifi_started = true;
    }

    ESP_RETURN_ON_ERROR(app_portal_start(), TAG, "portal start failed");
    ESP_LOGI(TAG, "Captive Portal active on open AP '%s' (192.168.4.1)", s_status.ap_ssid);
    return ESP_OK;
}

esp_err_t app_wifi_stop_portal_and_connect(const char *ssid, const char *pass,
                                           const char *tz_str, const char *tz_label,
                                           const char *ntp_server)
{
    app_settings_save_network(ssid, pass, tz_str, tz_label, ntp_server);

    app_settings_t cfg;
    app_settings_get(&cfg);
    setenv("TZ", cfg.tz_str, 1);
    tzset();

    app_portal_stop();
    s_portal_active = false;
    s_retry_cnt = 0;

    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }

    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid, cfg.wifi_ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, cfg.wifi_pass, sizeof(sta_cfg.sta.password));
    sta_cfg.sta.threshold.authmode = (cfg.wifi_pass[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set STA failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg), TAG, "set STA cfg failed");

    xSemaphoreTake(s_net_mux, portMAX_DELAY);
    s_status.wifi_state = NET_CONNECTING;
    strlcpy(s_status.ssid, cfg.wifi_ssid, sizeof(s_status.ssid));
    s_status.ap_sta_count = 0;
    s_status.ip_str[0] = '\0';
    xSemaphoreGive(s_net_mux);

    esp_err_t err = esp_wifi_connect();
    ESP_LOGI(TAG, "Portal closed, connecting to '%s'", cfg.wifi_ssid);
    return err;
}

esp_err_t app_wifi_cancel_portal(void)
{
    if (!s_portal_active) return ESP_OK;

    app_portal_stop();
    s_portal_active = false;
    s_retry_cnt = 0;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "restore STA failed");

    app_settings_t cfg;
    app_settings_get(&cfg);
    if (cfg.wifi_ssid[0] != '\0') {
        wifi_config_t sta_cfg = {0};
        strlcpy((char *)sta_cfg.sta.ssid, cfg.wifi_ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char *)sta_cfg.sta.password, cfg.wifi_pass, sizeof(sta_cfg.sta.password));
        sta_cfg.sta.threshold.authmode = (cfg.wifi_pass[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);

        xSemaphoreTake(s_net_mux, portMAX_DELAY);
        s_status.wifi_state = NET_CONNECTING;
        strlcpy(s_status.ssid, cfg.wifi_ssid, sizeof(s_status.ssid));
        s_status.ap_sta_count = 0;
        xSemaphoreGive(s_net_mux);

        esp_wifi_connect();
    } else {
        xSemaphoreTake(s_net_mux, portMAX_DELAY);
        s_status.wifi_state = NET_IDLE;
        s_status.ssid[0] = '\0';
        s_status.ap_sta_count = 0;
        xSemaphoreGive(s_net_mux);
    }
    return ESP_OK;
}

esp_err_t app_wifi_forget_and_disconnect(void)
{
    if (s_reconnect_timer) {
        esp_timer_stop(s_reconnect_timer);
    }
    if (s_portal_active) {
        app_portal_stop();
        s_portal_active = false;
        esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }
    esp_wifi_disconnect();
    app_settings_clear_wifi();

    xSemaphoreTake(s_net_mux, portMAX_DELAY);
    s_status.wifi_state = NET_IDLE;
    s_status.ssid[0] = '\0';
    s_status.ip_str[0] = '\0';
    s_status.rssi = 0;
    s_status.ntp_syncing = false;
    xSemaphoreGive(s_net_mux);

    return ESP_OK;
}

esp_err_t app_ntp_sync_now(void)
{
    net_status_t st;
    app_wifi_get_status(&st);
    if (st.wifi_state != NET_CONNECTED) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_net_mux, portMAX_DELAY);
    s_status.ntp_syncing = true;
    xSemaphoreGive(s_net_mux);

    if (esp_sntp_enabled()) {
        esp_sntp_restart();
    } else {
        start_or_restart_sntp();
    }
    return ESP_OK;
}

esp_err_t app_wifi_scan_networks(wifi_ap_record_t *out_aps, uint16_t max_aps, uint16_t *out_cnt)
{
    ESP_RETURN_ON_FALSE(out_aps && out_cnt && max_aps > 0, ESP_ERR_INVALID_ARG, TAG, "bad scan arg");
    *out_cnt = 0;

    if (xSemaphoreTake(s_scan_mux, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err == ESP_OK) {
        uint16_t num = max_aps;
        err = esp_wifi_scan_get_ap_records(&num, out_aps);
        if (err == ESP_OK) {
            *out_cnt = num;
        }
    }

    xSemaphoreGive(s_scan_mux);
    return err;
}

void app_wifi_set_low_power(bool low_power)
{
    if (!s_wifi_started || s_portal_active) {
        return;
    }
    esp_wifi_set_ps(low_power ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM);
}
