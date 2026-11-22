#ifndef APP_SETTINGS_H
#define APP_SETTINGS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char tz_str[48];
    char tz_label[24];
    char ntp_server[64];
    uint8_t brightness;
    uint8_t rot_mode;
    uint8_t locked_rot;
    uint16_t sleep_timeout_sec;
    uint8_t volume;
    bool muted;
    bool alarm_enabled;
    uint8_t alarm_hour;
    uint8_t alarm_min;
} app_settings_t;

esp_err_t app_settings_init(void);
void app_settings_get(app_settings_t *out);

esp_err_t app_settings_save_network(const char *ssid, const char *pass,
                                    const char *tz_str, const char *tz_label,
                                    const char *ntp_server);
esp_err_t app_settings_clear_wifi(void);
esp_err_t app_settings_set_brightness(uint8_t brightness);
esp_err_t app_settings_set_rot_mode(uint8_t rot_mode);
esp_err_t app_settings_set_locked_rot(uint8_t locked_rot);
esp_err_t app_settings_set_sleep_timeout(uint16_t sec);
esp_err_t app_settings_set_volume(uint8_t volume);
esp_err_t app_settings_set_muted(bool muted);
esp_err_t app_settings_set_alarm(bool enabled, uint8_t hour, uint8_t min);

#ifdef __cplusplus
}
#endif

#endif /* APP_SETTINGS_H */
