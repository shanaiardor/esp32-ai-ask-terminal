#ifndef BSP_POWER_H
#define BSP_POWER_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_IRQ_EXIO    (1U << 0)
#define BSP_IRQ_BTN     (1U << 1)

typedef enum {
    BTN_EVT_NONE = 0,
    BTN_EVT_BOOT_SHORT,
    BTN_EVT_BOOT_LONG,
    BTN_EVT_PWR_SHORT,
    BTN_EVT_PWR_LONG,
} bsp_btn_event_t;

esp_err_t bsp_power_init(void);
void bsp_power_sample_battery(void);
float bsp_power_get_voltage(void);
uint8_t bsp_power_get_battery_pct(void);
bool bsp_power_is_usb_powered(void);
void bsp_power_shutdown(void);

bsp_btn_event_t bsp_power_poll_buttons(void);
void bsp_power_register_wake_task(void *task_handle);
uint32_t bsp_power_wait_irq(uint32_t timeout_ms);
bool bsp_power_buttons_busy(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_POWER_H */
