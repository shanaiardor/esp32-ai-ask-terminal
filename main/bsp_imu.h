#ifndef BSP_IMU_H
#define BSP_IMU_H

#include <stdbool.h>
#include "esp_err.h"
#include "board_config.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t bsp_imu_init(void);
esp_err_t bsp_imu_read_accel(float *ax, float *ay, float *az);
bool bsp_imu_poll_orientation(rot_mode_t mode, disp_rot_t cur_rot, disp_rot_t *out_rot);
void bsp_imu_get_last_accel(float *ax, float *ay, float *az);

esp_err_t bsp_imu_enter_wom(uint8_t threshold_mg);
esp_err_t bsp_imu_exit_wom(void);
bool bsp_imu_check_wom(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_IMU_H */
