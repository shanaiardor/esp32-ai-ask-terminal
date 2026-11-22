#ifndef UI_TERMINAL_H
#define UI_TERMINAL_H

#include <stdbool.h>
#include <stdint.h>
#include "board_config.h"
#include "bsp_power.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_terminal_init(bool rtc_valid);
void ui_terminal_start_boot_anim(void);
void ui_terminal_play_shutdown_anim(void);
void ui_terminal_request_refresh(void);
void ui_terminal_set_sleeping(bool sleeping);
void ui_terminal_notify_button(bsp_btn_event_t evt);
void ui_terminal_notify_imu_rot(disp_rot_t rot);
void ui_terminal_notify_battery(float vbat, uint8_t pct, bool usb_powered);
void ui_terminal_notify_alarm_fired(void);
bool ui_terminal_take_shutdown_request(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_TERMINAL_H */
