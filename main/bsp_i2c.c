#include "bsp_i2c.h"
#include "board_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_check.h"
#include <string.h>

static const char *TAG = "bsp_i2c";

#define I2C_XFER_TIMEOUT_MS     200
#define TCA9554_REG_INPUT       0x00
#define TCA9554_REG_OUTPUT      0x01
#define TCA9554_REG_POLARITY    0x02
#define TCA9554_REG_CONFIG      0x03

static SemaphoreHandle_t s_i2c0_mutex = NULL;
static SemaphoreHandle_t s_i2c1_mutex = NULL;
static i2c_master_bus_handle_t s_bus0 = NULL;
static i2c_master_bus_handle_t s_bus1 = NULL;
static i2c_master_dev_handle_t s_dev_exio = NULL;
static i2c_master_dev_handle_t s_dev_rtc = NULL;
static i2c_master_dev_handle_t s_dev_imu = NULL;
static i2c_master_dev_handle_t s_dev_es8311 = NULL;
static i2c_master_dev_handle_t s_dev_touch = NULL;

static uint8_t s_exio_dir = 0xFF;  /* 1=input, 0=output */
static uint8_t s_exio_out = 0xFF;

static i2c_master_dev_handle_t get_dev_handle(bsp_i2c0_dev_t dev)
{
    switch (dev) {
    case I2C0_DEV_EXIO:   return s_dev_exio;
    case I2C0_DEV_RTC:    return s_dev_rtc;
    case I2C0_DEV_IMU:    return s_dev_imu;
    case I2C0_DEV_ES8311: return s_dev_es8311;
    default:              return NULL;
    }
}

esp_err_t bsp_i2c_init(void)
{
    if (s_i2c0_mutex == NULL) {
        s_i2c0_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_i2c0_mutex != NULL, ESP_ERR_NO_MEM, TAG, "mutex0 alloc failed");
    }
    if (s_i2c1_mutex == NULL) {
        s_i2c1_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_i2c1_mutex != NULL, ESP_ERR_NO_MEM, TAG, "mutex1 alloc failed");
    }

    i2c_master_bus_config_t bus0_cfg = {
        .i2c_port = SYS_I2C_PORT,
        .sda_io_num = PIN_SYS_I2C_SDA,
        .scl_io_num = PIN_SYS_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus0_cfg, &s_bus0), TAG, "init I2C0 failed");

    i2c_master_bus_config_t bus1_cfg = {
        .i2c_port = TOUCH_I2C_PORT,
        .sda_io_num = PIN_TOUCH_I2C_SDA,
        .scl_io_num = PIN_TOUCH_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus1_cfg, &s_bus1), TAG, "init I2C1 failed");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = I2C_ADDR_TCA9554,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus0, &dev_cfg, &s_dev_exio), TAG, "add TCA9554 failed");

    dev_cfg.device_address = I2C_ADDR_PCF85063;
    dev_cfg.scl_speed_hz = 300000;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus0, &dev_cfg, &s_dev_rtc), TAG, "add PCF85063 failed");

    dev_cfg.device_address = I2C_ADDR_QMI8658;
    dev_cfg.scl_speed_hz = 300000;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus0, &dev_cfg, &s_dev_imu), TAG, "add QMI8658 failed");

    dev_cfg.device_address = I2C_ADDR_ES8311;
    dev_cfg.scl_speed_hz = 300000;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus0, &dev_cfg, &s_dev_es8311), TAG, "add ES8311 failed");

    dev_cfg.device_address = I2C_ADDR_TOUCH;
    dev_cfg.scl_speed_hz = 300000;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus1, &dev_cfg, &s_dev_touch), TAG, "add Touch failed");

    ESP_LOGI(TAG, "I2C0 (EXIO/RTC/IMU/ES8311) & I2C1 (Touch) initialized");
    return ESP_OK;
}

esp_err_t bsp_i2c0_write_reg(bsp_i2c0_dev_t dev, int reg, const uint8_t *data, size_t len)
{
    i2c_master_dev_handle_t handle = get_dev_handle(dev);
    ESP_RETURN_ON_FALSE(handle != NULL && data != NULL && len > 0 && len <= 32, ESP_ERR_INVALID_ARG, TAG, "bad write arg");

    if (xSemaphoreTake(s_i2c0_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret;
    if (reg < 0) {
        ret = i2c_master_transmit(handle, data, len, I2C_XFER_TIMEOUT_MS);
    } else {
        uint8_t buf[33];
        buf[0] = (uint8_t)reg;
        memcpy(&buf[1], data, len);
        ret = i2c_master_transmit(handle, buf, len + 1, I2C_XFER_TIMEOUT_MS);
    }

    xSemaphoreGive(s_i2c0_mutex);
    return ret;
}

esp_err_t bsp_i2c0_read_reg(bsp_i2c0_dev_t dev, int reg, uint8_t *data, size_t len)
{
    i2c_master_dev_handle_t handle = get_dev_handle(dev);
    ESP_RETURN_ON_FALSE(handle != NULL && data != NULL && len > 0, ESP_ERR_INVALID_ARG, TAG, "bad read arg");

    if (xSemaphoreTake(s_i2c0_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret;
    if (reg < 0) {
        ret = i2c_master_receive(handle, data, len, I2C_XFER_TIMEOUT_MS);
    } else {
        uint8_t r = (uint8_t)reg;
        ret = i2c_master_transmit_receive(handle, &r, 1, data, len, I2C_XFER_TIMEOUT_MS);
    }

    xSemaphoreGive(s_i2c0_mutex);
    return ret;
}

esp_err_t bsp_i2c1_touch_xfer(const uint8_t *tx_buf, size_t tx_len, uint8_t *rx_buf, size_t rx_len)
{
    ESP_RETURN_ON_FALSE(s_dev_touch != NULL && s_i2c1_mutex != NULL, ESP_ERR_INVALID_STATE, TAG, "touch not init");
    if (xSemaphoreTake(s_i2c1_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    /* Match Espressif esp_lcd_touch_axs15231b & ESPHome: separate WRITE (STOP) then READ (START) */
    esp_err_t ret = i2c_master_transmit(s_dev_touch, tx_buf, tx_len, 80);
    if (ret == ESP_OK) {
        ret = i2c_master_receive(s_dev_touch, rx_buf, rx_len, 80);
    }
    xSemaphoreGive(s_i2c1_mutex);
    return ret;
}

esp_err_t bsp_exio_init(void)
{
    /* Set outputs first before switching pin direction to avoid glitches:
       BL_EN=0 (backlight off initially), LCD_RST=1, SYS_EN=1, NS_MODE=1 (speaker PA enable) */
    s_exio_out = EXIO_PIN_LCD_RST | EXIO_PIN_SYS_EN | EXIO_PIN_NS_MODE;
    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_EXIO, TCA9554_REG_OUTPUT, &s_exio_out, 1), TAG, "exio write out failed");

    uint8_t polarity = 0x00;
    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_EXIO, TCA9554_REG_POLARITY, &polarity, 1), TAG, "exio write pol failed");

    uint8_t out_mask = EXIO_PIN_BL_EN | EXIO_PIN_LCD_RST | EXIO_PIN_SYS_EN | EXIO_PIN_NS_MODE;
    s_exio_dir = (uint8_t)(~out_mask);
    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_EXIO, TCA9554_REG_CONFIG, &s_exio_dir, 1), TAG, "exio write dir failed");

    uint8_t init_in = 0;
    (void)bsp_i2c0_read_reg(I2C0_DEV_EXIO, TCA9554_REG_INPUT, &init_in, 1);

    ESP_LOGI(TAG, "TCA9554 initialized (dir=0x%02X, out=0x%02X, in=0x%02X)", s_exio_dir, s_exio_out, init_in);
    return ESP_OK;
}

esp_err_t bsp_exio_set_level(uint8_t mask, bool level)
{
    if (s_dev_exio == NULL || s_i2c0_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_i2c0_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    uint8_t next_out = level ? (uint8_t)(s_exio_out | mask) : (uint8_t)(s_exio_out & (uint8_t)(~mask));
    if (next_out == s_exio_out) {
        xSemaphoreGive(s_i2c0_mutex);
        return ESP_OK;
    }

    uint8_t buf[2] = {TCA9554_REG_OUTPUT, next_out};
    esp_err_t ret = i2c_master_transmit(s_dev_exio, buf, sizeof(buf), I2C_XFER_TIMEOUT_MS);
    if (ret == ESP_OK) {
        s_exio_out = next_out;
    }
    xSemaphoreGive(s_i2c0_mutex);
    return ret;
}

esp_err_t bsp_exio_read_inputs(uint8_t *out_val)
{
    ESP_RETURN_ON_FALSE(out_val != NULL, ESP_ERR_INVALID_ARG, TAG, "null out_val");
    return bsp_i2c0_read_reg(I2C0_DEV_EXIO, TCA9554_REG_INPUT, out_val, 1);
}
