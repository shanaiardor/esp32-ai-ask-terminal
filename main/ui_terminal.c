#include "ui_terminal.h"
#include "app_dsl.h"
#include "app_settings.h"
#include "app_wifi_ntp.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "bsp_rtc.h"
#include "terminal_fonts.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "ui_terminal";

#define UI_W 640
#define UI_H 172

/* Colors */
#define COL_BG          lv_color_hex(0x070B13)
#define COL_DOCK_BG     lv_color_hex(0x0E1525)
#define COL_CARD_BG     lv_color_hex(0x111A2E)
#define COL_ITEM_BG     lv_color_hex(0x18233A)
#define COL_BORDER      lv_color_hex(0x233352)
#define COL_TEXT_MAIN   lv_color_hex(0xF1F5F9)
#define COL_TEXT_MUTED  lv_color_hex(0x94A3B8)
#define COL_TEXT_DIM    lv_color_hex(0x64748B)

typedef struct {
    lv_color_t accent;
    lv_color_t badge_bg;
    const char *tag_en;
} state_theme_t;

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_idle_root = NULL;
static lv_obj_t *s_active_root = NULL;
static lv_obj_t *s_portal_modal = NULL;
static lv_obj_t *s_settings_modal = NULL;
static lv_obj_t *s_boot_overlay = NULL;
static lv_obj_t *s_shutdown_overlay = NULL;

/* Settings Modal widgets */
static lv_obj_t *s_set_bright_lbl = NULL;
static lv_obj_t *s_set_sleep_lbl = NULL;
static lv_obj_t *s_set_rot_lbl = NULL;
static lv_obj_t *s_set_net_lbl = NULL;
static lv_obj_t *s_set_vol_lbl = NULL;
static lv_obj_t *s_set_alarm_lbl = NULL;
static lv_obj_t *s_set_ntp_lbl = NULL;

static volatile bool s_shutdown_requested = false;
static int64_t s_alarm_toast_until_ms = 0;

/* Idle Dashboard widgets (Full-Width Immersive Clock + Slide-Out Drawer) */
static lv_obj_t *s_idle_hh_lbl = NULL;
static lv_obj_t *s_idle_colon_lbl = NULL;
static lv_obj_t *s_idle_mm_lbl = NULL;
static lv_obj_t *s_idle_ss_lbl = NULL;
static lv_obj_t *s_idle_bar_sec = NULL;
static lv_obj_t *s_idle_date_lbl = NULL;
static lv_obj_t *s_idle_net_pill = NULL;
static lv_obj_t *s_idle_net_dot = NULL;
static lv_obj_t *s_idle_net_badge = NULL;
static lv_obj_t *s_idle_bat_pill = NULL;
static lv_obj_t *s_idle_bat_fill = NULL;
static lv_obj_t *s_idle_bat_lbl = NULL;
static uint8_t s_ui_last_bat_pct = 255;
static bool s_ui_last_usb_pwr = false;
static bool s_ui_bat_inited = false;
static lv_obj_t *s_idle_mute_btn_lbl = NULL;
static lv_obj_t *s_idle_mute_ico_lbl = NULL;
static lv_obj_t *s_drawer_mask = NULL;
static lv_obj_t *s_drawer_panel = NULL;
static lv_obj_t *s_drawer_wifi_dot = NULL;
static lv_obj_t *s_drawer_wifi_lbl = NULL;
static lv_obj_t *s_drawer_bat_lbl = NULL;
static lv_obj_t *s_drawer_audio_tag = NULL;
static bool s_drawer_open = false;
static int64_t s_drawer_open_ms = 0;
static int s_idle_last_sec = -1;
static bool s_idle_colon_dim = false;

/* Active DSL 3-Zone widgets */
static lv_obj_t *s_left_dock = NULL;
static lv_obj_t *s_state_pill = NULL;
static lv_obj_t *s_state_dot = NULL;
static lv_obj_t *s_state_lbl = NULL;
static lv_obj_t *s_agent_lbl = NULL;
static lv_obj_t *s_countdown_lbl = NULL;
static lv_obj_t *s_left_meta_lbl = NULL;
static lv_obj_t *s_mute_chip_lbl = NULL;

static lv_obj_t *s_center_card = NULL;
static lv_obj_t *s_title_lbl = NULL;
static lv_obj_t *s_subtitle_lbl = NULL;
static lv_obj_t *s_body_box = NULL;

static lv_obj_t *s_right_dock = NULL;
static lv_obj_t *s_timeout_bar = NULL;

/* Portal Modal widgets */
static lv_obj_t *s_portal_title_lbl = NULL;
static lv_obj_t *s_portal_qr = NULL;
static lv_obj_t *s_portal_info_lbl = NULL;

static volatile bool s_dirty_active = true;
static uint32_t s_rendered_gen = 0;
static bool s_rendered_active = false;
static lv_timer_t *s_refresh_timer = NULL;
static bool s_rtc_valid = false;
static float s_vbat = 4.20f;
static uint8_t s_bat_pct = 100;
static bool s_usb_pwr = true;
static uint8_t s_anim_phase = 0;

/* Live-updatable widget pointers for Number / Choice without full rebuild */
static lv_obj_t *s_num_val_lbl = NULL;
static lv_obj_t *s_num_unit_lbl = NULL;
static lv_obj_t *s_choice_btns[DSL_MAX_CHOICES] = {0};

static state_theme_t get_state_theme(ai_state_t st)
{
    switch (st) {
    case AI_STATE_THINKING: return (state_theme_t){lv_color_hex(0xA855F7), lv_color_hex(0x2B1644), "THINKING"};
    case AI_STATE_ASKING:   return (state_theme_t){lv_color_hex(0x06B6D4), lv_color_hex(0x0D2C3A), "ASKING"};
    case AI_STATE_RUNNING:  return (state_theme_t){lv_color_hex(0x3B82F6), lv_color_hex(0x13274A), "RUNNING"};
    case AI_STATE_SUCCESS:  return (state_theme_t){lv_color_hex(0x22C55E), lv_color_hex(0x0E2F1E), "SUCCESS"};
    case AI_STATE_WARNING:  return (state_theme_t){lv_color_hex(0xF59E0B), lv_color_hex(0x38250A), "WARNING"};
    case AI_STATE_ERROR:    return (state_theme_t){lv_color_hex(0xEF4444), lv_color_hex(0x3B1317), "ERROR"};
    default:                return (state_theme_t){lv_color_hex(0x38BDF8), lv_color_hex(0x10273D), "READY"};
    }
}

static lv_color_t get_btn_bg(dsl_btn_style_t st)
{
    switch (st) {
    case DSL_BTN_PRIMARY:   return lv_color_hex(0x2563EB);
    case DSL_BTN_SUCCESS:   return lv_color_hex(0x16A34A);
    case DSL_BTN_DANGER:    return lv_color_hex(0xDC2626);
    case DSL_BTN_WARNING:   return lv_color_hex(0xD97706);
    case DSL_BTN_SECONDARY: return lv_color_hex(0x27354F);
    default:                return lv_color_hex(0x2563EB);
    }
}

static lv_color_t get_btn_border(dsl_btn_style_t st)
{
    switch (st) {
    case DSL_BTN_PRIMARY:   return lv_color_hex(0x60A5FA);
    case DSL_BTN_SUCCESS:   return lv_color_hex(0x4ADE80);
    case DSL_BTN_DANGER:    return lv_color_hex(0xF87171);
    case DSL_BTN_WARNING:   return lv_color_hex(0xFBBF24);
    case DSL_BTN_SECONDARY: return lv_color_hex(0x475569);
    default:                return lv_color_hex(0x60A5FA);
    }
}

static void style_clean_box(lv_obj_t *obj, lv_color_t bg, lv_color_t border, int radius)
{
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_style_bg_color(obj, bg, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, border, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

static void configure_responsive_btn(lv_obj_t *btn, lv_coord_t ext_hit)
{
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    if (ext_hit > 0) {
        lv_obj_set_ext_click_area(btn, ext_hit);
    }
}

static esp_timer_handle_t s_portal_async_timer = NULL;
static volatile int s_portal_async_op = 0; /* 1 = start, 2 = cancel */

static void portal_async_timer_cb(void *arg)
{
    (void)arg;
    int op = s_portal_async_op;
    s_portal_async_op = 0;
    if (op == 1) {
        app_wifi_start_portal();
    } else if (op == 2) {
        app_wifi_cancel_portal();
    }
}

static void trigger_portal_async(int op)
{
    if (!s_portal_async_timer) {
        esp_timer_create_args_t t_args = {
            .callback = portal_async_timer_cb,
            .name = "portal_op",
        };
        (void)esp_timer_create(&t_args, &s_portal_async_timer);
    }
    s_portal_async_op = op;
    if (s_portal_async_timer) {
        esp_timer_stop(s_portal_async_timer);
        esp_timer_start_once(s_portal_async_timer, 25 * 1000ULL);
    }
}

static void enable_marquee_scroll(lv_obj_t *lbl)
{
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_anim_speed(lbl, 30, 0);
}

/* ---------- Event Callbacks ---------- */

static void on_dsl_button_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    bsp_display_reset_idle_timer();
    app_dsl_submit_button(idx, "touch");
    s_dirty_active = true;
}

static void on_dismiss_click(lv_event_t *e)
{
    (void)e;
    bsp_display_reset_idle_timer();
    app_dsl_dismiss("touch");
    s_dirty_active = true;
}

static void on_choice_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    bsp_display_reset_idle_timer();
    app_dsl_toggle_choice(idx);

    dsl_request_t req;
    if (app_dsl_get_active_snapshot(&req, NULL, NULL, NULL)) {
        state_theme_t th = get_state_theme(req.state);
        for (int i = 0; i < req.choice_count && i < DSL_MAX_CHOICES; i++) {
            if (!s_choice_btns[i]) continue;
            bool sel = req.choices[i].selected;
            lv_obj_set_style_bg_color(s_choice_btns[i], sel ? th.badge_bg : COL_ITEM_BG, 0);
            lv_obj_set_style_border_color(s_choice_btns[i], sel ? th.accent : COL_BORDER, 0);
            lv_obj_set_style_border_width(s_choice_btns[i], sel ? 2 : 1, 0);
        }
    }
}

static void on_number_step_click(lv_event_t *e)
{
    int delta = (int)(intptr_t)lv_event_get_user_data(e);
    bsp_display_reset_idle_timer();
    app_dsl_adjust_number(delta);

    dsl_request_t req;
    if (app_dsl_get_active_snapshot(&req, NULL, NULL, NULL) && s_num_val_lbl) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%ld", (long)req.num_value);
        lv_label_set_text(s_num_val_lbl, buf);
        if (s_num_unit_lbl) {
            lv_obj_align(s_num_val_lbl, LV_ALIGN_CENTER, -14, 0);
            lv_obj_align_to(s_num_unit_lbl, s_num_val_lbl, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -3);
        }
    }
}

static void sync_mute_labels(bool muted)
{
    if (s_mute_chip_lbl) {
        lv_label_set_text(s_mute_chip_lbl, muted ? "静音: 开" : "声音: 开");
    }
    if (s_idle_mute_btn_lbl) {
        lv_label_set_text(s_idle_mute_btn_lbl, muted ? "有声" : "静音");
    }
    if (s_idle_mute_ico_lbl) {
        lv_label_set_text(s_idle_mute_ico_lbl, muted ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
    }
    if (s_drawer_audio_tag) {
        lv_label_set_text(s_drawer_audio_tag, muted ? "已静音" : "有声");
        lv_obj_set_style_text_color(s_drawer_audio_tag,
                                    muted ? COL_TEXT_DIM : lv_color_hex(0x38BDF8), 0);
    }
}

static void on_toggle_mute_click(lv_event_t *e)
{
    (void)e;
    bsp_display_reset_idle_timer();
    if (s_drawer_open) {
        s_drawer_open_ms = esp_timer_get_time() / 1000LL;
    }
    bool new_mute = !bsp_audio_is_muted();
    bsp_audio_set_mute(new_mute);
    app_settings_set_muted(new_mute);
    if (!new_mute) {
        bsp_audio_play_preset(AUDIO_PRESET_CLICK, 1);
    }
    sync_mute_labels(new_mute);
}

static void on_test_sound_click(lv_event_t *e)
{
    (void)e;
    bsp_display_reset_idle_timer();
    if (s_drawer_open) {
        s_drawer_open_ms = esp_timer_get_time() / 1000LL;
    }
    if (bsp_audio_is_muted()) {
        bsp_audio_set_mute(false);
        app_settings_set_muted(false);
        sync_mute_labels(false);
    }
    bsp_audio_play_preset(AUDIO_PRESET_ASK, 1);
}

static void close_portal_modal(void)
{
    if (s_portal_modal) {
        lv_obj_del(s_portal_modal);
        s_portal_modal = NULL;
        s_portal_title_lbl = NULL;
        s_portal_qr = NULL;
        s_portal_info_lbl = NULL;
    }
}

static void on_portal_close_click(lv_event_t *e)
{
    (void)e;
    bsp_display_reset_idle_timer();
    bsp_audio_play_preset(AUDIO_PRESET_CLICK, 1);
    close_portal_modal();
    trigger_portal_async(2);
}

static void open_portal_modal(bool force_ap_portal)
{
    close_portal_modal();
    net_status_t net;
    app_wifi_get_status(&net);

    bool show_ap_qr = (force_ap_portal || net.wifi_state != NET_CONNECTED);
    if (show_ap_qr) {
        trigger_portal_async(1);
    }

    s_portal_modal = lv_obj_create(s_scr);
    lv_obj_set_size(s_portal_modal, UI_W, UI_H);
    lv_obj_set_pos(s_portal_modal, 0, 0);
    style_clean_box(s_portal_modal, lv_color_hex(0x060911), COL_BORDER, 0);

    lv_obj_t *qr_box = lv_obj_create(s_portal_modal);
    lv_obj_set_size(qr_box, 140, 140);
    lv_obj_set_pos(qr_box, 18, 16);
    style_clean_box(qr_box, lv_color_hex(0xFFFFFF), lv_color_hex(0xFFFFFF), 10);

    char qr_payload[128];
    if (!show_ap_qr && net.wifi_state == NET_CONNECTED) {
        snprintf(qr_payload, sizeof(qr_payload), "http://%s/", net.ip_str);
    } else {
        snprintf(qr_payload, sizeof(qr_payload), "WIFI:T:nopass;S:%s;;", net.ap_ssid);
    }

    s_portal_qr = lv_qrcode_create(qr_box, 124, lv_color_hex(0x000000), lv_color_hex(0xFFFFFF));
    lv_qrcode_update(s_portal_qr, qr_payload, strlen(qr_payload));
    lv_obj_center(s_portal_qr);

    s_portal_title_lbl = lv_label_create(s_portal_modal);
    lv_obj_set_pos(s_portal_title_lbl, 174, 18);
    lv_obj_set_style_text_font(s_portal_title_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_portal_title_lbl, lv_color_hex(0x38BDF8), 0);
    lv_label_set_text(s_portal_title_lbl,
                      (!show_ap_qr && net.wifi_state == NET_CONNECTED) ? "Web 调试台与 Wi-Fi 管理" : "手机扫码连接配网热点");

    s_portal_info_lbl = lv_label_create(s_portal_modal);
    lv_obj_set_pos(s_portal_info_lbl, 174, 48);
    lv_obj_set_size(s_portal_info_lbl, 310, 108);
    lv_obj_set_style_text_font(s_portal_info_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_portal_info_lbl, COL_TEXT_MAIN, 0);
    lv_label_set_long_mode(s_portal_info_lbl, LV_LABEL_LONG_WRAP);

    char info[256];
    if (!show_ap_qr && net.wifi_state == NET_CONNECTED) {
        snprintf(info, sizeof(info),
                 "当前 Wi-Fi: %s\n浏览器访问: http://%s/\nAPI 地址: http://%s/api/ask",
                 net.ssid, net.ip_str, net.ip_str);
    } else {
        snprintf(info, sizeof(info),
                 "1. 连接热点: %s (无密码)\n2. 打开浏览器访问: http://192.168.4.1/\n3. 选定 Wi-Fi 并保存即可上线",
                 net.ap_ssid);
    }
    lv_label_set_text(s_portal_info_lbl, info);

    lv_obj_t *btn_close = lv_btn_create(s_portal_modal);
    lv_obj_set_size(btn_close, 126, 48);
    lv_obj_set_pos(btn_close, 496, 108);
    configure_responsive_btn(btn_close, 8);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0x27354F), 0);
    lv_obj_set_style_bg_color(btn_close, lv_color_hex(0x38BDF8), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn_close, 10, 0);
    lv_obj_add_event_cb(btn_close, on_portal_close_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn_close);
    lv_obj_set_style_text_font(lbl, &font_cn_16, 0);
    lv_label_set_text(lbl, "返回终端");
    lv_obj_center(lbl);
}

/* ---------- Comprehensive Settings Modal (640x172) ---------- */

static void set_label_if_changed(lv_obj_t *lbl, const char *txt);

static const char *rot_mode_cn(uint8_t mode)
{
    switch (mode) {
    case ROT_MODE_LANDSCAPE_ONLY: return "自动横屏";
    case ROT_MODE_LOCKED:         return "已锁定";
    default:                      return "自动横屏";
    }
}

static void refresh_settings_modal(void)
{
    if (!s_settings_modal) return;
    app_settings_t cfg;
    app_settings_get(&cfg);
    net_status_t net;
    app_wifi_get_status(&net);

    if (s_set_bright_lbl) {
        char buf[24];
        snprintf(buf, sizeof(buf), "亮度 %u%%", cfg.brightness);
        set_label_if_changed(s_set_bright_lbl, buf);
    }
    if (s_set_sleep_lbl) {
        char buf[32];
        if (cfg.sleep_timeout_sec == 0) {
            snprintf(buf, sizeof(buf), "待机: 常亮");
        } else if (cfg.sleep_timeout_sec < 60) {
            snprintf(buf, sizeof(buf), "待机: %u秒", cfg.sleep_timeout_sec);
        } else {
            snprintf(buf, sizeof(buf), "待机: %u分钟", (unsigned)(cfg.sleep_timeout_sec / 60));
        }
        set_label_if_changed(s_set_sleep_lbl, buf);
    }
    if (s_set_rot_lbl) {
        char buf[32];
        snprintf(buf, sizeof(buf), "旋转: %s", rot_mode_cn(cfg.rot_mode));
        set_label_if_changed(s_set_rot_lbl, buf);
    }
    if (s_set_net_lbl) {
        char buf[96];
        if (net.wifi_state == NET_CONNECTED) {
            snprintf(buf, sizeof(buf), "Wi-Fi: %s (%s)", net.ssid, net.ip_str);
        } else if (cfg.wifi_ssid[0]) {
            snprintf(buf, sizeof(buf), "Wi-Fi: %s (连接中)", cfg.wifi_ssid);
        } else {
            snprintf(buf, sizeof(buf), "Wi-Fi: 未配置");
        }
        set_label_if_changed(s_set_net_lbl, buf);
    }
    if (s_set_vol_lbl) {
        char buf[32];
        if (bsp_audio_is_muted()) {
            snprintf(buf, sizeof(buf), "静音 (%u%%)", cfg.volume);
        } else {
            snprintf(buf, sizeof(buf), "音量 %u%%", cfg.volume);
        }
        set_label_if_changed(s_set_vol_lbl, buf);
    }
    if (s_set_alarm_lbl) {
        char buf[32];
        snprintf(buf, sizeof(buf), "闹钟:%s %02u:%02u",
                 cfg.alarm_enabled ? "开" : "关", cfg.alarm_hour, cfg.alarm_min);
        set_label_if_changed(s_set_alarm_lbl, buf);
        lv_obj_set_style_text_color(s_set_alarm_lbl,
                                    cfg.alarm_enabled ? lv_color_hex(0x4ADE80) : COL_TEXT_MAIN, 0);
    }
}

static void close_settings_modal(void)
{
    if (s_settings_modal) {
        lv_obj_del(s_settings_modal);
        s_settings_modal = NULL;
        s_set_bright_lbl = NULL;
        s_set_sleep_lbl = NULL;
        s_set_rot_lbl = NULL;
        s_set_net_lbl = NULL;
        s_set_vol_lbl = NULL;
        s_set_alarm_lbl = NULL;
        s_set_ntp_lbl = NULL;
    }
}

static lv_obj_t *make_set_btn(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                              lv_coord_t w, lv_coord_t h, lv_color_t bg, lv_color_t border,
                              lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    configure_responsive_btn(btn, 2);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, border, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x38BDF8), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x7DD3FC), LV_STATE_PRESSED);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }
    return btn;
}

static void on_set_bright_slider(lv_event_t *e)
{
    bsp_display_reset_idle_timer();
    lv_obj_t *sl = lv_event_get_target(e);
    int32_t v = lv_slider_get_value(sl);
    if (v < 10) v = 10;
    if (v > 100) v = 100;
    bsp_display_set_brightness((uint8_t)v);
    if (s_set_bright_lbl) {
        char buf[24];
        snprintf(buf, sizeof(buf), "亮度 %d%%", (int)v);
        set_label_if_changed(s_set_bright_lbl, buf);
    }
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        app_settings_set_brightness((uint8_t)v);
    }
}

static void on_set_cycle_sleep(lv_event_t *e)
{
    (void)e;
    bsp_display_reset_idle_timer();
    bsp_audio_play_preset(AUDIO_PRESET_CLICK, 1);
    app_settings_t cfg;
    app_settings_get(&cfg);
    uint16_t next = 60;
    switch (cfg.sleep_timeout_sec) {
    case 30:  next = 60;  break;
    case 60:  next = 180; break;
    case 180: next = 300; break;
    case 300: next = 0;   break;
    default:  next = 30;  break;
    }
    app_settings_set_sleep_timeout(next);
    ESP_LOGI(TAG, "CLICK: on_set_cycle_sleep -> %us", (unsigned)next);
    refresh_settings_modal();
}

static int64_t s_settings_open_ms = 0;

static void on_set_sleep_now(lv_event_t *e)
{
    (void)e;
    int64_t now_ms = esp_timer_get_time() / 1000LL;
    if ((now_ms - s_settings_open_ms) < 400LL) {
        ESP_LOGW(TAG, "Ignored early on_set_sleep_now (%lldms after open)", (long long)(now_ms - s_settings_open_ms));
        return;
    }
    ESP_LOGI(TAG, "CLICK: on_set_sleep_now");
    close_settings_modal();
    bsp_display_sleep(true);
}

static void on_set_toggle_rot(lv_event_t *e)
{
    (void)e;
    bsp_display_reset_idle_timer();
    bsp_audio_play_preset(AUDIO_PRESET_CLICK, 1);
    app_settings_t cfg;
    app_settings_get(&cfg);
    uint8_t next = (cfg.rot_mode == ROT_MODE_LOCKED) ? ROT_MODE_LANDSCAPE_ONLY : ROT_MODE_LOCKED;
    app_settings_set_rot_mode(next);
    if (next == ROT_MODE_LOCKED) {
        app_settings_set_locked_rot((uint8_t)bsp_display_get_rotation());
    } else {
        disp_rot_t cur = bsp_display_get_rotation();
        if (cur != DISP_ROT_90 && cur != DISP_ROT_270) {
            bsp_display_set_rotation(DISP_ROT_90);
        }
    }
    refresh_settings_modal();
}

static void on_set_flip_landscape(lv_event_t *e)
{
    (void)e;
    bsp_display_reset_idle_timer();
    bsp_audio_play_preset(AUDIO_PRESET_CLICK, 1);
    disp_rot_t cur = bsp_display_get_rotation();
    disp_rot_t next = (cur == DISP_ROT_90) ? DISP_ROT_270 : DISP_ROT_90;
    app_settings_set_rot_mode(ROT_MODE_LOCKED);
    app_settings_set_locked_rot((uint8_t)next);
    bsp_display_set_rotation(next);
    lv_obj_invalidate(s_scr);
    refresh_settings_modal();
}

static void on_set_cycle_vol(lv_event_t *e)
{
    (void)e;
    bsp_display_reset_idle_timer();
    app_settings_t cfg;
    app_settings_get(&cfg);
    if (bsp_audio_is_muted()) {
        bsp_audio_set_mute(false);
        app_settings_set_muted(false);
        sync_mute_labels(false);
    } else {
        uint8_t next = 80;
        if (cfg.volume < 45) next = 60;
        else if (cfg.volume < 75) next = 80;
        else if (cfg.volume < 95) next = 100;
        else next = 35;
        bsp_audio_set_volume(next);
        app_settings_set_volume(next);
    }
    bsp_audio_play_preset(AUDIO_PRESET_CLICK, 1);
    refresh_settings_modal();
}

static void on_set_alarm_toggle(lv_event_t *e)
{
    (void)e;
    bsp_display_reset_idle_timer();
    bsp_audio_play_preset(AUDIO_PRESET_CLICK, 1);
    app_settings_t cfg;
    app_settings_get(&cfg);
    bool en = !cfg.alarm_enabled;
    app_settings_set_alarm(en, cfg.alarm_hour, cfg.alarm_min);
    refresh_settings_modal();
}

static void on_set_alarm_hr(lv_event_t *e)
{
    (void)e;
    bsp_display_reset_idle_timer();
    bsp_audio_play_preset(AUDIO_PRESET_CLICK, 1);
    app_settings_t cfg;
    app_settings_get(&cfg);
    uint8_t hr = (uint8_t)((cfg.alarm_hour + 1) % 24);
    app_settings_set_alarm(cfg.alarm_enabled, hr, cfg.alarm_min);
    refresh_settings_modal();
}

static void on_set_alarm_min(lv_event_t *e)
{
    (void)e;
    bsp_display_reset_idle_timer();
    bsp_audio_play_preset(AUDIO_PRESET_CLICK, 1);
    app_settings_t cfg;
    app_settings_get(&cfg);
    uint8_t mn = (uint8_t)(((cfg.alarm_min / 5) * 5 + 5) % 60);
    app_settings_set_alarm(cfg.alarm_enabled, cfg.alarm_hour, mn);
    refresh_settings_modal();
}

static void on_set_alarm_test(lv_event_t *e)
{
    (void)e;
    int64_t now_ms = esp_timer_get_time() / 1000LL;
    if ((now_ms - s_settings_open_ms) < 400LL) return;
    bsp_audio_play_preset(AUDIO_PRESET_CLICK, 1);
    close_settings_modal();
    bsp_rtc_arm_test_alarm_sec(10);
    bsp_display_sleep(true);
}

static void on_set_ntp_sync(lv_event_t *e)
{
    (void)e;
    bsp_display_reset_idle_timer();
    bsp_audio_play_preset(AUDIO_PRESET_CLICK, 1);
    esp_err_t err = app_ntp_sync_now();
    if (s_set_ntp_lbl) {
        lv_label_set_text(s_set_ntp_lbl, (err == ESP_OK) ? "校时中..." : "未联网");
    }
}

static void on_set_shutdown(lv_event_t *e)
{
    (void)e;
    int64_t now_ms = esp_timer_get_time() / 1000LL;
    if ((now_ms - s_settings_open_ms) < 400LL) return;
    close_settings_modal();
    s_shutdown_requested = true;
}

static void on_set_back(lv_event_t *e)
{
    (void)e;
    bsp_display_reset_idle_timer();
    bsp_audio_play_preset(AUDIO_PRESET_CLICK, 1);
    close_settings_modal();
}

static void open_settings_modal(void)
{
    close_portal_modal();
    close_settings_modal();
    s_settings_open_ms = esp_timer_get_time() / 1000LL;
    lv_indev_wait_release(lv_indev_get_act());

    app_settings_t cfg;
    app_settings_get(&cfg);

    s_settings_modal = lv_obj_create(s_scr);
    lv_obj_set_size(s_settings_modal, UI_W, UI_H);
    lv_obj_set_pos(s_settings_modal, 0, 0);
    style_clean_box(s_settings_modal, COL_BG, COL_BG, 0);

    /* Left Card: Brightness, Screen Timeout, Sleep, Rotation (305x152) */
    lv_obj_t *c1 = lv_obj_create(s_settings_modal);
    lv_obj_set_pos(c1, 10, 10);
    lv_obj_set_size(c1, 305, 152);
    style_clean_box(c1, COL_CARD_BG, COL_BORDER, 16);

    lv_obj_t *ttl = lv_label_create(c1);
    lv_label_set_text(ttl, "显示与待机设置");
    lv_obj_set_style_text_font(ttl, &font_cn_16, 0);
    lv_obj_set_style_text_color(ttl, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_pos(ttl, 14, 10);

    s_set_bright_lbl = lv_label_create(c1);
    lv_obj_set_style_text_font(s_set_bright_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_set_bright_lbl, COL_TEXT_MAIN, 0);
    lv_obj_align(s_set_bright_lbl, LV_ALIGN_TOP_RIGHT, -14, 10);

    lv_obj_t *sl = lv_slider_create(c1);
    lv_obj_set_pos(sl, 16, 40);
    lv_obj_set_size(sl, 273, 12);
    lv_obj_clear_flag(sl, LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_set_ext_click_area(sl, 10);
    lv_slider_set_range(sl, 10, 100);
    lv_slider_set_value(sl, cfg.brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0x38BDF8), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, COL_TEXT_MAIN, LV_PART_KNOB);
    lv_obj_add_event_cb(sl, on_set_bright_slider, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(sl, on_set_bright_slider, LV_EVENT_RELEASED, NULL);

    lv_obj_t *bslp = make_set_btn(c1, 12, 64, 137, 36, COL_ITEM_BG, COL_BORDER, on_set_cycle_sleep);
    s_set_sleep_lbl = lv_label_create(bslp);
    lv_obj_set_style_text_font(s_set_sleep_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_set_sleep_lbl, COL_TEXT_MAIN, 0);
    lv_obj_center(s_set_sleep_lbl);

    lv_obj_t *bslp_now = make_set_btn(c1, 155, 64, 138, 36, COL_ITEM_BG, COL_BORDER, on_set_sleep_now);
    lv_obj_t *lslp_now = lv_label_create(bslp_now);
    lv_label_set_text(lslp_now, "立即息屏休眠");
    lv_obj_set_style_text_font(lslp_now, &font_cn_16, 0);
    lv_obj_set_style_text_color(lslp_now, lv_color_hex(0x38BDF8), 0);
    lv_obj_center(lslp_now);

    lv_obj_t *brm = make_set_btn(c1, 12, 106, 137, 36, COL_ITEM_BG, COL_BORDER, on_set_toggle_rot);
    s_set_rot_lbl = lv_label_create(brm);
    lv_obj_set_style_text_font(s_set_rot_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_set_rot_lbl, COL_TEXT_MAIN, 0);
    lv_obj_center(s_set_rot_lbl);

    lv_obj_t *bflp = make_set_btn(c1, 155, 106, 138, 36, COL_ITEM_BG, COL_BORDER, on_set_flip_landscape);
    lv_obj_t *lflp = lv_label_create(bflp);
    lv_label_set_text(lflp, "翻转横屏 180°");
    lv_obj_set_style_text_font(lflp, &font_cn_16, 0);
    lv_obj_set_style_text_color(lflp, COL_TEXT_MAIN, 0);
    lv_obj_center(lflp);

    /* Right Card: Wi-Fi/Volume, RTC Alarm, NTP Sync, Power Off, Back (305x152) */
    lv_obj_t *c2 = lv_obj_create(s_settings_modal);
    lv_obj_set_pos(c2, 325, 10);
    lv_obj_set_size(c2, 305, 152);
    style_clean_box(c2, COL_CARD_BG, COL_BORDER, 16);

    s_set_net_lbl = lv_label_create(c2);
    lv_obj_set_pos(s_set_net_lbl, 12, 10);
    lv_obj_set_width(s_set_net_lbl, 186);
    enable_marquee_scroll(s_set_net_lbl);
    lv_obj_set_style_text_font(s_set_net_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_set_net_lbl, COL_TEXT_MAIN, 0);

    lv_obj_t *bvol = make_set_btn(c2, 204, 8, 91, 28, COL_ITEM_BG, COL_BORDER, on_set_cycle_vol);
    s_set_vol_lbl = lv_label_create(bvol);
    lv_obj_set_style_text_font(s_set_vol_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_set_vol_lbl, lv_color_hex(0x38BDF8), 0);
    lv_obj_center(s_set_vol_lbl);

    lv_obj_t *hsep = lv_obj_create(c2);
    lv_obj_set_pos(hsep, 12, 46);
    lv_obj_set_size(hsep, 281, 1);
    lv_obj_set_style_bg_color(hsep, COL_BORDER, 0);
    lv_obj_set_style_bg_opa(hsep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hsep, 0, 0);

    lv_obj_t *balm_tog = make_set_btn(c2, 10, 58, 114, 38, COL_ITEM_BG, COL_BORDER, on_set_alarm_toggle);
    s_set_alarm_lbl = lv_label_create(balm_tog);
    lv_obj_set_style_text_font(s_set_alarm_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_set_alarm_lbl, COL_TEXT_MAIN, 0);
    lv_obj_center(s_set_alarm_lbl);

    lv_obj_t *balm_hr = make_set_btn(c2, 128, 58, 46, 38, COL_ITEM_BG, COL_BORDER, on_set_alarm_hr);
    lv_obj_add_event_cb(balm_hr, on_set_alarm_hr, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
    lv_obj_t *lahr = lv_label_create(balm_hr);
    lv_label_set_text(lahr, "时+1");
    lv_obj_set_style_text_font(lahr, &font_cn_16, 0);
    lv_obj_set_style_text_color(lahr, COL_TEXT_MAIN, 0);
    lv_obj_center(lahr);

    lv_obj_t *balm_min = make_set_btn(c2, 178, 58, 46, 38, COL_ITEM_BG, COL_BORDER, on_set_alarm_min);
    lv_obj_add_event_cb(balm_min, on_set_alarm_min, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
    lv_obj_t *lamn = lv_label_create(balm_min);
    lv_label_set_text(lamn, "分+5");
    lv_obj_set_style_text_font(lamn, &font_cn_16, 0);
    lv_obj_set_style_text_color(lamn, COL_TEXT_MAIN, 0);
    lv_obj_center(lamn);

    lv_obj_t *balm_tst = make_set_btn(c2, 228, 58, 67, 38, COL_ITEM_BG, COL_BORDER, on_set_alarm_test);
    lv_obj_t *latst = lv_label_create(balm_tst);
    lv_label_set_text(latst, "10s测");
    lv_obj_set_style_text_font(latst, &font_cn_16, 0);
    lv_obj_set_style_text_color(latst, lv_color_hex(0x38BDF8), 0);
    lv_obj_center(latst);

    lv_obj_t *bntp = make_set_btn(c2, 10, 104, 88, 38, COL_ITEM_BG, COL_BORDER, on_set_ntp_sync);
    s_set_ntp_lbl = lv_label_create(bntp);
    lv_label_set_text(s_set_ntp_lbl, "立即校时");
    lv_obj_set_style_text_font(s_set_ntp_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_set_ntp_lbl, COL_TEXT_MAIN, 0);
    lv_obj_center(s_set_ntp_lbl);

    lv_obj_t *bpwr = make_set_btn(c2, 104, 104, 96, 38, lv_color_hex(0x3B1317), lv_color_hex(0xEF4444), on_set_shutdown);
    lv_obj_t *lpwr = lv_label_create(bpwr);
    lv_label_set_text(lpwr, LV_SYMBOL_POWER " 关机");
    lv_obj_set_style_text_font(lpwr, &font_cn_16, 0);
    lv_obj_set_style_text_color(lpwr, lv_color_hex(0xF87171), 0);
    lv_obj_center(lpwr);

    lv_obj_t *bbk = make_set_btn(c2, 206, 104, 89, 38, COL_ITEM_BG, lv_color_hex(0x38BDF8), on_set_back);
    lv_obj_t *lbk = lv_label_create(bbk);
    lv_label_set_text(lbk, "返回");
    lv_obj_set_style_text_font(lbk, &font_cn_16, 0);
    lv_obj_set_style_text_color(lbk, lv_color_hex(0x38BDF8), 0);
    lv_obj_center(lbk);

    refresh_settings_modal();
}

#define DRAWER_W            224
#define DRAWER_OPEN_X       408
#define DRAWER_CLOSED_X     644
#define DRAWER_ANIM_IN_MS   180
#define DRAWER_ANIM_OUT_MS  150
#define DRAWER_AUTO_CLOSE_MS 8000

static void drawer_set_x_cb(void *var, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)var, (lv_coord_t)v);
}

static void drawer_close_done_cb(lv_anim_t *a)
{
    (void)a;
    if (s_drawer_mask && !s_drawer_open) {
        lv_obj_add_flag(s_drawer_mask, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_drawer_open(bool open, bool animate)
{
    if (!s_drawer_panel || !s_drawer_mask) return;

    lv_indev_t *indev = lv_indev_get_act();
    if (indev) {
        lv_indev_wait_release(indev);
    }

    s_drawer_open = open;
    if (open) {
        s_drawer_open_ms = esp_timer_get_time() / 1000LL;
        lv_obj_clear_flag(s_drawer_mask, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_drawer_mask);
        lv_obj_move_foreground(s_drawer_panel);
    }

    lv_anim_del(s_drawer_panel, drawer_set_x_cb);

    if (!animate) {
        lv_obj_set_x(s_drawer_panel, open ? DRAWER_OPEN_X : DRAWER_CLOSED_X);
        if (!open) {
            lv_obj_add_flag(s_drawer_mask, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    lv_coord_t cur_x = lv_obj_get_x(s_drawer_panel);
    lv_coord_t tgt_x = open ? DRAWER_OPEN_X : DRAWER_CLOSED_X;

    lv_anim_t ax;
    lv_anim_init(&ax);
    lv_anim_set_var(&ax, s_drawer_panel);
    lv_anim_set_exec_cb(&ax, drawer_set_x_cb);
    lv_anim_set_values(&ax, cur_x, tgt_x);
    lv_anim_set_time(&ax, open ? DRAWER_ANIM_IN_MS : DRAWER_ANIM_OUT_MS);
    lv_anim_set_path_cb(&ax, open ? lv_anim_path_ease_out : lv_anim_path_ease_in);
    if (!open) {
        lv_anim_set_ready_cb(&ax, drawer_close_done_cb);
    }
    lv_anim_start(&ax);
}

static void on_drawer_handle_click(lv_event_t *e)
{
    (void)e;
    bsp_display_reset_idle_timer();
    if (!s_rendered_active && !s_portal_modal && !s_settings_modal && !s_drawer_open) {
        set_drawer_open(true, true);
    }
}

static void on_drawer_mask_click(lv_event_t *e)
{
    (void)e;
    bsp_display_reset_idle_timer();
    int64_t now_ms = esp_timer_get_time() / 1000LL;
    if ((now_ms - s_drawer_open_ms) < 300LL) {
        return;
    }
    if (s_drawer_open) {
        set_drawer_open(false, true);
    }
}

static void on_screen_gesture(lv_event_t *e)
{
    (void)e;
    if (s_rendered_active || s_portal_modal || s_settings_modal) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT && !s_drawer_open) {
        bsp_display_reset_idle_timer();
        set_drawer_open(true, true);
    } else if (dir == LV_DIR_RIGHT && s_drawer_open) {
        bsp_display_reset_idle_timer();
        set_drawer_open(false, true);
    }
}

static void on_open_wifi_click(lv_event_t *e)
{
    (void)e;
    bsp_display_reset_idle_timer();
    bsp_audio_play_preset(AUDIO_PRESET_CLICK, 1);
    set_drawer_open(false, false);
    open_portal_modal(true);
}

static void on_open_settings_click(lv_event_t *e)
{
    (void)e;
    bsp_display_reset_idle_timer();
    bsp_audio_play_preset(AUDIO_PRESET_CLICK, 1);
    set_drawer_open(false, false);
    open_settings_modal();
}

static lv_obj_t *make_drawer_tile_btn(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                      lv_coord_t w, lv_coord_t h, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    configure_responsive_btn(btn, 6);
    lv_obj_set_style_bg_color(btn, COL_ITEM_BG, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, COL_BORDER, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x38BDF8), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x7DD3FC), LV_STATE_PRESSED);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    }
    return btn;
}

/* ---------- Build Idle Dashboard (640x172 Full-Width Clock) ---------- */

static void build_idle_ui(void)
{
    s_idle_root = lv_obj_create(s_scr);
    lv_obj_set_size(s_idle_root, UI_W, UI_H);
    lv_obj_set_pos(s_idle_root, 0, 0);
    style_clean_box(s_idle_root, COL_BG, COL_BG, 0);

    /* Full-width Immersive Clock Card (620x152, rtc_desk_clock style) */
    lv_obj_t *card = lv_obj_create(s_idle_root);
    lv_obj_set_pos(card, 10, 10);
    lv_obj_set_size(card, 620, 152);
    style_clean_box(card, COL_CARD_BG, COL_BORDER, 20);

    /* Top-left Date + Weekday */
    s_idle_date_lbl = lv_label_create(card);
    lv_obj_set_style_text_font(s_idle_date_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_idle_date_lbl, COL_TEXT_MUTED, 0);
    lv_obj_align(s_idle_date_lbl, LV_ALIGN_TOP_LEFT, 22, 12);
    lv_label_set_text(s_idle_date_lbl, "等待校时");

    /* Top-right Subtle Status Pill */
    s_idle_net_pill = lv_obj_create(card);
    lv_obj_clear_flag(s_idle_net_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_idle_net_pill, LV_SIZE_CONTENT, 24);
    lv_obj_set_style_bg_color(s_idle_net_pill, COL_ITEM_BG, 0);
    lv_obj_set_style_bg_opa(s_idle_net_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_idle_net_pill, COL_BORDER, 0);
    lv_obj_set_style_border_width(s_idle_net_pill, 1, 0);
    lv_obj_set_style_radius(s_idle_net_pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(s_idle_net_pill, 10, 0);
    lv_obj_set_style_pad_ver(s_idle_net_pill, 1, 0);
    lv_obj_set_flex_flow(s_idle_net_pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_idle_net_pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_idle_net_pill, 6, 0);
    lv_obj_align(s_idle_net_pill, LV_ALIGN_TOP_RIGHT, -18, 10);

    s_idle_net_dot = lv_obj_create(s_idle_net_pill);
    lv_obj_set_size(s_idle_net_dot, 7, 7);
    lv_obj_set_style_radius(s_idle_net_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_idle_net_dot, 0, 0);
    lv_obj_set_style_bg_color(s_idle_net_dot, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_bg_opa(s_idle_net_dot, LV_OPA_COVER, 0);

    s_idle_net_badge = lv_label_create(s_idle_net_pill);
    lv_obj_set_style_text_font(s_idle_net_badge, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_idle_net_badge, lv_color_hex(0x38BDF8), 0);
    lv_label_set_text(s_idle_net_badge, "AI 待命");

    /* Top-right Minimalist Battery Pill (just left of AI status pill) */
    s_idle_bat_pill = lv_obj_create(card);
    lv_obj_clear_flag(s_idle_bat_pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_idle_bat_pill, LV_SIZE_CONTENT, 24);
    lv_obj_set_style_bg_color(s_idle_bat_pill, COL_ITEM_BG, 0);
    lv_obj_set_style_bg_opa(s_idle_bat_pill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_idle_bat_pill, COL_BORDER, 0);
    lv_obj_set_style_border_width(s_idle_bat_pill, 1, 0);
    lv_obj_set_style_radius(s_idle_bat_pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(s_idle_bat_pill, 9, 0);
    lv_obj_set_style_pad_ver(s_idle_bat_pill, 1, 0);
    lv_obj_set_flex_flow(s_idle_bat_pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_idle_bat_pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_idle_bat_pill, 5, 0);
    lv_obj_align(s_idle_bat_pill, LV_ALIGN_TOP_RIGHT, -116, 10);

    /* Crisp mini battery icon (shell + terminal nub + dynamic fill) */
    lv_obj_t *bat_ico = lv_obj_create(s_idle_bat_pill);
    lv_obj_clear_flag(bat_ico, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(bat_ico, 22, 12);
    lv_obj_set_style_bg_opa(bat_ico, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bat_ico, 0, 0);
    lv_obj_set_style_pad_all(bat_ico, 0, 0);

    lv_obj_t *bat_shell = lv_obj_create(bat_ico);
    lv_obj_clear_flag(bat_shell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(bat_shell, 0, 0);
    lv_obj_set_size(bat_shell, 19, 12);
    lv_obj_set_style_bg_color(bat_shell, COL_BG, 0);
    lv_obj_set_style_bg_opa(bat_shell, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bat_shell, COL_TEXT_MUTED, 0);
    lv_obj_set_style_border_width(bat_shell, 1, 0);
    lv_obj_set_style_radius(bat_shell, 3, 0);
    lv_obj_set_style_pad_all(bat_shell, 0, 0);

    lv_obj_t *bat_nub = lv_obj_create(bat_ico);
    lv_obj_clear_flag(bat_nub, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(bat_nub, 19, 3);
    lv_obj_set_size(bat_nub, 2, 6);
    lv_obj_set_style_bg_color(bat_nub, COL_TEXT_MUTED, 0);
    lv_obj_set_style_bg_opa(bat_nub, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bat_nub, 0, 0);
    lv_obj_set_style_radius(bat_nub, 1, 0);

    s_idle_bat_fill = lv_obj_create(bat_shell);
    lv_obj_clear_flag(s_idle_bat_fill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_idle_bat_fill, 2, 2);
    lv_obj_set_size(s_idle_bat_fill, 13, 6);
    lv_obj_set_style_bg_color(s_idle_bat_fill, lv_color_hex(0x4ADE80), 0);
    lv_obj_set_style_bg_opa(s_idle_bat_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_idle_bat_fill, 0, 0);
    lv_obj_set_style_radius(s_idle_bat_fill, 1, 0);

    s_idle_bat_lbl = lv_label_create(s_idle_bat_pill);
    lv_obj_set_style_text_font(s_idle_bat_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_idle_bat_lbl, COL_TEXT_MAIN, 0);
    lv_label_set_text(s_idle_bat_lbl, "100%");

    /* Visually Centered Large Time: HH : MM | 24H SS */
    s_idle_hh_lbl = lv_label_create(card);
    lv_obj_set_style_text_font(s_idle_hh_lbl, &font_clock_96, 0);
    lv_obj_set_style_text_color(s_idle_hh_lbl, COL_TEXT_MAIN, 0);
    lv_obj_set_pos(s_idle_hh_lbl, 136, 46);
    lv_label_set_text(s_idle_hh_lbl, "12");

    s_idle_colon_lbl = lv_label_create(card);
    lv_label_set_text(s_idle_colon_lbl, ":");
    lv_obj_set_style_text_font(s_idle_colon_lbl, &font_clock_96, 0);
    lv_obj_set_style_text_color(s_idle_colon_lbl, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_pos(s_idle_colon_lbl, 256, 36);

    s_idle_mm_lbl = lv_label_create(card);
    lv_obj_set_style_text_font(s_idle_mm_lbl, &font_clock_96, 0);
    lv_obj_set_style_text_color(s_idle_mm_lbl, COL_TEXT_MAIN, 0);
    lv_obj_set_pos(s_idle_mm_lbl, 284, 46);
    lv_label_set_text(s_idle_mm_lbl, "00");

    lv_obj_t *vsep = lv_obj_create(card);
    lv_obj_set_pos(vsep, 414, 55);
    lv_obj_set_size(vsep, 2, 54);
    lv_obj_set_style_bg_color(vsep, COL_BORDER, 0);
    lv_obj_set_style_bg_opa(vsep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(vsep, 0, 0);

    lv_obj_t *lbl_24h = lv_label_create(card);
    lv_label_set_text(lbl_24h, "24H");
    lv_obj_set_style_text_font(lbl_24h, &font_cn_16, 0);
    lv_obj_set_style_text_color(lbl_24h, COL_TEXT_MUTED, 0);
    lv_obj_set_pos(lbl_24h, 428, 54);

    s_idle_ss_lbl = lv_label_create(card);
    lv_obj_set_style_text_font(s_idle_ss_lbl, &font_clock_32, 0);
    lv_obj_set_style_text_color(s_idle_ss_lbl, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_pos(s_idle_ss_lbl, 428, 80);
    lv_label_set_text(s_idle_ss_lbl, "00");

    /* Full-width 60s Progress Bar */
    s_idle_bar_sec = lv_bar_create(card);
    lv_obj_add_flag(s_idle_bar_sec, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_size(s_idle_bar_sec, 572, 5);
    lv_obj_align(s_idle_bar_sec, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_bar_set_range(s_idle_bar_sec, 0, 59);
    lv_bar_set_value(s_idle_bar_sec, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_idle_bar_sec, COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_idle_bar_sec, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_idle_bar_sec, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_idle_bar_sec, lv_color_hex(0x38BDF8), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_idle_bar_sec, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

    /* Subtle floating right-edge grab handle + invisible hitbox (rtc_desk_clock style) */
    lv_obj_t *handle_bar = lv_obj_create(card);
    lv_obj_add_flag(handle_bar, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_size(handle_bar, 4, 38);
    lv_obj_align(handle_bar, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_set_style_bg_color(handle_bar, COL_TEXT_MUTED, 0);
    lv_obj_set_style_bg_opa(handle_bar, LV_OPA_40, 0);
    lv_obj_set_style_border_width(handle_bar, 0, 0);
    lv_obj_set_style_radius(handle_bar, LV_RADIUS_CIRCLE, 0);

    lv_obj_t *hit = lv_obj_create(card);
    lv_obj_clear_flag(hit, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_size(hit, 48, 152);
    lv_obj_align(hit, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hit, 0, 0);
    lv_obj_add_event_cb(hit, on_drawer_handle_click, LV_EVENT_CLICKED, NULL);

    /* Slide-Out Drawer Mask (left side outside drawer: 408x172) */
    s_drawer_mask = lv_obj_create(s_idle_root);
    lv_obj_clear_flag(s_drawer_mask, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_add_flag(s_drawer_mask, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_pos(s_drawer_mask, 0, 0);
    lv_obj_set_size(s_drawer_mask, DRAWER_OPEN_X, UI_H);
    lv_obj_set_style_bg_color(s_drawer_mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_drawer_mask, 0, 0);
    lv_obj_set_style_border_width(s_drawer_mask, 0, 0);
    lv_obj_set_style_radius(s_drawer_mask, 0, 0);
    lv_obj_set_style_pad_all(s_drawer_mask, 0, 0);
    lv_obj_add_flag(s_drawer_mask, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_drawer_mask, on_drawer_mask_click, LV_EVENT_CLICKED, NULL);

    /* Slide-Out Drawer Card (224x156, radius 18, cyan accent border) */
    s_drawer_panel = lv_obj_create(s_idle_root);
    style_clean_box(s_drawer_panel, COL_DOCK_BG, lv_color_hex(0x38BDF8), 18);
    lv_obj_add_flag(s_drawer_panel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_size(s_drawer_panel, DRAWER_W, 156);
    lv_obj_set_pos(s_drawer_panel, DRAWER_CLOSED_X, 8);

    /* Top Status Info inside Drawer */
    s_drawer_wifi_dot = lv_obj_create(s_drawer_panel);
    lv_obj_set_pos(s_drawer_wifi_dot, 14, 16);
    lv_obj_set_size(s_drawer_wifi_dot, 8, 8);
    lv_obj_set_style_radius(s_drawer_wifi_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_drawer_wifi_dot, 0, 0);
    lv_obj_set_style_bg_color(s_drawer_wifi_dot, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_bg_opa(s_drawer_wifi_dot, LV_OPA_COVER, 0);

    s_drawer_wifi_lbl = lv_label_create(s_drawer_panel);
    lv_obj_set_pos(s_drawer_wifi_lbl, 28, 10);
    lv_obj_set_width(s_drawer_wifi_lbl, 182);
    lv_label_set_long_mode(s_drawer_wifi_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(s_drawer_wifi_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_drawer_wifi_lbl, COL_TEXT_MAIN, 0);
    lv_label_set_text(s_drawer_wifi_lbl, "Wi-Fi 连接中");

    s_drawer_bat_lbl = lv_label_create(s_drawer_panel);
    lv_obj_set_pos(s_drawer_bat_lbl, 14, 40);
    lv_obj_set_style_text_font(s_drawer_bat_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_drawer_bat_lbl, COL_TEXT_MUTED, 0);
    lv_label_set_text(s_drawer_bat_lbl, LV_SYMBOL_CHARGE " USB");

    s_drawer_audio_tag = lv_label_create(s_drawer_panel);
    lv_obj_align(s_drawer_audio_tag, LV_ALIGN_TOP_RIGHT, -14, 40);
    lv_obj_set_style_text_font(s_drawer_audio_tag, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_drawer_audio_tag, lv_color_hex(0x38BDF8), 0);
    lv_label_set_text(s_drawer_audio_tag, bsp_audio_is_muted() ? "已静音" : "有声");

    lv_obj_t *hsep = lv_obj_create(s_drawer_panel);
    lv_obj_set_pos(hsep, 14, 70);
    lv_obj_set_size(hsep, 196, 1);
    lv_obj_set_style_bg_color(hsep, COL_BORDER, 0);
    lv_obj_set_style_bg_opa(hsep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(hsep, 0, 0);

    /* Bottom 3 Square Icon+Text Tiles (62x62, radius 14: 静音 / 配网 / 设置) */
    bool muted = bsp_audio_is_muted();

    lv_obj_t *b_mute = make_drawer_tile_btn(s_drawer_panel, 12, 80, 62, 62, on_toggle_mute_click);
    s_idle_mute_ico_lbl = lv_label_create(b_mute);
    lv_label_set_text(s_idle_mute_ico_lbl, muted ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
    lv_obj_set_style_text_font(s_idle_mute_ico_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_idle_mute_ico_lbl, lv_color_hex(0x38BDF8), 0);
    lv_obj_align(s_idle_mute_ico_lbl, LV_ALIGN_TOP_MID, 0, 10);
    s_idle_mute_btn_lbl = lv_label_create(b_mute);
    lv_label_set_text(s_idle_mute_btn_lbl, muted ? "有声" : "静音");
    lv_obj_set_style_text_font(s_idle_mute_btn_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_idle_mute_btn_lbl, COL_TEXT_MAIN, 0);
    lv_obj_align(s_idle_mute_btn_lbl, LV_ALIGN_BOTTOM_MID, 0, -9);

    lv_obj_t *b_wifi = make_drawer_tile_btn(s_drawer_panel, 81, 80, 62, 62, on_open_wifi_click);
    lv_obj_t *ic_wifi = lv_label_create(b_wifi);
    lv_label_set_text(ic_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(ic_wifi, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ic_wifi, lv_color_hex(0x38BDF8), 0);
    lv_obj_align(ic_wifi, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_t *lb_wifi = lv_label_create(b_wifi);
    lv_label_set_text(lb_wifi, "配网");
    lv_obj_set_style_text_font(lb_wifi, &font_cn_16, 0);
    lv_obj_set_style_text_color(lb_wifi, COL_TEXT_MAIN, 0);
    lv_obj_align(lb_wifi, LV_ALIGN_BOTTOM_MID, 0, -9);

    lv_obj_t *b_set = make_drawer_tile_btn(s_drawer_panel, 150, 80, 62, 62, on_open_settings_click);
    lv_obj_t *ic_set = lv_label_create(b_set);
    lv_label_set_text(ic_set, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(ic_set, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ic_set, lv_color_hex(0x38BDF8), 0);
    lv_obj_align(ic_set, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_t *lb_set = lv_label_create(b_set);
    lv_label_set_text(lb_set, "设置");
    lv_obj_set_style_text_font(lb_set, &font_cn_16, 0);
    lv_obj_set_style_text_color(lb_set, COL_TEXT_MAIN, 0);
    lv_obj_align(lb_set, LV_ALIGN_BOTTOM_MID, 0, -9);
}

/* ---------- Build Active DSL 3-Zone Skeleton (640x172) ---------- */

static void build_active_skeleton(void)
{
    s_active_root = lv_obj_create(s_scr);
    lv_obj_set_size(s_active_root, UI_W, UI_H);
    lv_obj_set_pos(s_active_root, 0, 0);
    style_clean_box(s_active_root, COL_BG, COL_BG, 0);
    lv_obj_add_flag(s_active_root, LV_OBJ_FLAG_HIDDEN);

    /* Zone 1: Left Status Dock (128 x 160) */
    s_left_dock = lv_obj_create(s_active_root);
    lv_obj_set_size(s_left_dock, 128, 160);
    lv_obj_set_pos(s_left_dock, 4, 4);
    style_clean_box(s_left_dock, COL_DOCK_BG, COL_BORDER, 10);

    s_state_pill = lv_obj_create(s_left_dock);
    lv_obj_set_size(s_state_pill, 114, 28);
    lv_obj_set_pos(s_state_pill, 7, 8);
    style_clean_box(s_state_pill, lv_color_hex(0x0D2C3A), lv_color_hex(0x06B6D4), 14);

    s_state_dot = lv_obj_create(s_state_pill);
    lv_obj_set_size(s_state_dot, 8, 8);
    lv_obj_set_pos(s_state_dot, 8, 9);
    style_clean_box(s_state_dot, lv_color_hex(0x06B6D4), lv_color_hex(0x06B6D4), 4);

    s_state_lbl = lv_label_create(s_state_pill);
    lv_obj_set_pos(s_state_lbl, 20, 3);
    lv_obj_set_size(s_state_lbl, 88, 22);
    lv_obj_set_style_text_font(s_state_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_state_lbl, COL_TEXT_MAIN, 0);
    lv_label_set_long_mode(s_state_lbl, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_state_lbl, "等待确认");

    s_agent_lbl = lv_label_create(s_left_dock);
    lv_obj_set_pos(s_agent_lbl, 8, 44);
    lv_obj_set_size(s_agent_lbl, 112, 22);
    lv_obj_set_style_text_font(s_agent_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_agent_lbl, COL_TEXT_MUTED, 0);
    enable_marquee_scroll(s_agent_lbl);
    lv_label_set_text(s_agent_lbl, "AI Agent");

    s_countdown_lbl = lv_label_create(s_left_dock);
    lv_obj_set_pos(s_countdown_lbl, 8, 70);
    lv_obj_set_size(s_countdown_lbl, 112, 34);
    lv_obj_set_style_text_font(s_countdown_lbl, &font_clock_32, 0);
    lv_obj_set_style_text_color(s_countdown_lbl, COL_TEXT_MAIN, 0);
    lv_label_set_text(s_countdown_lbl, "");

    lv_obj_t *mute_chip = lv_btn_create(s_left_dock);
    lv_obj_set_size(mute_chip, 114, 24);
    lv_obj_set_pos(mute_chip, 7, 108);
    configure_responsive_btn(mute_chip, 8);
    lv_obj_set_style_bg_color(mute_chip, COL_ITEM_BG, 0);
    lv_obj_set_style_bg_color(mute_chip, lv_color_hex(0x38BDF8), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(mute_chip, COL_BORDER, 0);
    lv_obj_set_style_border_width(mute_chip, 1, 0);
    lv_obj_set_style_radius(mute_chip, 6, 0);
    lv_obj_add_event_cb(mute_chip, on_toggle_mute_click, LV_EVENT_CLICKED, NULL);
    s_mute_chip_lbl = lv_label_create(mute_chip);
    lv_obj_set_style_text_font(s_mute_chip_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_mute_chip_lbl, COL_TEXT_MUTED, 0);
    lv_label_set_text(s_mute_chip_lbl, bsp_audio_is_muted() ? "静音: 开" : "声音: 开");
    lv_obj_center(s_mute_chip_lbl);

    s_left_meta_lbl = lv_label_create(s_left_dock);
    lv_obj_set_pos(s_left_meta_lbl, 8, 136);
    lv_obj_set_size(s_left_meta_lbl, 112, 20);
    lv_obj_set_style_text_font(s_left_meta_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_left_meta_lbl, COL_TEXT_DIM, 0);
    lv_label_set_long_mode(s_left_meta_lbl, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_left_meta_lbl, "IP --");

    /* Zone 2: Center Structured Content Canvas (368 x 160) */
    s_center_card = lv_obj_create(s_active_root);
    lv_obj_set_size(s_center_card, 368, 160);
    lv_obj_set_pos(s_center_card, 136, 4);
    style_clean_box(s_center_card, COL_CARD_BG, COL_BORDER, 10);

    s_title_lbl = lv_label_create(s_center_card);
    lv_obj_set_pos(s_title_lbl, 12, 6);
    lv_obj_set_size(s_title_lbl, 344, 22);
    lv_obj_set_style_text_font(s_title_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_title_lbl, COL_TEXT_MAIN, 0);
    enable_marquee_scroll(s_title_lbl);

    s_subtitle_lbl = lv_label_create(s_center_card);
    lv_obj_set_pos(s_subtitle_lbl, 12, 29);
    lv_obj_set_size(s_subtitle_lbl, 344, 22);
    lv_obj_set_style_text_font(s_subtitle_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(s_subtitle_lbl, COL_TEXT_MUTED, 0);
    enable_marquee_scroll(s_subtitle_lbl);

    s_body_box = lv_obj_create(s_center_card);
    lv_obj_set_size(s_body_box, 348, 102);
    lv_obj_set_pos(s_body_box, 10, 54);
    style_clean_box(s_body_box, COL_CARD_BG, COL_CARD_BG, 0);

    /* Zone 3: Right Fixed Action Dock (128 x 160) */
    s_right_dock = lv_obj_create(s_active_root);
    lv_obj_set_size(s_right_dock, 128, 160);
    lv_obj_set_pos(s_right_dock, 508, 4);
    style_clean_box(s_right_dock, COL_DOCK_BG, COL_BORDER, 10);

    /* Bottom Global Countdown Bar (640 x 4) */
    s_timeout_bar = lv_bar_create(s_active_root);
    lv_obj_set_size(s_timeout_bar, UI_W, 4);
    lv_obj_set_pos(s_timeout_bar, 0, 168);
    lv_bar_set_range(s_timeout_bar, 0, 200);
    lv_bar_set_value(s_timeout_bar, 200, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_timeout_bar, lv_color_hex(0x162033), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_timeout_bar, lv_color_hex(0x06B6D4), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_timeout_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_timeout_bar, 0, LV_PART_INDICATOR);
}

/* ---------- Populate Structured DSL Body Layouts ---------- */

static void render_body_text(const dsl_request_t *req, state_theme_t th)
{
    bool has_callout = (req->callout[0] != '\0');
    int scroll_h = has_callout ? 64 : 98;

    lv_obj_t *scroll_box = lv_obj_create(s_body_box);
    lv_obj_set_pos(scroll_box, 2, 0);
    lv_obj_set_size(scroll_box, 344, scroll_h);
    lv_obj_set_style_bg_opa(scroll_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scroll_box, 0, 0);
    lv_obj_set_style_pad_all(scroll_box, 0, 0);
    lv_obj_set_scroll_dir(scroll_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll_box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(scroll_box, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(scroll_box, th.accent, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(scroll_box, LV_OPA_60, LV_PART_SCROLLBAR);

    lv_obj_t *lbl = lv_label_create(scroll_box);
    lv_obj_set_pos(lbl, 0, 0);
    lv_obj_set_width(lbl, 336);
    lv_obj_set_style_text_font(lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(lbl, COL_TEXT_MAIN, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(lbl, req->body[0] ? req->body : "(无正文说明)");

    if (has_callout) {
        lv_obj_t *box = lv_obj_create(s_body_box);
        lv_obj_set_size(box, 344, 32);
        lv_obj_set_pos(box, 2, 68);
        style_clean_box(box, th.badge_bg, th.accent, 6);

        lv_obj_t *cl = lv_label_create(box);
        lv_obj_set_pos(cl, 8, 5);
        lv_obj_set_size(cl, 328, 22);
        lv_obj_set_style_text_font(cl, &font_cn_16, 0);
        lv_obj_set_style_text_color(cl, COL_TEXT_MAIN, 0);
        enable_marquee_scroll(cl);
        lv_label_set_text(cl, req->callout);
    }
}

static void render_body_kv(const dsl_request_t *req, state_theme_t th)
{
    int cnt = req->kv_count;
    if (cnt <= 0) {
        render_body_text(req, th);
        return;
    }
    int row_h = (cnt <= 3) ? 30 : 23;
    int gap = 3;
    int lbl_y = (row_h - 22) / 2;
    if (lbl_y < 0) lbl_y = 0;
    int lbl_h = (row_h < 22) ? row_h : 22;
    for (int i = 0; i < cnt && i < DSL_MAX_KV_ITEMS; i++) {
        lv_obj_t *row = lv_obj_create(s_body_box);
        lv_obj_set_size(row, 344, row_h);
        lv_obj_set_pos(row, 2, i * (row_h + gap));
        style_clean_box(row, COL_ITEM_BG,
                        req->kv_items[i].highlight ? th.accent : COL_BORDER, 6);

        lv_obj_t *lk = lv_label_create(row);
        lv_obj_set_pos(lk, 8, lbl_y);
        lv_obj_set_size(lk, 96, lbl_h);
        lv_obj_set_style_text_font(lk, &font_cn_16, 0);
        lv_obj_set_style_text_color(lk, COL_TEXT_MUTED, 0);
        enable_marquee_scroll(lk);
        lv_label_set_text(lk, req->kv_items[i].key);

        lv_obj_t *lv = lv_label_create(row);
        lv_obj_set_pos(lv, 110, lbl_y);
        lv_obj_set_size(lv, 226, lbl_h);
        lv_obj_set_style_text_font(lv, &font_cn_16, 0);
        lv_obj_set_style_text_color(lv, req->kv_items[i].highlight ? th.accent : COL_TEXT_MAIN, 0);
        lv_obj_set_style_text_align(lv, LV_TEXT_ALIGN_LEFT, 0);
        enable_marquee_scroll(lv);
        lv_label_set_text(lv, req->kv_items[i].val);
    }
}

static void render_body_progress(const dsl_request_t *req, state_theme_t th)
{
    lv_obj_t *step_lbl = lv_label_create(s_body_box);
    lv_obj_set_pos(step_lbl, 2, 0);
    lv_obj_set_size(step_lbl, 270, 22);
    lv_obj_set_style_text_font(step_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(step_lbl, COL_TEXT_MAIN, 0);
    enable_marquee_scroll(step_lbl);
    lv_label_set_text(step_lbl, req->step_text[0] ? req->step_text : req->body);

    lv_obj_t *pct_lbl = lv_label_create(s_body_box);
    lv_obj_set_pos(pct_lbl, 276, 0);
    lv_obj_set_size(pct_lbl, 70, 22);
    lv_obj_set_style_text_font(pct_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(pct_lbl, th.accent, 0);
    lv_obj_set_style_text_align(pct_lbl, LV_TEXT_ALIGN_RIGHT, 0);
    char pbuf[16];
    snprintf(pbuf, sizeof(pbuf), "%u%%", req->progress_percent);
    lv_label_set_text(pct_lbl, pbuf);

    lv_obj_t *bar = lv_bar_create(s_body_box);
    lv_obj_set_size(bar, 344, 18);
    lv_obj_set_pos(bar, 2, 28);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, req->progress_percent, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, COL_ITEM_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, th.accent, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 9, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 9, LV_PART_INDICATOR);

    int sc = req->step_count;
    if (sc > 0) {
        int cw = (344 - (sc - 1) * 6) / sc;
        for (int i = 0; i < sc && i < DSL_MAX_STEPS; i++) {
            const dsl_step_item_t *st = &req->steps[i];
            lv_obj_t *chip = lv_obj_create(s_body_box);
            lv_obj_set_size(chip, cw, 38);
            lv_obj_set_pos(chip, 2 + i * (cw + 6), 58);

            lv_color_t bg = COL_ITEM_BG;
            lv_color_t bd = COL_BORDER;
            lv_color_t tc = COL_TEXT_MUTED;
            if (st->done) {
                bg = lv_color_hex(0x0E2F1E);
                bd = lv_color_hex(0x22C55E);
                tc = lv_color_hex(0x4ADE80);
            } else if (st->active) {
                bg = th.badge_bg;
                bd = th.accent;
                tc = COL_TEXT_MAIN;
            }
            style_clean_box(chip, bg, bd, 8);

            lv_obj_t *sl = lv_label_create(chip);
            lv_obj_set_width(sl, cw - 8);
            lv_obj_set_style_text_font(sl, &font_cn_16, 0);
            lv_obj_set_style_text_color(sl, tc, 0);
            lv_obj_set_style_text_align(sl, LV_TEXT_ALIGN_CENTER, 0);
            enable_marquee_scroll(sl);
            lv_label_set_text(sl, st->label);
            lv_obj_center(sl);
        }
    }
}

static void render_body_choice(const dsl_request_t *req, state_theme_t th)
{
    memset(s_choice_btns, 0, sizeof(s_choice_btns));
    int cnt = req->choice_count;
    if (cnt <= 0) {
        render_body_text(req, th);
        return;
    }

    int cols = (cnt <= 4) ? 2 : 3;
    int rows = (cnt + cols - 1) / cols;
    int gap_x = 6;
    int gap_y = 6;
    int cw = (344 - (cols - 1) * gap_x) / cols;
    int ch = (98 - (rows - 1) * gap_y) / rows;
    if (ch > 46) ch = 46;

    for (int i = 0; i < cnt && i < DSL_MAX_CHOICES; i++) {
        int r = i / cols;
        int c = i % cols;
        lv_obj_t *btn = lv_btn_create(s_body_box);
        lv_obj_set_size(btn, cw, ch);
        lv_obj_set_pos(btn, 2 + c * (cw + gap_x), r * (ch + gap_y));
        configure_responsive_btn(btn, 4);
        bool sel = req->choices[i].selected;
        lv_obj_set_style_bg_color(btn, sel ? th.badge_bg : COL_ITEM_BG, 0);
        lv_obj_set_style_bg_color(btn, th.accent, LV_STATE_PRESSED);
        lv_obj_set_style_border_color(btn, sel ? th.accent : COL_BORDER, 0);
        lv_obj_set_style_border_width(btn, sel ? 2 : 1, 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_add_event_cb(btn, on_choice_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_width(lbl, cw - 10);
        lv_obj_set_style_text_font(lbl, &font_cn_16, 0);
        lv_obj_set_style_text_color(lbl, COL_TEXT_MAIN, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        enable_marquee_scroll(lbl);
        lv_label_set_text(lbl, req->choices[i].label);
        lv_obj_center(lbl);

        s_choice_btns[i] = btn;
    }
}

static void render_body_number(const dsl_request_t *req, state_theme_t th)
{
    lv_obj_t *btn_dec = lv_btn_create(s_body_box);
    lv_obj_set_size(btn_dec, 64, 62);
    lv_obj_set_pos(btn_dec, 8, 6);
    configure_responsive_btn(btn_dec, 8);
    lv_obj_set_style_bg_color(btn_dec, COL_ITEM_BG, 0);
    lv_obj_set_style_bg_color(btn_dec, th.accent, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn_dec, th.accent, 0);
    lv_obj_set_style_border_width(btn_dec, 1, 0);
    lv_obj_set_style_radius(btn_dec, 10, 0);
    lv_obj_add_event_cb(btn_dec, on_number_step_click, LV_EVENT_CLICKED, (void *)(intptr_t)(-1));
    lv_obj_add_event_cb(btn_dec, on_number_step_click, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)(-1));
    lv_obj_t *ld = lv_label_create(btn_dec);
    lv_obj_set_style_text_font(ld, &font_clock_32, 0);
    lv_label_set_text(ld, "-");
    lv_obj_center(ld);

    lv_obj_t *val_box = lv_obj_create(s_body_box);
    lv_obj_set_size(val_box, 184, 62);
    lv_obj_set_pos(val_box, 82, 6);
    style_clean_box(val_box, COL_ITEM_BG, COL_BORDER, 10);

    s_num_val_lbl = lv_label_create(val_box);
    lv_obj_set_style_text_font(s_num_val_lbl, &font_clock_32, 0);
    lv_obj_set_style_text_color(s_num_val_lbl, th.accent, 0);
    char vbuf[24];
    snprintf(vbuf, sizeof(vbuf), "%ld", (long)req->num_value);
    lv_label_set_text(s_num_val_lbl, vbuf);
    if (req->num_unit[0] != '\0') {
        lv_obj_align(s_num_val_lbl, LV_ALIGN_CENTER, -14, 0);
        s_num_unit_lbl = lv_label_create(val_box);
        lv_obj_set_style_text_font(s_num_unit_lbl, &font_cn_16, 0);
        lv_obj_set_style_text_color(s_num_unit_lbl, COL_TEXT_MUTED, 0);
        lv_label_set_text(s_num_unit_lbl, req->num_unit);
        lv_obj_align_to(s_num_unit_lbl, s_num_val_lbl, LV_ALIGN_OUT_RIGHT_BOTTOM, 6, -3);
    } else {
        s_num_unit_lbl = NULL;
        lv_obj_center(s_num_val_lbl);
    }

    lv_obj_t *btn_inc = lv_btn_create(s_body_box);
    lv_obj_set_size(btn_inc, 64, 62);
    lv_obj_set_pos(btn_inc, 276, 6);
    configure_responsive_btn(btn_inc, 8);
    lv_obj_set_style_bg_color(btn_inc, COL_ITEM_BG, 0);
    lv_obj_set_style_bg_color(btn_inc, th.accent, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn_inc, th.accent, 0);
    lv_obj_set_style_border_width(btn_inc, 1, 0);
    lv_obj_set_style_radius(btn_inc, 10, 0);
    lv_obj_add_event_cb(btn_inc, on_number_step_click, LV_EVENT_CLICKED, (void *)(intptr_t)(1));
    lv_obj_add_event_cb(btn_inc, on_number_step_click, LV_EVENT_LONG_PRESSED_REPEAT, (void *)(intptr_t)(1));
    lv_obj_t *li = lv_label_create(btn_inc);
    lv_obj_set_style_text_font(li, &font_clock_32, 0);
    lv_label_set_text(li, "+");
    lv_obj_center(li);

    lv_obj_t *range_lbl = lv_label_create(s_body_box);
    lv_obj_set_pos(range_lbl, 8, 76);
    lv_obj_set_size(range_lbl, 332, 22);
    lv_obj_set_style_text_font(range_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(range_lbl, COL_TEXT_MUTED, 0);
    lv_obj_set_style_text_align(range_lbl, LV_TEXT_ALIGN_CENTER, 0);
    char rbuf[96];
    snprintf(rbuf, sizeof(rbuf), "范围: %ld ~ %ld %s (步进 %ld)",
             (long)req->num_min, (long)req->num_max, req->num_unit, (long)req->num_step);
    lv_label_set_text(range_lbl, rbuf);
}

static void render_right_buttons(const dsl_request_t *req)
{
    lv_obj_clean(s_right_dock);
    int cnt = req->button_count;
    if (cnt <= 0) {
        lv_obj_t *btn = lv_btn_create(s_right_dock);
        lv_obj_set_size(btn, 116, 148);
        lv_obj_set_pos(btn, 6, 6);
        configure_responsive_btn(btn, 6);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x27354F), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x38BDF8), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x475569), 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_add_event_cb(btn, on_dismiss_click, LV_EVENT_CLICKED, NULL);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_style_text_font(lbl, &font_cn_16, 0);
        lv_label_set_text(lbl, "已知晓");
        lv_obj_center(lbl);
        return;
    }

    int gap = 5;
    int total_h = 148;
    int bh = (total_h - (cnt - 1) * gap) / cnt;
    for (int i = 0; i < cnt && i < DSL_MAX_BUTTONS; i++) {
        const dsl_button_t *b = &req->buttons[i];
        lv_obj_t *btn = lv_btn_create(s_right_dock);
        lv_obj_set_size(btn, 116, bh);
        lv_obj_set_pos(btn, 6, 6 + i * (bh + gap));
        configure_responsive_btn(btn, 4);
        lv_obj_set_style_bg_color(btn, get_btn_bg(b->style), 0);
        lv_obj_set_style_bg_color(btn, get_btn_border(b->style), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(btn, get_btn_border(b->style), 0);
        lv_obj_set_style_border_width(btn, b->is_default ? 2 : 1, 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_add_event_cb(btn, on_dsl_button_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_obj_set_width(lbl, 108);
        lv_obj_set_style_text_font(lbl, &font_cn_16, 0);
        lv_obj_set_style_text_color(lbl, COL_TEXT_MAIN, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        enable_marquee_scroll(lbl);
        if (b->is_default && req->timeout_sec > 0) {
            char tmp[56];
            snprintf(tmp, sizeof(tmp), "★ %s", b->label);
            lv_label_set_text(lbl, tmp);
        } else {
            lv_label_set_text(lbl, b->label);
        }
        lv_obj_center(lbl);
    }
}

static void render_active_dsl(const dsl_request_t *req)
{
    state_theme_t th = get_state_theme(req->state);

    lv_obj_set_style_border_color(s_left_dock, th.accent, 0);
    lv_obj_set_style_border_color(s_center_card, th.accent, 0);
    lv_obj_set_style_bg_color(s_state_pill, th.badge_bg, 0);
    lv_obj_set_style_border_color(s_state_pill, th.accent, 0);
    lv_obj_set_style_bg_color(s_state_dot, th.accent, 0);
    lv_obj_set_style_border_color(s_state_dot, th.accent, 0);
    lv_obj_set_style_bg_color(s_timeout_bar, th.accent, LV_PART_INDICATOR);

    lv_label_set_text(s_state_lbl, app_dsl_state_to_cn(req->state));
    lv_label_set_text(s_agent_lbl, req->agent);
    lv_label_set_text(s_title_lbl, req->title);
    lv_label_set_text(s_subtitle_lbl, req->subtitle[0] ? req->subtitle : th.tag_en);

    if (s_mute_chip_lbl) {
        lv_label_set_text(s_mute_chip_lbl, bsp_audio_is_muted() ? "静音: 开" : "声音: 开");
    }

    s_num_val_lbl = NULL;
    s_num_unit_lbl = NULL;
    lv_obj_clean(s_body_box);
    switch (req->layout) {
    case DSL_LAYOUT_KV:       render_body_kv(req, th); break;
    case DSL_LAYOUT_PROGRESS: render_body_progress(req, th); break;
    case DSL_LAYOUT_CHOICE:   render_body_choice(req, th); break;
    case DSL_LAYOUT_NUMBER:   render_body_number(req, th); break;
    default:                  render_body_text(req, th); break;
    }

    render_right_buttons(req);
}

/* ---------- Periodic UI Sync Timer (100ms) ---------- */

static void set_label_if_changed(lv_obj_t *lbl, const char *txt)
{
    if (!lbl || !txt) return;
    const char *cur = lv_label_get_text(lbl);
    if (!cur || strcmp(cur, txt) != 0) {
        lv_label_set_text(lbl, txt);
    }
}

static void ui_refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_anim_phase = (uint8_t)((s_anim_phase + 1) % 10);

    net_status_t net;
    app_wifi_get_status(&net);

    dsl_request_t req;
    uint32_t rem_ms = 0;
    uint32_t gen = 0;
    bool waiting = false;
    bool active = app_dsl_get_active_snapshot(&req, &rem_ms, &gen, &waiting);

    if (active != s_rendered_active || (active && gen != s_rendered_gen) || s_dirty_active) {
        s_dirty_active = false;
        s_rendered_active = active;
        s_rendered_gen = gen;

        if (active) {
            set_drawer_open(false, false);
            close_settings_modal();
            close_portal_modal();
            render_active_dsl(&req);
            lv_obj_add_flag(s_idle_root, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_active_root, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_active_root, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_idle_root, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (active) {
        /* Pulse status indicator dot only when phase crosses 0 or 5 (every 500ms) */
        if (s_anim_phase == 0 || s_anim_phase == 5) {
            lv_opa_t dot_opa = (s_anim_phase < 5) ? LV_OPA_COVER : LV_OPA_40;
            lv_obj_set_style_bg_opa(s_state_dot, dot_opa, 0);
        }

        if (req.timeout_sec > 0) {
            uint32_t total_ms = (uint32_t)req.timeout_sec * 1000U;
            int32_t bar_val = (int32_t)((rem_ms * 200ULL) / total_ms);
            if (lv_bar_get_value(s_timeout_bar) != bar_val) {
                lv_bar_set_value(s_timeout_bar, bar_val, LV_ANIM_OFF);
            }
            if (rem_ms <= 5000U && rem_ms > 4800U) {
                lv_obj_set_style_bg_color(s_timeout_bar, lv_color_hex(0xEF4444), LV_PART_INDICATOR);
            }
            char cbuf[16];
            snprintf(cbuf, sizeof(cbuf), "%lus", (unsigned long)((rem_ms + 999U) / 1000U));
            set_label_if_changed(s_countdown_lbl, cbuf);
        } else {
            if (lv_bar_get_value(s_timeout_bar) != 200) {
                lv_bar_set_value(s_timeout_bar, 200, LV_ANIM_OFF);
            }
            set_label_if_changed(s_countdown_lbl, "");
        }

        char mbuf[32];
        if (net.wifi_state == NET_CONNECTED) {
            snprintf(mbuf, sizeof(mbuf), "%s", net.ip_str);
        } else {
            snprintf(mbuf, sizeof(mbuf), "离线");
        }
        set_label_if_changed(s_left_meta_lbl, mbuf);
    } else {
        /* Update Full-Screen Idle Clock & Network */
        int64_t now_ms = esp_timer_get_time() / 1000LL;
        bool dim = ((now_ms / 500LL) % 2) != 0;
        if (dim != s_idle_colon_dim) {
            s_idle_colon_dim = dim;
            if (s_idle_colon_lbl) {
                lv_obj_set_style_text_opa(s_idle_colon_lbl, dim ? 60 : 255, 0);
            }
        }

        time_t now = 0;
        time(&now);
        struct tm tm_info;
        localtime_r(&now, &tm_info);
        if (tm_info.tm_sec != s_idle_last_sec) {
            s_idle_last_sec = tm_info.tm_sec;

            char hbuf[8], mbuf[8], sbuf[8];
            snprintf(hbuf, sizeof(hbuf), "%02d", tm_info.tm_hour);
            snprintf(mbuf, sizeof(mbuf), "%02d", tm_info.tm_min);
            snprintf(sbuf, sizeof(sbuf), "%02d", tm_info.tm_sec);
            set_label_if_changed(s_idle_hh_lbl, hbuf);
            set_label_if_changed(s_idle_mm_lbl, mbuf);
            set_label_if_changed(s_idle_ss_lbl, sbuf);
            if (s_idle_bar_sec) {
                lv_bar_set_value(s_idle_bar_sec, tm_info.tm_sec, LV_ANIM_OFF);
            }

            if (tm_info.tm_year + 1900 >= 2024 || s_rtc_valid) {
                static const char *wk[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
                char dbuf[48];
                snprintf(dbuf, sizeof(dbuf), "%04d-%02d-%02d  %s",
                         tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                         wk[tm_info.tm_wday % 7]);
                set_label_if_changed(s_idle_date_lbl, dbuf);
            }
        }

        if (s_drawer_open) {
            if ((now_ms - s_drawer_open_ms) >= DRAWER_AUTO_CLOSE_MS) {
                set_drawer_open(false, true);
            }
        }

        if (s_settings_modal && s_anim_phase == 0) {
            refresh_settings_modal();
        }

        bool alarm_toast = (now_ms < s_alarm_toast_until_ms);
        if (alarm_toast) {
            if (strcmp(lv_label_get_text(s_idle_net_badge), "闹钟时间到") != 0) {
                lv_label_set_text(s_idle_net_badge, "闹钟时间到");
                lv_obj_set_style_text_color(s_idle_net_badge, lv_color_hex(0xFBBF24), 0);
                if (s_idle_net_dot) {
                    lv_obj_set_style_bg_color(s_idle_net_dot, lv_color_hex(0xFBBF24), 0);
                }
                if (s_idle_net_pill) {
                    lv_obj_set_style_border_color(s_idle_net_pill, lv_color_hex(0xF59E0B), 0);
                }
            }
        } else if (net.wifi_state == NET_CONNECTED) {
            if (strcmp(lv_label_get_text(s_idle_net_badge), "AI 待命") != 0) {
                lv_label_set_text(s_idle_net_badge, "AI 待命");
                lv_obj_set_style_text_color(s_idle_net_badge, lv_color_hex(0x4ADE80), 0);
                if (s_idle_net_dot) {
                    lv_obj_set_style_bg_color(s_idle_net_dot, lv_color_hex(0x4ADE80), 0);
                }
                if (s_idle_net_pill) {
                    lv_obj_set_style_border_color(s_idle_net_pill, lv_color_hex(0x22C55E), 0);
                }
                if (s_drawer_wifi_dot) {
                    lv_obj_set_style_bg_color(s_drawer_wifi_dot, lv_color_hex(0x4ADE80), 0);
                }
            }
            char wbuf[96];
            if (net.ssid[0] && net.ip_str[0]) {
                snprintf(wbuf, sizeof(wbuf), "%s (%s)", net.ssid, net.ip_str);
            } else if (net.ip_str[0]) {
                snprintf(wbuf, sizeof(wbuf), "%s", net.ip_str);
            } else {
                snprintf(wbuf, sizeof(wbuf), "已连接 Wi-Fi");
            }
            set_label_if_changed(s_drawer_wifi_lbl, wbuf);
        } else if (net.wifi_state == NET_PORTAL_AP) {
            if (strcmp(lv_label_get_text(s_idle_net_badge), "配网热点") != 0) {
                lv_label_set_text(s_idle_net_badge, "配网热点");
                lv_obj_set_style_text_color(s_idle_net_badge, lv_color_hex(0xFBBF24), 0);
                if (s_idle_net_dot) {
                    lv_obj_set_style_bg_color(s_idle_net_dot, lv_color_hex(0xFBBF24), 0);
                }
                if (s_idle_net_pill) {
                    lv_obj_set_style_border_color(s_idle_net_pill, lv_color_hex(0xF59E0B), 0);
                }
                if (s_drawer_wifi_dot) {
                    lv_obj_set_style_bg_color(s_drawer_wifi_dot, lv_color_hex(0xFBBF24), 0);
                }
            }
            set_label_if_changed(s_drawer_wifi_lbl, "配网热点已开");
        } else {
            if (strcmp(lv_label_get_text(s_idle_net_badge), "等待联网") != 0) {
                lv_label_set_text(s_idle_net_badge, "等待联网");
                lv_obj_set_style_text_color(s_idle_net_badge, lv_color_hex(0x38BDF8), 0);
                if (s_idle_net_dot) {
                    lv_obj_set_style_bg_color(s_idle_net_dot, lv_color_hex(0x38BDF8), 0);
                }
                if (s_idle_net_pill) {
                    lv_obj_set_style_border_color(s_idle_net_pill, COL_BORDER, 0);
                }
                if (s_drawer_wifi_dot) {
                    lv_obj_set_style_bg_color(s_drawer_wifi_dot, lv_color_hex(0x38BDF8), 0);
                }
            }
            set_label_if_changed(s_drawer_wifi_lbl, "未连接 Wi-Fi");
        }

        if (!s_ui_bat_inited || s_bat_pct != s_ui_last_bat_pct || s_usb_pwr != s_ui_last_usb_pwr) {
            s_ui_bat_inited = true;
            s_ui_last_bat_pct = s_bat_pct;
            s_ui_last_usb_pwr = s_usb_pwr;

            lv_color_t bat_col;
            if (s_usb_pwr) {
                bat_col = lv_color_hex(0x38BDF8); /* Cyan when USB powered / charging */
            } else if (s_bat_pct <= 20) {
                bat_col = lv_color_hex(0xF87171); /* Red low battery alert */
            } else if (s_bat_pct <= 50) {
                bat_col = lv_color_hex(0xFBBF24); /* Amber mid battery */
            } else {
                bat_col = lv_color_hex(0x4ADE80); /* Green healthy battery */
            }

            if (s_idle_bat_fill) {
                lv_coord_t fw = (lv_coord_t)(((int)s_bat_pct * 13 + 50) / 100);
                if (fw < 2 && s_bat_pct > 0) fw = 2;
                if (fw > 13) fw = 13;
                lv_obj_set_width(s_idle_bat_fill, fw);
                lv_obj_set_style_bg_color(s_idle_bat_fill, bat_col, 0);
            }
            if (s_idle_bat_lbl) {
                char ibbuf[24];
                if (s_usb_pwr) {
                    snprintf(ibbuf, sizeof(ibbuf), "⚡%u%%", s_bat_pct);
                } else {
                    snprintf(ibbuf, sizeof(ibbuf), "%u%%", s_bat_pct);
                }
                set_label_if_changed(s_idle_bat_lbl, ibbuf);
                lv_obj_set_style_text_color(s_idle_bat_lbl, bat_col, 0);
            }
            if (s_idle_bat_pill) {
                lv_obj_set_style_border_color(s_idle_bat_pill,
                                              (!s_usb_pwr && s_bat_pct <= 20) ? lv_color_hex(0xEF4444) : COL_BORDER, 0);
            }
        }

        if (s_drawer_bat_lbl) {
            char dbbuf[40];
            if (s_usb_pwr) {
                snprintf(dbbuf, sizeof(dbbuf), "⚡ %u%% · %.2fV", s_bat_pct, (double)s_vbat);
                lv_obj_set_style_text_color(s_drawer_bat_lbl, lv_color_hex(0x38BDF8), 0);
            } else {
                snprintf(dbbuf, sizeof(dbbuf), "%u%% · %.2fV", s_bat_pct, (double)s_vbat);
                lv_color_t dc = (s_bat_pct <= 20) ? lv_color_hex(0xF87171)
                              : (s_bat_pct <= 50) ? lv_color_hex(0xFBBF24)
                                                  : COL_TEXT_MUTED;
                lv_obj_set_style_text_color(s_drawer_bat_lbl, dc, 0);
            }
            set_label_if_changed(s_drawer_bat_lbl, dbbuf);
        }
    }
}

void ui_terminal_init(bool rtc_valid)
{
    s_rtc_valid = rtc_valid;
    s_scr = lv_scr_act();
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(s_scr, on_screen_gesture, LV_EVENT_GESTURE, NULL);

    build_idle_ui();
    build_active_skeleton();

    s_refresh_timer = lv_timer_create(ui_refresh_timer_cb, 100, NULL);
    ESP_LOGI(TAG, "AI Ask-Me Terminal UI initialized (640x172)");
}

void ui_terminal_set_sleeping(bool sleeping)
{
    if (!s_refresh_timer) return;
    if (sleeping) {
        lv_timer_pause(s_refresh_timer);
    } else {
        lv_timer_resume(s_refresh_timer);
        lv_timer_ready(s_refresh_timer);
    }
}

void ui_terminal_request_refresh(void)
{
    s_dirty_active = true;
    bsp_display_pm_boost(1200);
    bsp_display_wake_lvgl_task();
}

void ui_terminal_notify_button(bsp_btn_event_t evt)
{
    if (!bsp_display_lock(200)) return;
    if (evt == BTN_EVT_BOOT_SHORT) {
        dsl_request_t req;
        if (app_dsl_get_active_snapshot(&req, NULL, NULL, NULL)) {
            app_dsl_submit_default_or_first("button");
            s_dirty_active = true;
        } else {
            bsp_audio_play_preset(AUDIO_PRESET_INFO, 1);
        }
    } else if (evt == BTN_EVT_BOOT_LONG) {
        if (s_portal_modal) {
            close_portal_modal();
            trigger_portal_async(2);
        } else {
            open_portal_modal(true);
        }
    } else if (evt == BTN_EVT_PWR_SHORT) {
        bsp_display_sleep(!bsp_display_is_sleeping());
    }
    bsp_display_unlock();
    bsp_display_wake_lvgl_task();
}

void ui_terminal_notify_imu_rot(disp_rot_t rot)
{
    if (!bsp_display_lock(200)) return;
    bsp_display_set_rotation(rot);
    bsp_display_unlock();
}

void ui_terminal_notify_battery(float vbat, uint8_t pct, bool usb_powered)
{
    s_vbat = vbat;
    s_bat_pct = pct;
    s_usb_pwr = usb_powered;
}

void ui_terminal_notify_alarm_fired(void)
{
    s_alarm_toast_until_ms = (esp_timer_get_time() / 1000LL) + 10000LL;
    bsp_audio_play_melody("C5:80,E5:80,G5:80,C6:160", 2);
}

bool ui_terminal_take_shutdown_request(void)
{
    if (s_shutdown_requested) {
        s_shutdown_requested = false;
        return true;
    }
    return false;
}

/* ---------- Boot & Shutdown Animations ---------- */

static void anim_set_opa_cb(void *var, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)var, (lv_opa_t)v, 0);
}

static void anim_set_width_cb(void *var, int32_t v)
{
    lv_obj_set_width((lv_obj_t *)var, (lv_coord_t)v);
}

static void anim_set_height_cb(void *var, int32_t v)
{
    lv_obj_set_height((lv_obj_t *)var, (lv_coord_t)v);
}

static void anim_set_y_cb(void *var, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)var, (lv_coord_t)v);
}

static void anim_set_bar_cb(void *var, int32_t v)
{
    lv_bar_set_value((lv_obj_t *)var, v, LV_ANIM_OFF);
}

static void boot_overlay_done_cb(lv_anim_t *a)
{
    lv_obj_t *ov = (lv_obj_t *)a->var;
    if (ov == s_boot_overlay) {
        s_boot_overlay = NULL;
    }
    if (ov && lv_obj_is_valid(ov)) {
        lv_obj_del_async(ov);
    }
}

void ui_terminal_start_boot_anim(void)
{
    if (s_boot_overlay && lv_obj_is_valid(s_boot_overlay)) {
        lv_obj_del(s_boot_overlay);
        s_boot_overlay = NULL;
    }

    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    s_boot_overlay = ov;
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(ov, COL_BG, 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);

    /* Top & bottom cyan accent rails expanding from center */
    lv_obj_t *top_rail = lv_obj_create(ov);
    lv_obj_remove_style_all(top_rail);
    lv_obj_set_size(top_rail, 0, 2);
    lv_obj_set_style_bg_color(top_rail, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_bg_opa(top_rail, LV_OPA_80, 0);
    lv_obj_set_style_radius(top_rail, 1, 0);
    lv_obj_align(top_rail, LV_ALIGN_TOP_MID, 0, 14);

    lv_obj_t *bot_rail = lv_obj_create(ov);
    lv_obj_remove_style_all(bot_rail);
    lv_obj_set_size(bot_rail, 0, 2);
    lv_obj_set_style_bg_color(bot_rail, lv_color_hex(0x818CF8), 0);
    lv_obj_set_style_bg_opa(bot_rail, LV_OPA_70, 0);
    lv_obj_set_style_radius(bot_rail, 1, 0);
    lv_obj_align(bot_rail, LV_ALIGN_BOTTOM_MID, 0, -14);

    /* Brand title row (glowing orb + AI ASK TERMINAL) */
    lv_obj_t *title_row = lv_obj_create(ov);
    lv_obj_remove_style_all(title_row);
    lv_obj_set_size(title_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(title_row, 12, 0);
    lv_obj_align(title_row, LV_ALIGN_CENTER, 0, -8);
    lv_obj_set_style_opa(title_row, LV_OPA_TRANSP, 0);

    lv_obj_t *orb_ring = lv_obj_create(title_row);
    lv_obj_remove_style_all(orb_ring);
    lv_obj_set_size(orb_ring, 24, 24);
    lv_obj_set_style_radius(orb_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(orb_ring, lv_color_hex(0x0C4A6E), 0);
    lv_obj_set_style_bg_opa(orb_ring, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(orb_ring, lv_color_hex(0x38BDF8), 0);
    lv_obj_set_style_border_width(orb_ring, 2, 0);

    lv_obj_t *orb_core = lv_obj_create(orb_ring);
    lv_obj_remove_style_all(orb_core);
    lv_obj_set_size(orb_core, 10, 10);
    lv_obj_set_style_radius(orb_core, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(orb_core, lv_color_hex(0x7DD3FC), 0);
    lv_obj_set_style_bg_opa(orb_core, LV_OPA_COVER, 0);
    lv_obj_center(orb_core);

    lv_obj_t *title_lbl = lv_label_create(title_row);
    lv_label_set_text(title_lbl, "AI ASK TERMINAL");
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title_lbl, COL_TEXT_MAIN, 0);
    lv_obj_set_style_text_letter_space(title_lbl, 2, 0);

    /* Chinese subtitle */
    lv_obj_t *sub_lbl = lv_label_create(ov);
    lv_label_set_text(sub_lbl, "智能桌面交互终端 · 启动中");
    lv_obj_set_style_text_font(sub_lbl, &font_cn_16, 0);
    lv_obj_set_style_text_color(sub_lbl, COL_TEXT_MUTED, 0);
    lv_obj_align(sub_lbl, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_opa(sub_lbl, LV_OPA_TRANSP, 0);

    /* Sleek progress bar */
    lv_obj_t *bar = lv_bar_create(ov);
    lv_obj_set_size(bar, 280, 4);
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 38);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x162238), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x38BDF8), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 2, LV_PART_INDICATOR);

    lv_anim_t a;

    /* 1. Rails expand 0 -> 460px */
    lv_anim_init(&a);
    lv_anim_set_var(&a, top_rail);
    lv_anim_set_exec_cb(&a, anim_set_width_cb);
    lv_anim_set_values(&a, 0, 460);
    lv_anim_set_time(&a, 420);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_anim_set_var(&a, bot_rail);
    lv_anim_set_delay(&a, 60);
    lv_anim_start(&a);

    /* 2. Title row slide up + fade in */
    lv_anim_init(&a);
    lv_anim_set_var(&a, title_row);
    lv_anim_set_exec_cb(&a, anim_set_y_cb);
    lv_anim_set_values(&a, -6, -22);
    lv_anim_set_delay(&a, 100);
    lv_anim_set_time(&a, 420);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, title_row);
    lv_anim_set_exec_cb(&a, anim_set_opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_delay(&a, 100);
    lv_anim_set_time(&a, 380);
    lv_anim_start(&a);

    /* 3. Subtitle fade in */
    lv_anim_init(&a);
    lv_anim_set_var(&a, sub_lbl);
    lv_anim_set_exec_cb(&a, anim_set_opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_delay(&a, 240);
    lv_anim_set_time(&a, 340);
    lv_anim_start(&a);

    /* 4. Progress bar sweep 0 -> 100 */
    lv_anim_init(&a);
    lv_anim_set_var(&a, bar);
    lv_anim_set_exec_cb(&a, anim_set_bar_cb);
    lv_anim_set_values(&a, 0, 100);
    lv_anim_set_delay(&a, 140);
    lv_anim_set_time(&a, 760);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);

    /* 5. Whole overlay fade out & auto-delete */
    lv_anim_init(&a);
    lv_anim_set_var(&a, ov);
    lv_anim_set_exec_cb(&a, anim_set_opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_delay(&a, 980);
    lv_anim_set_time(&a, 280);
    lv_anim_set_ready_cb(&a, boot_overlay_done_cb);
    lv_anim_start(&a);
}

void ui_terminal_play_shutdown_anim(void)
{
    if (bsp_display_is_sleeping()) {
        return;
    }

    /* Soft descending chime */
    bsp_audio_play_melody("G5:65,E5:65,C5:130", 1);

    if (bsp_display_lock(300)) {
        if (s_boot_overlay && lv_obj_is_valid(s_boot_overlay)) {
            lv_obj_del(s_boot_overlay);
            s_boot_overlay = NULL;
        }
        if (s_shutdown_overlay && lv_obj_is_valid(s_shutdown_overlay)) {
            lv_obj_del(s_shutdown_overlay);
            s_shutdown_overlay = NULL;
        }

        lv_obj_t *ov = lv_obj_create(lv_layer_top());
        s_shutdown_overlay = ov;
        lv_obj_remove_style_all(ov);
        lv_obj_set_size(ov, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_color(ov, COL_BG, 0);
        lv_obj_set_style_bg_opa(ov, LV_OPA_COVER, 0);
        lv_obj_set_style_opa(ov, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *text_box = lv_obj_create(ov);
        lv_obj_remove_style_all(text_box);
        lv_obj_set_size(text_box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(text_box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(text_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(text_box, 6, 0);
        lv_obj_center(text_box);

        lv_obj_t *t_lbl = lv_label_create(text_box);
        lv_label_set_text(t_lbl, "POWERING OFF");
        lv_obj_set_style_text_font(t_lbl, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(t_lbl, lv_color_hex(0xF8FAFC), 0);
        lv_obj_set_style_text_letter_space(t_lbl, 2, 0);

        lv_obj_t *s_lbl = lv_label_create(text_box);
        lv_label_set_text(s_lbl, "正在安全关机...");
        lv_obj_set_style_text_font(s_lbl, &font_cn_16, 0);
        lv_obj_set_style_text_color(s_lbl, COL_TEXT_MUTED, 0);

        /* CRT-style horizontal collapse beam */
        lv_obj_t *beam = lv_obj_create(ov);
        lv_obj_remove_style_all(beam);
        lv_obj_set_size(beam, 520, 4);
        lv_obj_set_style_radius(beam, 2, 0);
        lv_obj_set_style_bg_color(beam, lv_color_hex(0x7DD3FC), 0);
        lv_obj_set_style_bg_opa(beam, LV_OPA_COVER, 0);
        lv_obj_set_style_opa(beam, LV_OPA_TRANSP, 0);
        lv_obj_center(beam);

        lv_anim_t a;

        /* Overlay fade in */
        lv_anim_init(&a);
        lv_anim_set_var(&a, ov);
        lv_anim_set_exec_cb(&a, anim_set_opa_cb);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_time(&a, 150);
        lv_anim_start(&a);

        /* Text fade out before beam collapse */
        lv_anim_init(&a);
        lv_anim_set_var(&a, text_box);
        lv_anim_set_exec_cb(&a, anim_set_opa_cb);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_delay(&a, 340);
        lv_anim_set_time(&a, 160);
        lv_anim_start(&a);

        /* Beam appear -> collapse horizontally -> extinguish */
        lv_anim_init(&a);
        lv_anim_set_var(&a, beam);
        lv_anim_set_exec_cb(&a, anim_set_opa_cb);
        lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_delay(&a, 380);
        lv_anim_set_time(&a, 80);
        lv_anim_start(&a);

        lv_anim_init(&a);
        lv_anim_set_var(&a, beam);
        lv_anim_set_exec_cb(&a, anim_set_width_cb);
        lv_anim_set_values(&a, 520, 8);
        lv_anim_set_delay(&a, 430);
        lv_anim_set_time(&a, 300);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);

        lv_anim_init(&a);
        lv_anim_set_var(&a, beam);
        lv_anim_set_exec_cb(&a, anim_set_height_cb);
        lv_anim_set_values(&a, 4, 2);
        lv_anim_set_delay(&a, 430);
        lv_anim_set_time(&a, 300);
        lv_anim_start(&a);

        lv_anim_init(&a);
        lv_anim_set_var(&a, beam);
        lv_anim_set_exec_cb(&a, anim_set_opa_cb);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_delay(&a, 690);
        lv_anim_set_time(&a, 120);
        lv_anim_start(&a);

        bsp_display_unlock();
    }

    /* Let LVGL render the collapse sequence smoothly, then fade backlight out */
    vTaskDelay(pdMS_TO_TICKS(780));
    bsp_display_fade_out(160);
    bsp_display_sleep(true);

    /* Clean up overlay in case USB power keeps MCU alive and user wakes later */
    if (bsp_display_lock(200)) {
        if (s_shutdown_overlay && lv_obj_is_valid(s_shutdown_overlay)) {
            lv_obj_del(s_shutdown_overlay);
            s_shutdown_overlay = NULL;
        }
        bsp_display_unlock();
    }
}
