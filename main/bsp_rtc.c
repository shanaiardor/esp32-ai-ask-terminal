#include "bsp_rtc.h"
#include "bsp_i2c.h"
#include "esp_log.h"
#include "esp_check.h"
#include <sys/time.h>
#include <string.h>

static const char *TAG = "bsp_rtc";

#define PCF85063_REG_CTRL1      0x00
#define PCF85063_REG_CTRL2      0x01
#define PCF85063_REG_RAM        0x03
#define PCF85063_REG_SEC        0x04
#define PCF85063_REG_ALRM_SEC   0x0B

#define PCF85063_CTRL2_AIE      (1U << 7)
#define PCF85063_CTRL2_AF       (1U << 6)

static bool s_test_alarm_active = false;
static bool s_test_alarm_fired = false;

static uint8_t bcd2dec(uint8_t val)
{
    return (uint8_t)(((val >> 4) * 10) + (val & 0x0F));
}

static uint8_t dec2bcd(uint8_t val)
{
    return (uint8_t)(((val / 10) << 4) | (val % 10));
}

esp_err_t bsp_rtc_init(void)
{
    uint8_t ctrl1 = 0;
    ESP_RETURN_ON_ERROR(bsp_i2c0_read_reg(I2C0_DEV_RTC, PCF85063_REG_CTRL1, &ctrl1, 1), TAG, "read CTRL1 failed");

    /* Ensure STOP (bit5)=0, 24h mode (bit1)=0, CIE=0 */
    uint8_t new_ctrl1 = ctrl1 & (uint8_t)(~((1U << 5) | (1U << 1) | (1U << 7)));
    if (new_ctrl1 != ctrl1) {
        ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_RTC, PCF85063_REG_CTRL1, &new_ctrl1, 1), TAG, "write CTRL1 failed");
    }

    ESP_LOGI(TAG, "PCF85063 RTC initialized (CTRL1=0x%02X)", new_ctrl1);
    return ESP_OK;
}

esp_err_t bsp_rtc_read(struct tm *out_tm, bool *out_valid)
{
    ESP_RETURN_ON_FALSE(out_tm != NULL, ESP_ERR_INVALID_ARG, TAG, "null out_tm");

    uint8_t buf[7] = {0};
    ESP_RETURN_ON_ERROR(bsp_i2c0_read_reg(I2C0_DEV_RTC, PCF85063_REG_SEC, buf, sizeof(buf)), TAG, "read time failed");

    bool os_lost = (buf[0] & 0x80U) != 0;
    uint8_t sec   = bcd2dec(buf[0] & 0x7F);
    uint8_t min   = bcd2dec(buf[1] & 0x7F);
    uint8_t hour  = bcd2dec(buf[2] & 0x3F);
    uint8_t day   = bcd2dec(buf[3] & 0x3F);
    uint8_t wday  = buf[4] & 0x07;
    uint8_t month = bcd2dec(buf[5] & 0x1F);
    uint16_t year = (uint16_t)bcd2dec(buf[6]) + 2000;

    bool valid = (!os_lost) &&
                 (year >= 2024 && year <= 2099) &&
                 (month >= 1 && month <= 12) &&
                 (day >= 1 && day <= 31) &&
                 (hour < 24) && (min < 60) && (sec < 60);

    memset(out_tm, 0, sizeof(*out_tm));
    out_tm->tm_year = (int)year - 1900;
    out_tm->tm_mon  = (int)month - 1;
    out_tm->tm_mday = (int)day;
    out_tm->tm_hour = (int)hour;
    out_tm->tm_min  = (int)min;
    out_tm->tm_sec  = (int)sec;
    out_tm->tm_wday = (int)wday;
    out_tm->tm_isdst = -1;

    if (out_valid) {
        *out_valid = valid;
    }
    return ESP_OK;
}

esp_err_t bsp_rtc_write(const struct tm *tm_in)
{
    ESP_RETURN_ON_FALSE(tm_in != NULL, ESP_ERR_INVALID_ARG, TAG, "null tm_in");

    int year = tm_in->tm_year + 1900;
    if (year < 2000) year = 2000;
    if (year > 2099) year = 2099;

    uint8_t buf[7];
    buf[0] = dec2bcd((uint8_t)tm_in->tm_sec) & 0x7F; /* Clear OS flag */
    buf[1] = dec2bcd((uint8_t)tm_in->tm_min);
    buf[2] = dec2bcd((uint8_t)tm_in->tm_hour);
    buf[3] = dec2bcd((uint8_t)tm_in->tm_mday);
    buf[4] = (uint8_t)(tm_in->tm_wday & 0x07);
    buf[5] = dec2bcd((uint8_t)(tm_in->tm_mon + 1));
    buf[6] = dec2bcd((uint8_t)(year - 2000));

    return bsp_i2c0_write_reg(I2C0_DEV_RTC, PCF85063_REG_SEC, buf, sizeof(buf));
}

void bsp_rtc_sync_to_system(bool *out_valid)
{
    struct tm t;
    bool valid = false;
    esp_err_t err = bsp_rtc_read(&t, &valid);
    if (err == ESP_OK && valid) {
        time_t epoch = mktime(&t);
        if (epoch > 0) {
            struct timeval tv = {.tv_sec = epoch, .tv_usec = 0};
            settimeofday(&tv, NULL);
            ESP_LOGI(TAG, "Restored system time from PCF85063: %04d-%02d-%02d %02d:%02d:%02d",
                     t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                     t.tm_hour, t.tm_min, t.tm_sec);
            if (out_valid) *out_valid = true;
            return;
        }
    }

    /* Fallback default date: 2026-01-01 00:00:00 */
    struct tm def_tm = {
        .tm_year = 2026 - 1900,
        .tm_mon  = 0,
        .tm_mday = 1,
        .tm_hour = 0,
        .tm_min  = 0,
        .tm_sec  = 0,
        .tm_isdst = -1,
    };
    time_t def_epoch = mktime(&def_tm);
    struct timeval tv = {.tv_sec = def_epoch, .tv_usec = 0};
    settimeofday(&tv, NULL);
    ESP_LOGW(TAG, "PCF85063 time invalid (err=%s, os/range check=%d); set fallback 2026-01-01",
             esp_err_to_name(err), valid ? 1 : 0);
    if (out_valid) *out_valid = false;
}

esp_err_t bsp_rtc_sync_from_system(void)
{
    time_t now = 0;
    struct tm local_tm;
    time(&now);
    localtime_r(&now, &local_tm);

    esp_err_t err = bsp_rtc_write(&local_tm);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Synced system time -> PCF85063: %04d-%02d-%02d %02d:%02d:%02d",
                 local_tm.tm_year + 1900, local_tm.tm_mon + 1, local_tm.tm_mday,
                 local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec);
    } else {
        ESP_LOGE(TAG, "Failed to write PCF85063: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t bsp_rtc_set_alarm(bool enable, uint8_t hour, uint8_t minute, uint8_t second)
{
    uint8_t buf[5];
    if (enable) {
        buf[0] = dec2bcd((uint8_t)(second % 60)) & 0x7F;
        buf[1] = dec2bcd((uint8_t)(minute % 60)) & 0x7F;
        buf[2] = dec2bcd((uint8_t)(hour % 24)) & 0x3F;
        buf[3] = 0x80;
        buf[4] = 0x80;
    } else {
        memset(buf, 0x80, sizeof(buf));
    }

    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_RTC, PCF85063_REG_ALRM_SEC, buf, sizeof(buf)),
                        TAG, "write alarm regs failed");

    uint8_t ctrl2 = 0;
    ESP_RETURN_ON_ERROR(bsp_i2c0_read_reg(I2C0_DEV_RTC, PCF85063_REG_CTRL2, &ctrl2, 1),
                        TAG, "read CTRL2 failed");
    ctrl2 &= (uint8_t)(~(PCF85063_CTRL2_AF | PCF85063_CTRL2_AIE));
    if (enable) {
        ctrl2 |= PCF85063_CTRL2_AIE;
    }
    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_RTC, PCF85063_REG_CTRL2, &ctrl2, 1),
                        TAG, "write CTRL2 failed");

    ESP_LOGI(TAG, "PCF85063 hardware alarm %s (%02u:%02u:%02u)",
             enable ? "armed" : "disabled", hour, minute, second);
    return ESP_OK;
}

esp_err_t bsp_rtc_arm_test_alarm_sec(uint32_t delta_sec)
{
    struct tm t;
    bool valid = false;
    time_t ep = 0;
    if (bsp_rtc_read(&t, &valid) == ESP_OK && valid) {
        ep = mktime(&t);
    }
    if (ep <= 0) {
        time(&ep);
    }
    ep += (time_t)delta_sec;
    localtime_r(&ep, &t);

    s_test_alarm_active = true;
    s_test_alarm_fired = false;
    return bsp_rtc_set_alarm(true, (uint8_t)t.tm_hour, (uint8_t)t.tm_min, (uint8_t)t.tm_sec);
}

bool bsp_rtc_check_and_clear_alarm(void)
{
    uint8_t ctrl2 = 0;
    if (bsp_i2c0_read_reg(I2C0_DEV_RTC, PCF85063_REG_CTRL2, &ctrl2, 1) != ESP_OK) {
        return false;
    }
    if ((ctrl2 & PCF85063_CTRL2_AF) == 0) {
        return false;
    }

    ctrl2 &= (uint8_t)(~PCF85063_CTRL2_AF);
    bsp_i2c0_write_reg(I2C0_DEV_RTC, PCF85063_REG_CTRL2, &ctrl2, 1);

    if (s_test_alarm_active) {
        s_test_alarm_active = false;
        s_test_alarm_fired = true;
    }
    ESP_LOGI(TAG, "PCF85063 hardware alarm interrupt triggered!");
    return true;
}

bool bsp_rtc_consume_test_alarm_flag(void)
{
    bool v = s_test_alarm_fired;
    s_test_alarm_fired = false;
    return v;
}
