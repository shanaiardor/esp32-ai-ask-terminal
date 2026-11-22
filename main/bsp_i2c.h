#ifndef BSP_I2C_H
#define BSP_I2C_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    I2C0_DEV_EXIO = 0,
    I2C0_DEV_RTC,
    I2C0_DEV_IMU,
    I2C0_DEV_ES8311,
} bsp_i2c0_dev_t;

esp_err_t bsp_i2c_init(void);

esp_err_t bsp_i2c0_write_reg(bsp_i2c0_dev_t dev, int reg, const uint8_t *data, size_t len);
esp_err_t bsp_i2c0_read_reg(bsp_i2c0_dev_t dev, int reg, uint8_t *data, size_t len);
esp_err_t bsp_i2c1_touch_xfer(const uint8_t *tx_buf, size_t tx_len, uint8_t *rx_buf, size_t rx_len);

esp_err_t bsp_exio_init(void);
esp_err_t bsp_exio_set_level(uint8_t mask, bool level);
esp_err_t bsp_exio_read_inputs(uint8_t *out_val);

#ifdef __cplusplus
}
#endif

#endif /* BSP_I2C_H */
