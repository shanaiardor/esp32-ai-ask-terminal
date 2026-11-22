#ifndef BSP_RTC_H
#define BSP_RTC_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bsp_rtc_init(void);
esp_err_t bsp_rtc_read(struct tm *out_tm, bool *out_valid);
esp_err_t bsp_rtc_write(const struct tm *tm_in);
void bsp_rtc_sync_to_system(bool *out_valid);
esp_err_t bsp_rtc_sync_from_system(void);

esp_err_t bsp_rtc_set_alarm(bool enable, uint8_t hour, uint8_t minute, uint8_t second);
esp_err_t bsp_rtc_arm_test_alarm_sec(uint32_t delta_sec);
bool bsp_rtc_check_and_clear_alarm(void);
bool bsp_rtc_consume_test_alarm_flag(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_RTC_H */
