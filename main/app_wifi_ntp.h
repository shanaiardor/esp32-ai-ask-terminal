#ifndef APP_WIFI_NTP_H
#define APP_WIFI_NTP_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"
#include "esp_wifi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NET_IDLE = 0,
    NET_CONNECTING,
    NET_CONNECTED,
    NET_PORTAL_AP,
} wifi_net_state_t;

typedef struct {
    wifi_net_state_t wifi_state;
    char ssid[33];
    char ip_str[16];
    int8_t rssi;
    char ap_ssid[32];
    int ap_sta_count;
    bool ntp_synced;
    bool ntp_syncing;
    time_t last_sync_epoch;
} net_status_t;

esp_err_t app_wifi_ntp_init(void);
void app_wifi_get_status(net_status_t *out);

esp_err_t app_wifi_start_portal(void);
esp_err_t app_wifi_stop_portal_and_connect(const char *ssid, const char *pass,
                                           const char *tz_str, const char *tz_label,
                                           const char *ntp_server);
esp_err_t app_wifi_cancel_portal(void);
esp_err_t app_wifi_forget_and_disconnect(void);

esp_err_t app_ntp_sync_now(void);
esp_err_t app_wifi_scan_networks(wifi_ap_record_t *out_aps, uint16_t max_aps, uint16_t *out_cnt);
void app_wifi_set_low_power(bool low_power);

#ifdef __cplusplus
}
#endif

#endif /* APP_WIFI_NTP_H */
