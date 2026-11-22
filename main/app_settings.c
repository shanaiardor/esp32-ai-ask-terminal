#include "app_settings.h"
#include "bsp_rtc.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "app_settings";
static const char *NVS_NS = "aiterm";
static const char *FALLBACK_NVS_NS = "deskclock";

static SemaphoreHandle_t s_mux = NULL;
static esp_timer_handle_t s_nvs_timer = NULL;
static app_settings_t s_cfg;
static volatile bool s_alarm_hw_dirty = false;

static void deferred_nvs_flush_cb(void *arg)
{
    (void)arg;
    app_settings_t snap;
    bool sync_alarm = false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    snap = s_cfg;
    if (s_alarm_hw_dirty) {
        s_alarm_hw_dirty = false;
        sync_alarm = true;
    }
    xSemaphoreGive(s_mux);

    if (sync_alarm) {
        bsp_rtc_set_alarm(snap.alarm_enabled, snap.alarm_hour, snap.alarm_min, 0);
    }

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "bright", snap.brightness);
        nvs_set_u8(h, "rot_mode", snap.rot_mode);
        nvs_set_u8(h, "lock_rot", snap.locked_rot);
        nvs_set_u16(h, "slp_sec", snap.sleep_timeout_sec);
        nvs_set_u8(h, "vol", snap.volume);
        nvs_set_u8(h, "muted", snap.muted ? 1 : 0);
        nvs_set_u8(h, "alm_en", snap.alarm_enabled ? 1 : 0);
        nvs_set_u8(h, "alm_hr", snap.alarm_hour);
        nvs_set_u8(h, "alm_min", snap.alarm_min);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void schedule_nvs_flush(void)
{
    if (s_nvs_timer) {
        esp_timer_stop(s_nvs_timer);
        esp_timer_start_once(s_nvs_timer, 600 * 1000ULL);
    }
}

static void set_defaults(app_settings_t *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->tz_str, sizeof(c->tz_str), "CST-8");
    snprintf(c->tz_label, sizeof(c->tz_label), "UTC+8 北京");
    snprintf(c->ntp_server, sizeof(c->ntp_server), "ntp.aliyun.com");
    c->brightness = 90;
    c->rot_mode = ROT_MODE_LANDSCAPE_ONLY;
    c->locked_rot = DISP_ROT_90;
    c->sleep_timeout_sec = 60; /* 1-minute screen sleep by default */
    c->volume = 78;
    c->muted = false;
    c->alarm_enabled = false;
    c->alarm_hour = 8;
    c->alarm_min = 0;
}

static void read_nvs_str(nvs_handle_t h, const char *key, char *dst, size_t dst_len)
{
    size_t len = dst_len;
    if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
        /* keep existing */
    }
    dst[dst_len - 1] = '\0';
}

esp_err_t app_settings_init(void)
{
    if (!s_mux) {
        s_mux = xSemaphoreCreateMutex();
    }
    set_defaults(&s_cfg);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_OK) {
        read_nvs_str(h, "wifi_ssid", s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid));
        read_nvs_str(h, "wifi_pass", s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass));
        read_nvs_str(h, "tz_str", s_cfg.tz_str, sizeof(s_cfg.tz_str));
        read_nvs_str(h, "tz_label", s_cfg.tz_label, sizeof(s_cfg.tz_label));
        read_nvs_str(h, "ntp_srv", s_cfg.ntp_server, sizeof(s_cfg.ntp_server));

        uint8_t v = 0;
        if (nvs_get_u8(h, "bright", &v) == ESP_OK && v >= 10 && v <= 100) s_cfg.brightness = v;
        if (nvs_get_u8(h, "rot_mode", &v) == ESP_OK && v <= ROT_MODE_LOCKED) s_cfg.rot_mode = v;
        if (nvs_get_u8(h, "lock_rot", &v) == ESP_OK && v <= DISP_ROT_270) s_cfg.locked_rot = v;
        if (nvs_get_u8(h, "vol", &v) == ESP_OK && v <= 100) s_cfg.volume = v;
        if (nvs_get_u8(h, "muted", &v) == ESP_OK) s_cfg.muted = (v != 0);
        if (nvs_get_u8(h, "alm_en", &v) == ESP_OK) s_cfg.alarm_enabled = (v != 0);
        if (nvs_get_u8(h, "alm_hr", &v) == ESP_OK && v < 24) s_cfg.alarm_hour = v;
        if (nvs_get_u8(h, "alm_min", &v) == ESP_OK && v < 60) s_cfg.alarm_min = v;

        uint16_t u16v = 0;
        if (nvs_get_u16(h, "slp_sec", &u16v) == ESP_OK && u16v <= 3600) s_cfg.sleep_timeout_sec = u16v;
        nvs_close(h);
    }

    if (s_cfg.rot_mode == ROT_MODE_AUTO_4WAY) {
        s_cfg.rot_mode = ROT_MODE_LANDSCAPE_ONLY;
    }
    if (s_cfg.locked_rot != DISP_ROT_90 && s_cfg.locked_rot != DISP_ROT_270) {
        s_cfg.locked_rot = DISP_ROT_90;
    }

    if (!s_nvs_timer) {
        esp_timer_create_args_t t_args = {
            .callback = deferred_nvs_flush_cb,
            .name = "nvs_flush",
        };
        (void)esp_timer_create(&t_args, &s_nvs_timer);
    }

    /* If no Wi-Fi saved in aiterm namespace yet, migrate credentials from previous deskclock NVS if present */
    if (s_cfg.wifi_ssid[0] == '\0') {
        nvs_handle_t fh;
        if (nvs_open(FALLBACK_NVS_NS, NVS_READONLY, &fh) == ESP_OK) {
            read_nvs_str(fh, "wifi_ssid", s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid));
            read_nvs_str(fh, "wifi_pass", s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass));
            nvs_close(fh);
            if (s_cfg.wifi_ssid[0] != '\0') {
                ESP_LOGI(TAG, "Migrated Wi-Fi credentials '%s' from NVS", s_cfg.wifi_ssid);
            }
        }
    }

    ESP_LOGI(TAG, "Settings loaded: ssid='%s', bright=%u, vol=%u, mute=%d, sleep=%us",
             s_cfg.wifi_ssid, s_cfg.brightness, s_cfg.volume, s_cfg.muted ? 1 : 0, s_cfg.sleep_timeout_sec);
    return ESP_OK;
}

void app_settings_get(app_settings_t *out)
{
    if (!out) return;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_mux);
}

esp_err_t app_settings_save_network(const char *ssid, const char *pass,
                                    const char *tz_str, const char *tz_label,
                                    const char *ntp_server)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (ssid) strlcpy(s_cfg.wifi_ssid, ssid, sizeof(s_cfg.wifi_ssid));
    if (pass) strlcpy(s_cfg.wifi_pass, pass, sizeof(s_cfg.wifi_pass));
    if (tz_str && tz_str[0]) strlcpy(s_cfg.tz_str, tz_str, sizeof(s_cfg.tz_str));
    if (tz_label && tz_label[0]) strlcpy(s_cfg.tz_label, tz_label, sizeof(s_cfg.tz_label));
    if (ntp_server && ntp_server[0]) strlcpy(s_cfg.ntp_server, ntp_server, sizeof(s_cfg.ntp_server));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_set_str(h, "wifi_ssid", s_cfg.wifi_ssid);
        nvs_set_str(h, "wifi_pass", s_cfg.wifi_pass);
        nvs_set_str(h, "tz_str", s_cfg.tz_str);
        nvs_set_str(h, "tz_label", s_cfg.tz_label);
        nvs_set_str(h, "ntp_srv", s_cfg.ntp_server);
        err = nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(s_mux);
    return err;
}

esp_err_t app_settings_clear_wifi(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_cfg.wifi_ssid[0] = '\0';
    s_cfg.wifi_pass[0] = '\0';
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        nvs_erase_key(h, "wifi_ssid");
        nvs_erase_key(h, "wifi_pass");
        err = nvs_commit(h);
        nvs_close(h);
    }
    xSemaphoreGive(s_mux);
    return err;
}

esp_err_t app_settings_set_brightness(uint8_t brightness)
{
    if (brightness < 10) brightness = 10;
    if (brightness > 100) brightness = 100;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_cfg.brightness = brightness;
    xSemaphoreGive(s_mux);
    schedule_nvs_flush();
    return ESP_OK;
}

esp_err_t app_settings_set_rot_mode(uint8_t rot_mode)
{
    if (rot_mode != ROT_MODE_LANDSCAPE_ONLY && rot_mode != ROT_MODE_LOCKED) {
        rot_mode = ROT_MODE_LANDSCAPE_ONLY;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_cfg.rot_mode = rot_mode;
    xSemaphoreGive(s_mux);
    schedule_nvs_flush();
    return ESP_OK;
}

esp_err_t app_settings_set_locked_rot(uint8_t locked_rot)
{
    if (locked_rot != DISP_ROT_90 && locked_rot != DISP_ROT_270) {
        locked_rot = DISP_ROT_90;
    }
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_cfg.locked_rot = locked_rot;
    xSemaphoreGive(s_mux);
    schedule_nvs_flush();
    return ESP_OK;
}

esp_err_t app_settings_set_sleep_timeout(uint16_t sec)
{
    if (sec > 3600) sec = 0;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_cfg.sleep_timeout_sec = sec;
    xSemaphoreGive(s_mux);
    schedule_nvs_flush();
    return ESP_OK;
}

esp_err_t app_settings_set_volume(uint8_t volume)
{
    if (volume > 100) volume = 100;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_cfg.volume = volume;
    xSemaphoreGive(s_mux);
    schedule_nvs_flush();
    return ESP_OK;
}

esp_err_t app_settings_set_muted(bool muted)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_cfg.muted = muted;
    xSemaphoreGive(s_mux);
    schedule_nvs_flush();
    return ESP_OK;
}

esp_err_t app_settings_set_alarm(bool enabled, uint8_t hour, uint8_t min)
{
    hour %= 24;
    min %= 60;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_cfg.alarm_enabled = enabled;
    s_cfg.alarm_hour = hour;
    s_cfg.alarm_min = min;
    s_alarm_hw_dirty = true;
    xSemaphoreGive(s_mux);
    schedule_nvs_flush();
    return ESP_OK;
}
