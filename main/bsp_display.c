#include "bsp_display.h"
#include "bsp_i2c.h"
#include "board_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/portmacro.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_axs15231b.h"
#include "esp_pm.h"
#include "esp_log.h"
#include "esp_check.h"
#include "lvgl.h"
#include <string.h>

static const char *TAG = "bsp_display";

#define LVGL_TICK_PERIOD_MS     5
#define LVGL_TASK_MAX_DELAY_MS  100
#define LVGL_TASK_MIN_DELAY_MS  2

static SemaphoreHandle_t s_lvgl_mux = NULL;
static SemaphoreHandle_t s_flush_sem = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;

static uint16_t *s_dma_buf[2] = {NULL, NULL};
static lv_disp_draw_buf_t s_disp_buf;
static lv_disp_drv_t s_disp_drv;
static lv_disp_t *s_disp = NULL;
static const uint16_t * volatile s_last_frame_buf = NULL;

static disp_rot_t s_cur_rot = DISP_ROT_90;
static uint8_t s_brightness_pct = 85;
static bool s_battery_mode = false;
static bool s_dimmed = false;
static volatile bool s_sleeping = false;
static bool s_swallow_touch = false;
static bool s_touch_pressed = false;
static TaskHandle_t s_lvgl_task = NULL;
static portMUX_TYPE s_disp_spin = portMUX_INITIALIZER_UNLOCKED;
static volatile int64_t s_touch_window_until_ms = 0;
static volatile int64_t s_last_activity_ms = 0;
static volatile int64_t s_pm_boost_until_ms = 0;
static esp_pm_lock_handle_t s_pm_render_lock = NULL;
static esp_pm_lock_handle_t s_pm_boost_lock = NULL;
static bool s_pm_boost_held = false;

static const axs15231b_lcd_init_cmd_t s_lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 100},
    {0x29, (uint8_t[]){0x00}, 0, 100},
};

void bsp_display_early_bl_off(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_LCD_BL_PWM),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(PIN_LCD_BL_PWM, 0);
}

static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                          esp_lcd_panel_io_event_data_t *edata,
                                          void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_sem, &woken);
    return (woken == pdTRUE);
}

static void lvgl_tick_cb(void *arg)
{
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void lcd_hw_reset(void)
{
    bsp_exio_set_level(EXIO_PIN_LCD_RST, true);
    vTaskDelay(pdMS_TO_TICKS(30));
    bsp_exio_set_level(EXIO_PIN_LCD_RST, false);
    vTaskDelay(pdMS_TO_TICKS(250));
    bsp_exio_set_level(EXIO_PIN_LCD_RST, true);
    /* Wait 120ms for AXS15231B touch MCU boot & capacitive baseline calibration */
    vTaskDelay(pdMS_TO_TICKS(120));
}

static void backlight_pwm_init(void)
{
    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_3,
        .freq_hz = 20 * 1000, /* 20kHz: inaudible, avoids 50~150kHz capacitive touch TX band */
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_conf));

    ledc_channel_config_t ch_conf = {
        .gpio_num = PIN_LCD_BL_PWM,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_3,
        .duty = 255, /* Inverted: 255 = off */
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_conf));
}

static void apply_backlight_hw(uint8_t pct, bool enable)
{
    if (!enable || pct == 0) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, 255);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
        bsp_exio_set_level(EXIO_PIN_BL_EN, false);
        return;
    }
    if (pct > 100) pct = 100;
    /* Map 1..100% -> pwm 125..255 (Waveshare V2 boost IC active range LCD_PWM_MODE_125..255),
     * then invert: duty = 255 - pwm */
    uint32_t pwm = 125 + ((uint32_t)pct * 130) / 100;
    if (pwm > 255) pwm = 255;
    uint32_t duty = 255 - pwm;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
    bsp_exio_set_level(EXIO_PIN_BL_EN, true);
}

static uint8_t compute_effective_brightness(uint8_t base_pct)
{
    if (base_pct == 0) return 0;
    uint8_t eff = base_pct;
    if (s_dimmed && eff > 15) {
        eff = 15;
    }
    return eff;
}

void bsp_display_set_brightness(uint8_t pct)
{
    if (pct > 100) pct = 100;
    s_brightness_pct = pct;
    if (pct == 0) {
        bsp_display_sleep(true);
    } else if (s_sleeping) {
        bsp_display_sleep(false);
    } else {
        apply_backlight_hw(compute_effective_brightness(s_brightness_pct), true);
    }
}

void bsp_display_set_power_profile(bool battery_mode, bool dimmed)
{
    if (battery_mode == s_battery_mode && dimmed == s_dimmed) {
        return;
    }
    s_battery_mode = battery_mode;
    s_dimmed = dimmed;
    if (!s_sleeping && s_brightness_pct > 0) {
        apply_backlight_hw(compute_effective_brightness(s_brightness_pct), true);
    }
}

uint8_t bsp_display_get_brightness(void)
{
    return s_brightness_pct;
}

void bsp_display_fade_in(uint8_t base_pct, uint32_t duration_ms)
{
    if (base_pct > 100) base_pct = 100;
    s_brightness_pct = base_pct;
    if (base_pct == 0) {
        bsp_display_sleep(true);
        return;
    }
    uint8_t target = compute_effective_brightness(base_pct);
    const int steps = 10;
    uint32_t step_ms = (duration_ms >= (uint32_t)steps) ? (duration_ms / (uint32_t)steps) : 10U;
    for (int i = 1; i <= steps; i++) {
        uint8_t cur = (uint8_t)(((uint32_t)target * (uint32_t)i) / (uint32_t)steps);
        if (cur == 0) cur = 1;
        apply_backlight_hw(cur, true);
        if (i < steps) {
            vTaskDelay(pdMS_TO_TICKS(step_ms));
        }
    }
}

void bsp_display_fade_out(uint32_t duration_ms)
{
    if (s_sleeping || s_brightness_pct == 0) {
        apply_backlight_hw(0, false);
        return;
    }
    uint8_t start = compute_effective_brightness(s_brightness_pct);
    const int steps = 8;
    uint32_t step_ms = (duration_ms >= (uint32_t)steps) ? (duration_ms / (uint32_t)steps) : 10U;
    for (int i = steps - 1; i >= 0; i--) {
        uint8_t cur = (uint8_t)(((uint32_t)start * (uint32_t)i) / (uint32_t)steps);
        apply_backlight_hw(cur, cur > 0);
        if (i > 0) {
            vTaskDelay(pdMS_TO_TICKS(step_ms));
        }
    }
}

void bsp_display_wake_lvgl_task(void)
{
    TaskHandle_t t = s_lvgl_task;
    if (t != NULL) {
        xTaskNotifyGive(t);
    }
}

void bsp_display_pm_boost(uint32_t duration_ms)
{
    int64_t now_ms = esp_timer_get_time() / 1000LL;
    int64_t target_ms = now_ms + (int64_t)duration_ms;
    taskENTER_CRITICAL(&s_disp_spin);
    if (s_pm_boost_until_ms < target_ms) {
        s_pm_boost_until_ms = target_ms;
    }
    taskEXIT_CRITICAL(&s_disp_spin);
}

void bsp_display_notify_touch_irq(void)
{
    int64_t now_ms = esp_timer_get_time() / 1000LL;
    taskENTER_CRITICAL(&s_disp_spin);
    if (s_touch_window_until_ms < now_ms + 500LL) {
        s_touch_window_until_ms = now_ms + 500LL;
    }
    if (s_pm_boost_until_ms < now_ms + 1500LL) {
        s_pm_boost_until_ms = now_ms + 1500LL;
    }
    taskEXIT_CRITICAL(&s_disp_spin);
    bsp_display_wake_lvgl_task();
}

void bsp_display_reset_idle_timer(void)
{
    int64_t now_ms = esp_timer_get_time() / 1000LL;
    taskENTER_CRITICAL(&s_disp_spin);
    s_last_activity_ms = now_ms;
    if (s_pm_boost_until_ms < now_ms + 1500LL) {
        s_pm_boost_until_ms = now_ms + 1500LL;
    }
    taskEXIT_CRITICAL(&s_disp_spin);
    if (s_dimmed && !s_sleeping && s_brightness_pct > 0) {
        s_dimmed = false;
        apply_backlight_hw(compute_effective_brightness(s_brightness_pct), true);
    }
}

uint32_t bsp_display_get_idle_sec(void)
{
    int64_t now_ms = esp_timer_get_time() / 1000LL;
    int64_t last_ms = 0;
    taskENTER_CRITICAL(&s_disp_spin);
    last_ms = s_last_activity_ms;
    taskEXIT_CRITICAL(&s_disp_spin);
    if (now_ms <= last_ms) return 0;
    return (uint32_t)((now_ms - last_ms) / 1000LL);
}

void bsp_display_sleep(bool sleep_on)
{
    if (sleep_on == s_sleeping) {
        return;
    }

    bool need_lock = (s_lvgl_task != NULL) && (xTaskGetCurrentTaskHandle() != s_lvgl_task);
    bool locked = false;
    if (need_lock) {
        locked = bsp_display_lock(1000);
    }

    if (sleep_on != s_sleeping) {
        ESP_LOGI(TAG, "bsp_display_sleep(%d) idle=%lus", sleep_on ? 1 : 0, (unsigned long)bsp_display_get_idle_sec());
        if (sleep_on) {
            s_sleeping = true;
            s_touch_pressed = false;
            apply_backlight_hw(0, false);
            /* NOTE: AXS15231B is an In-Cell TDDI chip whose touch scan is multiplexed
             * with display timing. Sending LCD_CMD_DISPOFF (0x28) disables touch scanning
             * completely, breaking touch-to-wake. Keep panel ON with backlight fully OFF. */
        } else {
            int64_t now_ms = esp_timer_get_time() / 1000LL;
            taskENTER_CRITICAL(&s_disp_spin);
            s_last_activity_ms = now_ms;
            s_touch_window_until_ms = now_ms + 400LL;
            if (s_pm_boost_until_ms < now_ms + 1200LL) {
                s_pm_boost_until_ms = now_ms + 1200LL;
            }
            taskEXIT_CRITICAL(&s_disp_spin);
            s_swallow_touch = true;
            s_dimmed = false;
            s_sleeping = false;
            uint8_t pct = (s_brightness_pct < 10) ? 85 : s_brightness_pct;
            s_brightness_pct = pct;
            apply_backlight_hw(compute_effective_brightness(pct), true);
            bsp_display_wake_lvgl_task();
        }
    }

    if (locked) {
        bsp_display_unlock();
    }
}

bool bsp_display_is_sleeping(void)
{
    return s_sleeping;
}

bool bsp_display_probe_touch(void)
{
    static const uint8_t read_cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00};
    uint8_t buff[32] = {0};
    if (bsp_i2c1_touch_xfer(read_cmd, sizeof(read_cmd), buff, sizeof(buff)) != ESP_OK) {
        return false;
    }
    if (buff[1] != 1) {
        return false;
    }
    uint16_t raw_x = (((uint16_t)buff[2] & 0x0f) << 8) | (uint16_t)buff[3];
    uint16_t raw_y = (((uint16_t)buff[4] & 0x0f) << 8) | (uint16_t)buff[5];
    return (raw_x <= LCD_PHYS_H && raw_y <= LCD_PHYS_W);
}

static void IRAM_ATTR rotate_chunk_to_dma(const uint16_t * __restrict src,
                                          uint16_t * __restrict dst,
                                          int y1, disp_rot_t rot)
{
    if (rot == DISP_ROT_90) {
        for (int px = 0; px < LCD_PHYS_W; px++) {
            const uint16_t * __restrict src_row = &src[(LCD_PHYS_W - 1 - px) * LCD_PHYS_H + y1];
            uint16_t * __restrict dst_col = &dst[px];
            for (int ly = 0; ly < LCD_DMA_LINES; ly++) {
                dst_col[ly * LCD_PHYS_W] = src_row[ly];
            }
        }
    } else if (rot == DISP_ROT_270) {
        for (int px = 0; px < LCD_PHYS_W; px++) {
            const uint16_t * __restrict src_row = &src[px * LCD_PHYS_H + (LCD_PHYS_H - 1 - y1)];
            uint16_t * __restrict dst_col = &dst[px];
            for (int ly = 0; ly < LCD_DMA_LINES; ly++) {
                dst_col[ly * LCD_PHYS_W] = src_row[-ly];
            }
        }
    } else if (rot == DISP_ROT_180) {
        const int total = LCD_PHYS_W * LCD_PHYS_H;
        const int chunk_px = LCD_PHYS_W * LCD_DMA_LINES;
        const int start_idx = total - 1 - (y1 * LCD_PHYS_W);
        for (int i = 0; i < chunk_px; i++) {
            dst[i] = src[start_idx - i];
        }
    } else {
        memcpy(dst, src + y1 * LCD_PHYS_W, LCD_DMA_BUF_BYTES);
    }
}

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    (void)area;
    if (!lv_disp_flush_is_last(drv)) {
        lv_disp_flush_ready(drv);
        return;
    }

    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)drv->user_data;
    const uint16_t *src = (const uint16_t *)color_map;
    const int chunk_cnt = LCD_PHYS_H / LCD_DMA_LINES; /* 10 chunks of 64 lines */

    xSemaphoreGive(s_flush_sem);
    for (int i = 0; i < chunk_cnt; i++) {
        int y1 = i * LCD_DMA_LINES;
        xSemaphoreTake(s_flush_sem, portMAX_DELAY);
        rotate_chunk_to_dma(src, s_dma_buf[0], y1, s_cur_rot);
        esp_lcd_panel_draw_bitmap(panel, 0, y1, LCD_PHYS_W, y1 + LCD_DMA_LINES, s_dma_buf[0]);
    }
    xSemaphoreTake(s_flush_sem, portMAX_DELAY);

    s_last_frame_buf = src;
    lv_disp_flush_ready(drv);
}

static void lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    static int64_t s_last_probe_ms = 0;
    static bool s_pending_press = false;
    static lv_coord_t s_pending_x = 0;
    static lv_coord_t s_pending_y = 0;
    static lv_coord_t s_last_valid_x = 0;
    static lv_coord_t s_last_valid_y = 0;
    int64_t now_ms = esp_timer_get_time() / 1000LL;
    int64_t win_until = 0;
    taskENTER_CRITICAL(&s_disp_spin);
    win_until = s_touch_window_until_ms;
    taskEXIT_CRITICAL(&s_disp_spin);

    /* Never throttle touch polling while screen is at active brightness!
     * Only when pre-sleep dimmed (s_dimmed) and idle, gently poll at 25ms (40Hz). */
    if (!s_touch_pressed &&
        !s_pending_press &&
        s_dimmed &&
        now_ms > win_until &&
        (now_ms - s_last_probe_ms) < 25LL &&
        gpio_get_level(PIN_EXIO_INT) != 0) {
        data->state = LV_INDEV_STATE_REL;
        return;
    }
    s_last_probe_ms = now_ms;

    /* Use exact official Waveshare V2 AXS15231B read packet (0x0e, 32 bytes) at 300kHz I2C */
    static const uint8_t read_cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00};
    uint8_t buff[32] = {0};

    if (bsp_i2c1_touch_xfer(read_cmd, sizeof(read_cmd), buff, sizeof(buff)) != ESP_OK) {
        s_touch_pressed = false;
        s_pending_press = false;
        data->state = LV_INDEV_STATE_REL;
        return;
    }

    /* Single-finger UI: only accept buff[1] == 1 to reject n>=2 EMI/capacitive ghost-touch bursts */
    if (buff[1] == 1) {
        uint16_t raw_x = (((uint16_t)buff[2] & 0x0f) << 8) | (uint16_t)buff[3];
        uint16_t raw_y = (((uint16_t)buff[4] & 0x0f) << 8) | (uint16_t)buff[5];
        if (raw_x > LCD_PHYS_H + 8 || raw_y > LCD_PHYS_W + 8) {
            /* Reject clearly out-of-range / corrupted I2C coordinates */
            if (s_touch_pressed) {
                data->point.x = s_last_valid_x;
                data->point.y = s_last_valid_y;
                data->state = LV_INDEV_STATE_PR;
            } else {
                s_pending_press = false;
                data->state = LV_INDEV_STATE_REL;
            }
            return;
        }
        if (raw_x >= LCD_PHYS_H) raw_x = LCD_PHYS_H - 1;
        if (raw_y >= LCD_PHYS_W) raw_y = LCD_PHYS_W - 1;

        uint16_t px = raw_y;
        uint16_t py = (uint16_t)((LCD_PHYS_H - 1) - raw_x);
        lv_coord_t lx = 0, ly = 0;

        switch (s_cur_rot) {
        case DISP_ROT_0:
            lx = (lv_coord_t)px;
            ly = (lv_coord_t)py;
            break;
        case DISP_ROT_90:
            lx = (lv_coord_t)py;
            ly = (lv_coord_t)((LCD_PHYS_W - 1) - px);
            break;
        case DISP_ROT_180:
            lx = (lv_coord_t)((LCD_PHYS_W - 1) - px);
            ly = (lv_coord_t)((LCD_PHYS_H - 1) - py);
            break;
        case DISP_ROT_270:
            lx = (lv_coord_t)((LCD_PHYS_H - 1) - py);
            ly = (lv_coord_t)px;
            break;
        }

        if (s_sleeping) {
            bsp_display_sleep(false);
            s_swallow_touch = true;
            s_pending_press = false;
            data->state = LV_INDEV_STATE_REL;
            return;
        }
        if (s_swallow_touch) {
            s_pending_press = false;
            data->state = LV_INDEV_STATE_REL;
            return;
        }

        /* 2-sample press confirmation (15ms) to completely block single-frame EMI/I2C ghost clicks */
        if (!s_touch_pressed) {
            if (!s_pending_press) {
                s_pending_press = true;
                s_pending_x = lx;
                s_pending_y = ly;
                data->state = LV_INDEV_STATE_REL;
                return;
            }
            lv_coord_t dx = (lx > s_pending_x) ? (lx - s_pending_x) : (s_pending_x - lx);
            lv_coord_t dy = (ly > s_pending_y) ? (ly - s_pending_y) : (s_pending_y - ly);
            if (dx > 48 || dy > 48) {
                /* Unstable jump between first two samples: re-seed pending point */
                s_pending_x = lx;
                s_pending_y = ly;
                data->state = LV_INDEV_STATE_REL;
                return;
            }
            s_pending_press = false;
            s_touch_pressed = true;
        } else {
            /* Mid-press single-frame spike guard */
            lv_coord_t dx = (lx > s_last_valid_x) ? (lx - s_last_valid_x) : (s_last_valid_x - lx);
            lv_coord_t dy = (ly > s_last_valid_y) ? (ly - s_last_valid_y) : (s_last_valid_y - ly);
            if (dx > 140 || dy > 100) {
                lx = s_last_valid_x;
                ly = s_last_valid_y;
            }
        }

        s_last_valid_x = lx;
        s_last_valid_y = ly;

        taskENTER_CRITICAL(&s_disp_spin);
        s_touch_window_until_ms = now_ms + 500LL;
        if (s_pm_boost_until_ms < now_ms + 1500LL) {
            s_pm_boost_until_ms = now_ms + 1500LL;
        }
        s_last_activity_ms = now_ms;
        taskEXIT_CRITICAL(&s_disp_spin);

        if (s_dimmed && !s_sleeping && s_brightness_pct > 0) {
            s_dimmed = false;
            apply_backlight_hw(compute_effective_brightness(s_brightness_pct), true);
        }

        data->point.x = lx;
        data->point.y = ly;
        data->state = LV_INDEV_STATE_PR;
    } else {
        s_touch_pressed = false;
        s_pending_press = false;
        s_swallow_touch = false;
        data->state = LV_INDEV_STATE_REL;
    }
}

static void lvgl_port_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_sleeping) {
            if (s_pm_boost_held && s_pm_boost_lock) {
                esp_pm_lock_release(s_pm_boost_lock);
                s_pm_boost_held = false;
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
            continue;
        }

        int64_t now_ms = esp_timer_get_time() / 1000LL;
        int64_t boost_until = 0;
        taskENTER_CRITICAL(&s_disp_spin);
        boost_until = s_pm_boost_until_ms;
        taskEXIT_CRITICAL(&s_disp_spin);

        /* Keep 240MHz steady while screen is active (!s_dimmed) to avoid DVFS PLL jitter
         * and first-touch ramp latency; drop to 80MHz when dimmed or sleeping. */
        bool want_boost = (!s_dimmed && !s_sleeping) || (now_ms < boost_until) || s_touch_pressed;
        if (want_boost && !s_pm_boost_held && s_pm_boost_lock) {
            esp_pm_lock_acquire(s_pm_boost_lock);
            s_pm_boost_held = true;
        } else if (!want_boost && s_pm_boost_held && s_pm_boost_lock) {
            esp_pm_lock_release(s_pm_boost_lock);
            s_pm_boost_held = false;
        }

        uint32_t delay_ms = LVGL_TASK_MAX_DELAY_MS;
        if (bsp_display_lock(-1)) {
            if (!s_sleeping) {
                if (!s_pm_boost_held && s_pm_render_lock) esp_pm_lock_acquire(s_pm_render_lock);
                delay_ms = lv_timer_handler();
                if (!s_pm_boost_held && s_pm_render_lock) esp_pm_lock_release(s_pm_render_lock);
            }
            bsp_display_unlock();
        }

        if (delay_ms > LVGL_TASK_MAX_DELAY_MS) delay_ms = LVGL_TASK_MAX_DELAY_MS;
        if (delay_ms < LVGL_TASK_MIN_DELAY_MS) delay_ms = LVGL_TASK_MIN_DELAY_MS;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay_ms));
    }
}

esp_err_t bsp_display_init(disp_rot_t initial_rot)
{
    s_cur_rot = initial_rot;
    s_flush_sem = xSemaphoreCreateBinary();
    s_lvgl_mux = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_flush_sem && s_lvgl_mux, ESP_ERR_NO_MEM, TAG, "sem alloc failed");

    spi_bus_config_t buscfg = {
        .data0_io_num = PIN_LCD_DATA0,
        .data1_io_num = PIN_LCD_DATA1,
        .sclk_io_num = PIN_LCD_PCLK,
        .data2_io_num = PIN_LCD_DATA2,
        .data3_io_num = PIN_LCD_DATA3,
        .max_transfer_sz = LCD_DMA_BUF_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "spi bus init failed");

    esp_lcd_panel_io_handle_t panel_io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = PIN_LCD_CS,
        .dc_gpio_num = GPIO_NUM_NC,
        .spi_mode = 3,
        .pclk_hz = 40 * 1000 * 1000,
        .trans_queue_depth = 10,
        .on_color_trans_done = on_color_trans_done,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .flags = {
            .quad_mode = true,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &panel_io),
                        TAG, "panel io init failed");

    axs15231b_vendor_config_t vendor_cfg = {
        .init_cmds = s_lcd_init_cmds,
        .init_cmds_size = sizeof(s_lcd_init_cmds) / sizeof(s_lcd_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = GPIO_NUM_NC,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_cfg,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_axs15231b(panel_io, &panel_cfg, &s_panel), TAG, "new panel failed");

    lcd_hw_reset();
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init failed");
    backlight_pwm_init();
    apply_backlight_hw(0, false);

    s_dma_buf[0] = (uint16_t *)heap_caps_malloc(LCD_DMA_BUF_BYTES, MALLOC_CAP_DMA);
    s_dma_buf[1] = s_dma_buf[0];
    lv_color_t *buf1 = (lv_color_t *)heap_caps_malloc(LCD_FULL_BUF_BYTES, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_dma_buf[0] && buf1, ESP_ERR_NO_MEM, TAG, "display buffer alloc failed");
    memset(buf1, 0, LCD_FULL_BUF_BYTES);

    lv_init();
    lv_disp_draw_buf_init(&s_disp_buf, buf1, NULL, LCD_PHYS_W * LCD_PHYS_H);

    bool is_land = (s_cur_rot == DISP_ROT_90 || s_cur_rot == DISP_ROT_270);
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = is_land ? LCD_PHYS_H : LCD_PHYS_W;
    s_disp_drv.ver_res = is_land ? LCD_PHYS_W : LCD_PHYS_H;
    s_disp_drv.flush_cb = lvgl_flush_cb;
    s_disp_drv.draw_buf = &s_disp_buf;
    s_disp_drv.full_refresh = 0;
    s_disp_drv.direct_mode = 1;
    s_disp_drv.rotated = LV_DISP_ROT_NONE;
    s_disp_drv.user_data = s_panel;
    s_disp = lv_disp_drv_register(&s_disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_cb;
    indev_drv.scroll_limit = 22;
    indev_drv.gesture_limit = 28;
    indev_drv.gesture_min_velocity = 2;
    lv_indev_drv_register(&indev_drv);

    esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick_timer), TAG, "tick timer create failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000), TAG, "tick timer start failed");

    s_last_activity_ms = esp_timer_get_time() / 1000LL;
    s_touch_window_until_ms = s_last_activity_ms + 1000LL;
    s_pm_boost_until_ms = s_last_activity_ms + 2500LL;
    (void)esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "lv_render", &s_pm_render_lock);
    (void)esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "ui_boost", &s_pm_boost_lock);
    xTaskCreatePinnedToCore(lvgl_port_task, "lvgl", 8192, NULL, 4, &s_lvgl_task, 0);
    ESP_LOGI(TAG, "AXS15231B Display & LVGL initialized (%dx%d, rot=%d)",
             s_disp_drv.hor_res, s_disp_drv.ver_res, (int)s_cur_rot);
    return ESP_OK;
}

bool bsp_display_lock(int timeout_ms)
{
    if (!s_lvgl_mux) return false;
    TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(s_lvgl_mux, ticks) == pdTRUE;
}

void bsp_display_unlock(void)
{
    if (s_lvgl_mux) {
        xSemaphoreGive(s_lvgl_mux);
    }
}

void bsp_display_set_rotation(disp_rot_t new_rot)
{
    if (new_rot != DISP_ROT_90 && new_rot != DISP_ROT_270) {
        new_rot = DISP_ROT_90;
    }
    if (s_cur_rot == new_rot) return;
    bsp_display_pm_boost(1000);
    s_cur_rot = new_rot;
    bool is_land = (new_rot == DISP_ROT_90 || new_rot == DISP_ROT_270);
    s_disp_drv.hor_res = is_land ? LCD_PHYS_H : LCD_PHYS_W;
    s_disp_drv.ver_res = is_land ? LCD_PHYS_W : LCD_PHYS_H;
    lv_disp_drv_update(s_disp, &s_disp_drv);
    if (lv_scr_act()) {
        lv_obj_invalidate(lv_scr_act());
    }
    bsp_display_wake_lvgl_task();
    ESP_LOGI(TAG, "Display rotated -> %d (%dx%d)", (int)new_rot, s_disp_drv.hor_res, s_disp_drv.ver_res);
}

disp_rot_t bsp_display_get_rotation(void)
{
    return s_cur_rot;
}

static inline void write_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static inline void write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

/**
 * @brief Capture current LVGL framebuffer into a 24-bit BI_RGB BMP buffer in SPIRAM.
 */
esp_err_t bsp_display_capture_bmp(uint8_t **out_buf, size_t *out_len)
{
    if (!out_buf || !out_len) return ESP_ERR_INVALID_ARG;
    *out_buf = NULL;
    *out_len = 0;

    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    int w = s_disp_drv.hor_res;
    int h = s_disp_drv.ver_res;
    const uint16_t *src = s_last_frame_buf ? s_last_frame_buf : (const uint16_t *)s_disp_buf.buf1;
    if (w <= 0 || h <= 0 || !src) {
        bsp_display_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    size_t row_stride = (size_t)((w * 3 + 3) & ~3);
    size_t img_bytes = row_stride * (size_t)h;
    size_t total_sz = 54 + img_bytes;

    uint8_t *bmp = (uint8_t *)heap_caps_malloc(total_sz, MALLOC_CAP_SPIRAM);
    if (!bmp) {
        bsp_display_unlock();
        return ESP_ERR_NO_MEM;
    }

    memset(bmp, 0, 54);
    bmp[0] = 'B';
    bmp[1] = 'M';
    write_le32(&bmp[2], (uint32_t)total_sz);
    write_le32(&bmp[10], 54);
    write_le32(&bmp[14], 40);
    write_le32(&bmp[18], (uint32_t)w);
    write_le32(&bmp[22], (uint32_t)h);
    write_le16(&bmp[26], 1);
    write_le16(&bmp[28], 24);
    write_le32(&bmp[30], 0);
    write_le32(&bmp[34], (uint32_t)img_bytes);
    write_le32(&bmp[38], 2835);
    write_le32(&bmp[42], 2835);

    size_t pad = row_stride - (size_t)(w * 3);
    uint8_t *dst_row = bmp + 54;
    for (int y = h - 1; y >= 0; y--) {
        const uint16_t *src_row = src + (size_t)y * (size_t)w;
        uint8_t *p = dst_row;
        for (int x = 0; x < w; x++) {
            uint16_t raw = src_row[x];
            uint16_t c = (uint16_t)((raw >> 8) | (raw << 8));
            uint8_t r5 = (uint8_t)((c >> 11) & 0x1F);
            uint8_t g6 = (uint8_t)((c >> 5) & 0x3F);
            uint8_t b5 = (uint8_t)(c & 0x1F);
            p[0] = (uint8_t)((b5 << 3) | (b5 >> 2));
            p[1] = (uint8_t)((g6 << 2) | (g6 >> 4));
            p[2] = (uint8_t)((r5 << 3) | (r5 >> 2));
            p += 3;
        }
        for (size_t k = 0; k < pad; k++) {
            p[k] = 0;
        }
        dst_row += row_stride;
    }

    bsp_display_unlock();
    *out_buf = bmp;
    *out_len = total_sz;
    return ESP_OK;
}
