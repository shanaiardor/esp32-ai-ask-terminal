#include "bsp_power.h"
#include "board_config.h"
#include "bsp_i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include "driver/gpio.h"
#include "driver/usb_serial_jtag.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG = "bsp_power";

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_cali_handle = NULL;
static bool s_cali_ok = false;

static float s_vbat_avg = 4.10f;
static bool s_vbat_inited = false;
static uint8_t s_bat_pct_filtered = 100;
static bool s_bat_pct_inited = false;

static portMUX_TYPE s_irq_spin = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t s_irq_flags = 0;
static TaskHandle_t s_wake_task = NULL;

typedef struct {
    gpio_num_t pin;
    bool pressed;
    bool long_fired;
    int64_t press_start_ms;
    int64_t last_change_ms;
    bool raw_last;
} btn_state_t;

static btn_state_t s_boot_btn = {.pin = PIN_BOOT_BTN};
static btn_state_t s_pwr_btn  = {.pin = PIN_PWR_BTN};

static void IRAM_ATTR gpio_wake_isr(void *arg)
{
    uint32_t flag = (uint32_t)(uintptr_t)arg;
    TaskHandle_t task = NULL;
    taskENTER_CRITICAL_ISR(&s_irq_spin);
    s_irq_flags |= flag;
    task = s_wake_task;
    taskEXIT_CRITICAL_ISR(&s_irq_spin);

    if (task != NULL) {
        BaseType_t woken = pdFALSE;
        vTaskNotifyGiveFromISR(task, &woken);
        if (woken) {
            portYIELD_FROM_ISR();
        }
    }
}

static void init_gpio_irq_pin(gpio_num_t pin, gpio_int_type_t intr_type, uint32_t irq_flag)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = intr_type,
    };
    gpio_config(&cfg);
    gpio_isr_handler_add(pin, gpio_wake_isr, (void *)(uintptr_t)irq_flag);
}

esp_err_t bsp_power_init(void)
{
    esp_err_t isr_err = gpio_install_isr_service(0);
    if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(isr_err, TAG, "gpio isr service failed");
    }

    init_gpio_irq_pin(PIN_BOOT_BTN, GPIO_INTR_ANYEDGE, BSP_IRQ_BTN);
    init_gpio_irq_pin(PIN_PWR_BTN, GPIO_INTR_ANYEDGE, BSP_IRQ_BTN);
    init_gpio_irq_pin(PIN_EXIO_INT, GPIO_INTR_NEGEDGE, BSP_IRQ_EXIO);

    s_boot_btn.raw_last = (gpio_get_level(PIN_BOOT_BTN) == 0);
    s_pwr_btn.raw_last  = (gpio_get_level(PIN_PWR_BTN) == 0);

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BAT_ADC_UNIT,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc_handle), TAG, "adc unit init failed");

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc_handle, BAT_ADC_CHANNEL, &chan_cfg), TAG, "adc ch cfg failed");

    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = BAT_ADC_UNIT,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali_handle) == ESP_OK) {
        s_cali_ok = true;
    }

    bsp_power_sample_battery();
    ESP_LOGI(TAG, "Power & Buttons initialized (Vbat=%.2fV)", s_vbat_avg);
    return ESP_OK;
}

/* 12-point 18650 Li-ion discharge curve under typical ~180mA terminal load */
static uint8_t vbat_to_pct_18650(float v)
{
    static const struct {
        float v;
        uint8_t pct;
    } lut[] = {
        {4.16f, 100},
        {4.06f,  90},
        {3.98f,  80},
        {3.91f,  70},
        {3.85f,  60},
        {3.80f,  50},
        {3.76f,  40},
        {3.73f,  30},
        {3.69f,  20},
        {3.62f,  10},
        {3.45f,   5},
        {3.20f,   0},
    };
    if (v >= lut[0].v) return 100;
    const size_t n = sizeof(lut) / sizeof(lut[0]);
    if (v <= lut[n - 1].v) return 0;
    for (size_t i = 0; i + 1 < n; i++) {
        if (v >= lut[i + 1].v) {
            float span_v = lut[i].v - lut[i + 1].v;
            float frac = (v - lut[i + 1].v) / span_v;
            float p = (float)lut[i + 1].pct + frac * (float)(lut[i].pct - lut[i + 1].pct);
            int pi = (int)(p + 0.5f);
            if (pi < 0) pi = 0;
            if (pi > 100) pi = 100;
            return (uint8_t)pi;
        }
    }
    return 0;
}

void bsp_power_sample_battery(void)
{
    if (!s_adc_handle) return;

    /* 8-sample trimmed mean (drop min & max) to filter Wi-Fi / backlight transient spikes */
    int sum = 0;
    int vmin = 0x7FFFFFFF;
    int vmax = -1;
    int ok_cnt = 0;
    for (int i = 0; i < 8; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc_handle, BAT_ADC_CHANNEL, &raw) == ESP_OK) {
            sum += raw;
            if (raw < vmin) vmin = raw;
            if (raw > vmax) vmax = raw;
            ok_cnt++;
        }
    }
    if (ok_cnt == 0) return;
    int raw_avg = (ok_cnt >= 4) ? ((sum - vmin - vmax) / (ok_cnt - 2)) : (sum / ok_cnt);

    float v = 0.0f;
    if (s_cali_ok) {
        int mv = 0;
        if (adc_cali_raw_to_voltage(s_cali_handle, raw_avg, &mv) == ESP_OK) {
            v = (float)mv * 3.0f / 1000.0f;
        }
    } else {
        v = ((float)raw_avg * 3.3f / 4096.0f) * 3.0f;
    }

    if (!s_vbat_inited) {
        s_vbat_avg = v;
        s_vbat_inited = true;
    } else {
        s_vbat_avg = 0.80f * s_vbat_avg + 0.20f * v;
    }

    uint8_t raw_pct = vbat_to_pct_18650(s_vbat_avg);
    if (!s_bat_pct_inited) {
        s_bat_pct_filtered = raw_pct;
        s_bat_pct_inited = true;
    } else {
        /* 2% hysteresis band (except exact 0% / 100%) to prevent +/-1% flicker */
        int diff = (int)raw_pct - (int)s_bat_pct_filtered;
        if (diff >= 2 || diff <= -2 || raw_pct == 100 || raw_pct == 0) {
            s_bat_pct_filtered = raw_pct;
        }
    }
}

float bsp_power_get_voltage(void)
{
    return s_vbat_avg;
}

bool bsp_power_is_usb_powered(void)
{
    return usb_serial_jtag_is_connected() || (s_vbat_avg >= 4.22f);
}

uint8_t bsp_power_get_battery_pct(void)
{
    return s_bat_pct_filtered;
}

void bsp_power_shutdown(void)
{
    ESP_LOGW(TAG, "Powering down via EXIO_PIN_SYS_EN = 0");
    bsp_exio_set_level(EXIO_PIN_BL_EN, false);
    bsp_exio_set_level(EXIO_PIN_SYS_EN, false);
}

static int poll_single_btn(btn_state_t *b, int64_t now_ms, int64_t long_ms)
{
    /* Returns 0=none, 1=short, 2=long */
    bool raw = (gpio_get_level(b->pin) == 0);
    if (raw != b->raw_last) {
        b->raw_last = raw;
        b->last_change_ms = now_ms;
    }
    if ((now_ms - b->last_change_ms) < 15) {
        return 0;
    }

    if (raw && !b->pressed) {
        b->pressed = true;
        b->long_fired = false;
        b->press_start_ms = now_ms;
    } else if (raw && b->pressed) {
        if (!b->long_fired && (now_ms - b->press_start_ms) >= long_ms) {
            b->long_fired = true;
            return 2;
        }
    } else if (!raw && b->pressed) {
        b->pressed = false;
        if (!b->long_fired) {
            return 1;
        }
    }
    return 0;
}

bsp_btn_event_t bsp_power_poll_buttons(void)
{
    int64_t now_ms = esp_timer_get_time() / 1000LL;

    int boot_r = poll_single_btn(&s_boot_btn, now_ms, 1800);
    if (boot_r == 1) return BTN_EVT_BOOT_SHORT;
    if (boot_r == 2) return BTN_EVT_BOOT_LONG;

    int pwr_r = poll_single_btn(&s_pwr_btn, now_ms, 2500);
    if (pwr_r == 1) return BTN_EVT_PWR_SHORT;
    if (pwr_r == 2) return BTN_EVT_PWR_LONG;

    return BTN_EVT_NONE;
}

void bsp_power_register_wake_task(void *task_handle)
{
    taskENTER_CRITICAL(&s_irq_spin);
    s_wake_task = (TaskHandle_t)task_handle;
    taskEXIT_CRITICAL(&s_irq_spin);
}

uint32_t bsp_power_wait_irq(uint32_t timeout_ms)
{
    uint32_t flags = 0;
    taskENTER_CRITICAL(&s_irq_spin);
    flags = s_irq_flags;
    s_irq_flags = 0;
    taskEXIT_CRITICAL(&s_irq_spin);

    if (flags != 0) {
        (void)ulTaskNotifyTake(pdTRUE, 0);
        return flags;
    }

    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms));

    taskENTER_CRITICAL(&s_irq_spin);
    flags = s_irq_flags;
    s_irq_flags = 0;
    taskEXIT_CRITICAL(&s_irq_spin);
    return flags;
}

bool bsp_power_buttons_busy(void)
{
    int64_t now_ms = esp_timer_get_time() / 1000LL;
    return s_boot_btn.pressed || s_pwr_btn.pressed ||
           s_boot_btn.raw_last || s_pwr_btn.raw_last ||
           ((now_ms - s_boot_btn.last_change_ms) < 40) ||
           ((now_ms - s_pwr_btn.last_change_ms) < 40) ||
           (gpio_get_level(PIN_BOOT_BTN) == 0) ||
           (gpio_get_level(PIN_PWR_BTN) == 0);
}
