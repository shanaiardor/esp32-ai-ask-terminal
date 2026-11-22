#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "lvgl.h"

#include "board_config.h"
#include "bsp_i2c.h"
#include "bsp_rtc.h"
#include "bsp_imu.h"
#include "bsp_power.h"
#include "bsp_display.h"
#include "bsp_audio.h"
#include "app_settings.h"
#include "app_wifi_ntp.h"
#include "app_dsl.h"
#include "ui_terminal.h"
#include "terminal_fonts.h"

static const char *TAG = "main";

static void hw_poll_task(void *arg)
{
    (void)arg;
    bsp_power_register_wake_task(xTaskGetCurrentTaskHandle());

    bool low_power_active = false;
    int64_t last_imu_ms = 0;
    int64_t last_bat_ms = 0;

    for (;;) {
        bool is_sleeping = bsp_display_is_sleeping();

        if (is_sleeping && !low_power_active) {
            low_power_active = true;
            if (bsp_display_lock(200)) {
                ui_terminal_set_sleeping(true);
                bsp_display_unlock();
            }
            bsp_imu_enter_wom(120);
            app_wifi_set_low_power(true);
            uint8_t exio_in = 0xFF;
            bsp_exio_read_inputs(&exio_in);
            (void)bsp_power_wait_irq(0);
            ESP_LOGI(TAG, "Entered sleep mode (80MHz)");
        } else if (!is_sleeping && low_power_active) {
            low_power_active = false;
            bsp_display_pm_boost(1200);
            bsp_imu_exit_wom();
            app_wifi_set_low_power(false);
            if (bsp_rtc_consume_test_alarm_flag()) {
                app_settings_t cfg;
                app_settings_get(&cfg);
                bsp_rtc_set_alarm(cfg.alarm_enabled, cfg.alarm_hour, cfg.alarm_min, 0);
            }
            if (bsp_display_lock(200)) {
                ui_terminal_set_sleeping(false);
                bsp_display_unlock();
            }
            last_imu_ms = esp_timer_get_time() / 1000LL;
            ESP_LOGI(TAG, "Woke from sleep (DVFS 80~240MHz)");
        }

        if (low_power_active) {
            uint32_t wait_ms = bsp_power_buttons_busy() ? 20 : 100;
            uint32_t irq = bsp_power_wait_irq(wait_ms);
            if (!bsp_display_is_sleeping()) continue;

            bool exio_low = (gpio_get_level(PIN_EXIO_INT) == 0);
            bsp_btn_event_t btn = bsp_power_buttons_busy() ? bsp_power_poll_buttons() : BTN_EVT_NONE;
            if (btn == BTN_EVT_PWR_LONG) {
                bsp_power_shutdown();
                continue;
            } else if (btn != BTN_EVT_NONE) {
                bsp_display_sleep(false);
                continue;
            }

            bool rtc_wake = false;
            bool imu_wake = false;
            bool touch_wake = false;

            if ((irq & BSP_IRQ_EXIO) || exio_low) {
                uint8_t exio_in = 0xFF;
                bsp_exio_read_inputs(&exio_in);
                rtc_wake = bsp_rtc_check_and_clear_alarm();
                imu_wake = bsp_imu_check_wom();
                touch_wake = (!(exio_in & EXIO_PIN_TOUCH_INT));
            }
            if (!rtc_wake && !imu_wake && !touch_wake) {
                touch_wake = bsp_display_probe_touch();
            }

            if (rtc_wake) {
                bsp_display_sleep(false);
                ui_terminal_notify_alarm_fired();
            } else if (imu_wake || touch_wake) {
                if (touch_wake) bsp_display_notify_touch_irq();
                bsp_display_sleep(false);
            }
            continue;
        }

        uint32_t wait_ms = bsp_power_buttons_busy() ? 20 : 250;
        uint32_t irq = bsp_power_wait_irq(wait_ms);
        int64_t now_ms = esp_timer_get_time() / 1000LL;

        if (ui_terminal_take_shutdown_request()) {
            ui_terminal_play_shutdown_anim();
            bsp_power_shutdown();
            continue;
        }

        if ((irq & BSP_IRQ_EXIO) || (gpio_get_level(PIN_EXIO_INT) == 0)) {
            uint8_t exio_in = 0xFF;
            bsp_exio_read_inputs(&exio_in);
            bsp_display_notify_touch_irq();
            if (bsp_rtc_check_and_clear_alarm()) {
                bsp_display_reset_idle_timer();
                ui_terminal_notify_alarm_fired();
                if (bsp_rtc_consume_test_alarm_flag()) {
                    app_settings_t cfg;
                    app_settings_get(&cfg);
                    bsp_rtc_set_alarm(cfg.alarm_enabled, cfg.alarm_hour, cfg.alarm_min, 0);
                }
            }
        }

        if ((irq & BSP_IRQ_BTN) || bsp_power_buttons_busy()) {
            bsp_btn_event_t btn = bsp_power_poll_buttons();
            if (btn != BTN_EVT_NONE) {
                bsp_display_reset_idle_timer();
                if (btn == BTN_EVT_PWR_LONG) {
                    ui_terminal_play_shutdown_anim();
                    bsp_power_shutdown();
                } else {
                    ui_terminal_notify_button(btn);
                }
            }
        }

        app_settings_t cfg;
        app_settings_get(&cfg);

        if ((now_ms - last_imu_ms) >= 250) {
            last_imu_ms = now_ms;
            disp_rot_t cur_rot = bsp_display_get_rotation();
            disp_rot_t target_rot = cur_rot;
            if (bsp_imu_poll_orientation((rot_mode_t)cfg.rot_mode, cur_rot, &target_rot)) {
                bsp_display_reset_idle_timer();
                ui_terminal_notify_imu_rot(target_rot);
            }
        }

        if ((now_ms - last_bat_ms) >= 5000) {
            last_bat_ms = now_ms;
            bsp_power_sample_battery();
            ui_terminal_notify_battery(bsp_power_get_voltage(),
                                       bsp_power_get_battery_pct(),
                                       bsp_power_is_usb_powered());
        }

        bool battery_mode = !bsp_power_is_usb_powered();
        uint32_t eff_sleep_sec = cfg.sleep_timeout_sec;
        uint32_t idle_sec = bsp_display_get_idle_sec();
        uint32_t dim_sec = (eff_sleep_sec >= 20) ? (eff_sleep_sec / 2) : 0;
        bool dimmed = (dim_sec > 0 && idle_sec >= dim_sec);
        bsp_display_set_power_profile(battery_mode, dimmed);

        if (eff_sleep_sec > 0 && idle_sec >= eff_sleep_sec) {
            bsp_display_sleep(true);
        }
    }
}

void app_main(void)
{
    bsp_display_early_bl_off();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(app_settings_init());

    esp_pm_config_t pm_cfg = {
        .max_freq_mhz = 240,
        .min_freq_mhz = 80,
        .light_sleep_enable = false,
    };
    esp_err_t pm_err = esp_pm_configure(&pm_cfg);
    if (pm_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_pm_configure failed: %s", esp_err_to_name(pm_err));
    }

    /* Hold 240MHz during boot initialization & boot animation */
    esp_pm_lock_handle_t boot_pm_lock = NULL;
    if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "boot", &boot_pm_lock) == ESP_OK) {
        esp_pm_lock_acquire(boot_pm_lock);
    }

    app_settings_t cfg;
    app_settings_get(&cfg);
    setenv("TZ", cfg.tz_str, 1);
    tzset();

    ESP_ERROR_CHECK(bsp_i2c_init());
    ESP_ERROR_CHECK(bsp_exio_init());
    ESP_ERROR_CHECK(bsp_rtc_init());

    bool rtc_valid = false;
    bsp_rtc_sync_to_system(&rtc_valid);
    bsp_rtc_set_alarm(cfg.alarm_enabled, cfg.alarm_hour, cfg.alarm_min, 0);

    if (bsp_imu_init() != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 init failed; auto-rotation disabled");
    }
    ESP_ERROR_CHECK(bsp_power_init());

    /* Initialize ES8311 + I2S Audio (boot chime will sync with boot animation) */
    bool audio_ok = (bsp_audio_init(cfg.volume, cfg.muted) == ESP_OK);

    ESP_ERROR_CHECK(app_dsl_init());
    (void)bsp_font_cn_init();

    disp_rot_t init_rot = (cfg.rot_mode == ROT_MODE_LOCKED)
                          ? (disp_rot_t)cfg.locked_rot
                          : DISP_ROT_90;
    ESP_ERROR_CHECK(bsp_display_init(init_rot));

    if (bsp_display_lock(-1)) {
        ui_terminal_init(rtc_valid);
        ui_terminal_notify_battery(bsp_power_get_voltage(),
                                   bsp_power_get_battery_pct(),
                                   bsp_power_is_usb_powered());
        ui_terminal_start_boot_anim();
        lv_timer_handler();
        bsp_display_unlock();
    }

    if (audio_ok) {
        bsp_audio_play_melody("C5:55,E5:55,G5:65,C6:130", 1);
    }
    bsp_display_fade_in(cfg.brightness, 220);
    bsp_display_set_power_profile(!bsp_power_is_usb_powered(), false);

    ESP_ERROR_CHECK(app_wifi_ntp_init());
    xTaskCreatePinnedToCore(hw_poll_task, "hw_poll", 8192, NULL, 3, NULL, 1);

    if (boot_pm_lock) {
        esp_pm_lock_release(boot_pm_lock);
        esp_pm_lock_delete(boot_pm_lock);
    }

    ESP_LOGI(TAG, "AI Ask-Me Terminal started cleanly (on-demand DVFS 80~240MHz)");
}
