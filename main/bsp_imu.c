#include "bsp_imu.h"
#include "bsp_i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include <math.h>

static const char *TAG = "bsp_imu";

/* QMI8658 Registers */
#define QMI_REG_WHO_AM_I    0x00
#define QMI_REG_CTRL1       0x02
#define QMI_REG_CTRL2       0x03
#define QMI_REG_CTRL5       0x06
#define QMI_REG_CTRL7       0x08
#define QMI_REG_CTRL8       0x09
#define QMI_REG_CTRL9       0x0A
#define QMI_REG_CAL1_L      0x0B
#define QMI_REG_CAL1_H      0x0C
#define QMI_REG_STATUS_INT  0x2D
#define QMI_REG_STATUS0     0x2E
#define QMI_REG_STATUS1     0x2F
#define QMI_REG_AX_L        0x35
#define QMI_REG_RESET       0x60

#define QMI_CMD_ACK         0x00
#define QMI_CMD_WRITE_WOM   0x08
#define QMI_ODR_LP_128HZ    0x0C
#define QMI_ODR_LP_21HZ     0x0D

#define QMI_WHO_AM_I_VAL    0x05
#define QMI_ACC_SCALE_4G    (4.0f / 32768.0f)

/*
 * Centralized axis-to-display-orientation mapping.
 * Adjust these macros if physical board orientation differs on your desk.
 */
#define IMU_SWAP_XY         1
#define IMU_SIGN_X          (-1.0f)
#define IMU_SIGN_Y          (-1.0f)

#define IMU_THRESH_GRAV     0.50f
#define IMU_THRESH_MARGIN   0.18f
#define IMU_THRESH_FLAT_Z   0.88f
#define IMU_STABLE_COUNT    5

static bool s_imu_ready = false;
static bool s_wom_active = false;
static bool s_filter_init = false;
static float s_fx = 0.0f, s_fy = 0.0f, s_fz = 1.0f;
static int s_log_samples = 0;
static disp_rot_t s_cand_rot = DISP_ROT_90;
static int s_cand_cnt = 0;

static esp_err_t qmi_issue_ctrl9(uint8_t cmd)
{
    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_IMU, QMI_REG_CTRL9, &cmd, 1), TAG, "CTRL9 write failed");

    uint8_t st = 0;
    bool done = false;
    for (int i = 0; i < 60; i++) {
        if (bsp_i2c0_read_reg(I2C0_DEV_IMU, QMI_REG_STATUS_INT, &st, 1) == ESP_OK && (st & 0x80U)) {
            done = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!done) {
        ESP_LOGW(TAG, "CTRL9 cmd 0x%02X ack timeout (st=0x%02X)", cmd, st);
        return ESP_ERR_TIMEOUT;
    }

    uint8_t ack = QMI_CMD_ACK;
    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_IMU, QMI_REG_CTRL9, &ack, 1), TAG, "CTRL9 ACK write failed");
    for (int i = 0; i < 60; i++) {
        if (bsp_i2c0_read_reg(I2C0_DEV_IMU, QMI_REG_STATUS_INT, &st, 1) == ESP_OK && !(st & 0x80U)) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return ESP_OK;
}

esp_err_t bsp_imu_init(void)
{
    uint8_t rst_cmd = 0xB0;
    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_IMU, QMI_REG_RESET, &rst_cmd, 1), TAG, "reset failed");
    vTaskDelay(pdMS_TO_TICKS(25));

    uint8_t who = 0;
    ESP_RETURN_ON_ERROR(bsp_i2c0_read_reg(I2C0_DEV_IMU, QMI_REG_WHO_AM_I, &who, 1), TAG, "read WHO_AM_I failed");
    if (who != QMI_WHO_AM_I_VAL) {
        ESP_LOGE(TAG, "Unexpected QMI8658 WHO_AM_I=0x%02X (expected 0x%02X)", who, QMI_WHO_AM_I_VAL);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t ctrl1 = 0x40; /* Auto-increment, little-endian */
    uint8_t ctrl2 = (1U << 4) | QMI_ODR_LP_128HZ; /* +-4G, Low-Power 128Hz ODR */
    uint8_t ctrl5 = 0x05; /* Accel LPF enable, mode 2 */
    uint8_t ctrl8 = 0x80; /* STATUS_INT.bit7 CTRL9 handshake */
    uint8_t ctrl7 = 0x21; /* DRDY_DIS=1, enable accelerometer only */

    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_IMU, QMI_REG_CTRL1, &ctrl1, 1), TAG, "CTRL1 failed");
    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_IMU, QMI_REG_CTRL2, &ctrl2, 1), TAG, "CTRL2 failed");
    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_IMU, QMI_REG_CTRL5, &ctrl5, 1), TAG, "CTRL5 failed");
    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_IMU, QMI_REG_CTRL8, &ctrl8, 1), TAG, "CTRL8 failed");
    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_IMU, QMI_REG_CTRL7, &ctrl7, 1), TAG, "CTRL7 failed");

    s_imu_ready = true;
    s_wom_active = false;
    ESP_LOGI(TAG, "QMI8658C initialized (WHO_AM_I=0x%02X, +-4G, LP 128Hz)", who);
    return ESP_OK;
}

esp_err_t bsp_imu_read_accel(float *ax, float *ay, float *az)
{
    if (!s_imu_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t raw[6] = {0};
    ESP_RETURN_ON_ERROR(bsp_i2c0_read_reg(I2C0_DEV_IMU, QMI_REG_AX_L, raw, sizeof(raw)), TAG, "read accel failed");

    int16_t rx = (int16_t)(((uint16_t)raw[1] << 8) | raw[0]);
    int16_t ry = (int16_t)(((uint16_t)raw[3] << 8) | raw[2]);
    int16_t rz = (int16_t)(((uint16_t)raw[5] << 8) | raw[4]);

    if (ax) *ax = (float)rx * QMI_ACC_SCALE_4G;
    if (ay) *ay = (float)ry * QMI_ACC_SCALE_4G;
    if (az) *az = (float)rz * QMI_ACC_SCALE_4G;
    return ESP_OK;
}

void bsp_imu_get_last_accel(float *ax, float *ay, float *az)
{
    if (ax) *ax = s_fx;
    if (ay) *ay = s_fy;
    if (az) *az = s_fz;
}

bool bsp_imu_poll_orientation(rot_mode_t mode, disp_rot_t cur_rot, disp_rot_t *out_rot)
{
    float ax = 0.0f, ay = 0.0f, az = 0.0f;
    if (bsp_imu_read_accel(&ax, &ay, &az) != ESP_OK) {
        return false;
    }

    if (!s_filter_init) {
        s_fx = ax;
        s_fy = ay;
        s_fz = az;
        s_filter_init = true;
    } else {
        s_fx = 0.70f * s_fx + 0.30f * ax;
        s_fy = 0.70f * s_fy + 0.30f * ay;
        s_fz = 0.70f * s_fz + 0.30f * az;
    }

    if (s_log_samples < 5) {
        s_log_samples++;
        ESP_LOGI(TAG, "IMU sample[%d]: ax=%.3f ay=%.3f az=%.3f", s_log_samples, ax, ay, az);
    }

    if (mode == ROT_MODE_LOCKED) {
        s_cand_cnt = 0;
        return false;
    }

    /* Flat on table: hold current orientation */
    if (fabsf(s_fz) > IMU_THRESH_FLAT_Z) {
        s_cand_cnt = 0;
        return false;
    }

    float px = IMU_SWAP_XY ? (s_fy * IMU_SIGN_X) : (s_fx * IMU_SIGN_X);
    float py = IMU_SWAP_XY ? (s_fx * IMU_SIGN_Y) : (s_fy * IMU_SIGN_Y);
    float apx = fabsf(px);
    float apy = fabsf(py);

    disp_rot_t detected = cur_rot;
    bool has_det = false;

    if (apy > IMU_THRESH_GRAV && apy > apx + IMU_THRESH_MARGIN) {
        detected = (py < 0.0f) ? DISP_ROT_0 : DISP_ROT_180;
        has_det = true;
    } else if (apx > IMU_THRESH_GRAV && apx > apy + IMU_THRESH_MARGIN) {
        detected = (px > 0.0f) ? DISP_ROT_90 : DISP_ROT_270;
        has_det = true;
    }

    if (!has_det) {
        s_cand_cnt = 0;
        return false;
    }

    if (mode == ROT_MODE_LANDSCAPE_ONLY &&
        detected != DISP_ROT_90 && detected != DISP_ROT_270) {
        s_cand_cnt = 0;
        return false;
    }

    if (detected == cur_rot) {
        s_cand_cnt = 0;
        return false;
    }

    if (detected == s_cand_rot) {
        s_cand_cnt++;
    } else {
        s_cand_rot = detected;
        s_cand_cnt = 1;
    }

    if (s_cand_cnt >= IMU_STABLE_COUNT) {
        s_cand_cnt = 0;
        ESP_LOGI(TAG, "Orientation -> %d (fx=%.2f fy=%.2f fz=%.2f)",
                 (int)detected, (double)s_fx, (double)s_fy, (double)s_fz);
        if (out_rot) {
            *out_rot = detected;
        }
        return true;
    }
    return false;
}

esp_err_t bsp_imu_enter_wom(uint8_t threshold_mg)
{
    if (!s_imu_ready) return ESP_ERR_INVALID_STATE;

    uint8_t ctrl7_off = 0x00;
    uint8_t ctrl8 = 0x80;
    uint8_t ctrl2 = (1U << 4) | QMI_ODR_LP_21HZ;
    uint8_t cal1_l = threshold_mg;
    uint8_t cal1_h = (0x02U << 6) | 0x10U; /* INT1 initial high -> active-low motion, 16-sample blanking */
    uint8_t ctrl1 = 0x40U | (1U << 3);      /* Enable INT1 */
    uint8_t ctrl7_on = 0x21;                /* DRDY_DIS=1, accel enabled */
    uint8_t st1 = 0;

    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_IMU, QMI_REG_CTRL7, &ctrl7_off, 1), TAG, "CTRL7 off failed");
    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_IMU, QMI_REG_CTRL8, &ctrl8, 1), TAG, "CTRL8 failed");
    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_IMU, QMI_REG_CTRL2, &ctrl2, 1), TAG, "CTRL2 WoM failed");
    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_IMU, QMI_REG_CAL1_L, &cal1_l, 1), TAG, "CAL1_L failed");
    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_IMU, QMI_REG_CAL1_H, &cal1_h, 1), TAG, "CAL1_H failed");
    ESP_RETURN_ON_ERROR(qmi_issue_ctrl9(QMI_CMD_WRITE_WOM), TAG, "WRITE_WOM enter failed");
    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_IMU, QMI_REG_CTRL1, &ctrl1, 1), TAG, "CTRL1 INT1 failed");
    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_IMU, QMI_REG_CTRL7, &ctrl7_on, 1), TAG, "CTRL7 on failed");

    vTaskDelay(pdMS_TO_TICKS(30));
    (void)bsp_i2c0_read_reg(I2C0_DEV_IMU, QMI_REG_STATUS1, &st1, 1);
    s_wom_active = true;
    ESP_LOGI(TAG, "QMI8658C entered Wake-on-Motion (thr=%umg, 21Hz LP, INT1 enabled)", threshold_mg);
    return ESP_OK;
}

esp_err_t bsp_imu_exit_wom(void)
{
    if (!s_imu_ready) return ESP_ERR_INVALID_STATE;

    uint8_t ctrl7_off = 0x00;
    uint8_t zero = 0x00;
    uint8_t ctrl1 = 0x40;
    uint8_t ctrl2 = (1U << 4) | QMI_ODR_LP_128HZ;
    uint8_t ctrl5 = 0x05;
    uint8_t ctrl8 = 0x80;
    uint8_t ctrl7_on = 0x21;
    uint8_t st1 = 0;

    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_IMU, QMI_REG_CTRL7, &ctrl7_off, 1), TAG, "CTRL7 off failed");
    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_IMU, QMI_REG_CAL1_L, &zero, 1), TAG, "CAL1_L clear failed");
    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_IMU, QMI_REG_CAL1_H, &zero, 1), TAG, "CAL1_H clear failed");
    (void)qmi_issue_ctrl9(QMI_CMD_WRITE_WOM);

    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_IMU, QMI_REG_CTRL1, &ctrl1, 1), TAG, "CTRL1 restore failed");
    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_IMU, QMI_REG_CTRL2, &ctrl2, 1), TAG, "CTRL2 restore failed");
    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_IMU, QMI_REG_CTRL5, &ctrl5, 1), TAG, "CTRL5 restore failed");
    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_IMU, QMI_REG_CTRL8, &ctrl8, 1), TAG, "CTRL8 restore failed");
    ESP_RETURN_ON_ERROR(bsp_i2c0_write_reg(I2C0_DEV_IMU, QMI_REG_CTRL7, &ctrl7_on, 1), TAG, "CTRL7 restore failed");

    (void)bsp_i2c0_read_reg(I2C0_DEV_IMU, QMI_REG_STATUS1, &st1, 1);
    s_wom_active = false;
    s_cand_cnt = 0;
    ESP_LOGI(TAG, "QMI8658C exited WoM -> Low-Power 128Hz");
    return ESP_OK;
}

bool bsp_imu_check_wom(void)
{
    if (!s_imu_ready || !s_wom_active) return false;
    uint8_t st1 = 0;
    if (bsp_i2c0_read_reg(I2C0_DEV_IMU, QMI_REG_STATUS1, &st1, 1) != ESP_OK) {
        return false;
    }
    return (st1 & 0x04U) != 0;
}
