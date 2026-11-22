#ifndef BSP_DISPLAY_H
#define BSP_DISPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

void bsp_display_early_bl_off(void);
esp_err_t bsp_display_init(disp_rot_t initial_rot);

bool bsp_display_lock(int timeout_ms);
void bsp_display_unlock(void);

void bsp_display_set_rotation(disp_rot_t new_rot);
disp_rot_t bsp_display_get_rotation(void);

void bsp_display_set_brightness(uint8_t pct);
uint8_t bsp_display_get_brightness(void);
void bsp_display_fade_in(uint8_t base_pct, uint32_t duration_ms);
void bsp_display_fade_out(uint32_t duration_ms);
void bsp_display_set_power_profile(bool battery_mode, bool dimmed);
void bsp_display_sleep(bool sleep_on);
bool bsp_display_is_sleeping(void);
void bsp_display_notify_touch_irq(void);
void bsp_display_wake_lvgl_task(void);
void bsp_display_pm_boost(uint32_t duration_ms);
void bsp_display_reset_idle_timer(void);
uint32_t bsp_display_get_idle_sec(void);
bool bsp_display_probe_touch(void);
esp_err_t bsp_display_capture_bmp(uint8_t **out_buf, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* BSP_DISPLAY_H */
