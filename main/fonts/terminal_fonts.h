#ifndef TERMINAL_FONTS_H
#define TERMINAL_FONTS_H

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

LV_FONT_DECLARE(font_clock_96);
LV_FONT_DECLARE(font_clock_64);
LV_FONT_DECLARE(font_clock_32);
extern lv_font_t font_cn_16;

esp_err_t bsp_font_cn_init(void);

#ifdef __cplusplus
}
#endif

#endif /* TERMINAL_FONTS_H */
