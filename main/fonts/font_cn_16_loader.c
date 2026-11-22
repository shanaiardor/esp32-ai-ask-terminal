#include "terminal_fonts.h"
#include "esp_partition.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "font_cn_loader";

#define FONT_BIN_MAGIC    0x464E5431U /* "FNT1" */
#define FONT_PART_LABEL   "font_dat"
#define FONT_PART_SUBTYPE 0x40
#define MAX_BIN_CMAPS     32

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t line_height;
    uint16_t base_line;
    uint8_t  bpp;
    uint8_t  cmap_num;
    uint16_t reserved;
    uint32_t glyph_count;
    uint32_t glyph_dsc_ofs;
    uint32_t cmaps_ofs;
    uint32_t bitmap_ofs;
    uint32_t total_size;
} bin_font_hdr_t;

typedef struct __attribute__((packed)) {
    uint32_t range_start;
    uint16_t range_length;
    uint16_t glyph_id_start;
    uint16_t list_length;
    uint16_t type_id;
    uint32_t unicode_list_ofs;
} bin_cmap_t;

extern const lv_font_t lv_font_montserrat_16;

static esp_partition_mmap_handle_t s_mmap_handle = 0;
static lv_font_fmt_txt_cmap_t s_cmaps[MAX_BIN_CMAPS];
static lv_font_fmt_txt_glyph_cache_t s_cache;
static lv_font_fmt_txt_dsc_t s_font_dsc;

lv_font_t font_cn_16 = {
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = 22,
    .base_line = 5,
    .subpx = LV_FONT_SUBPX_NONE,
    .underline_position = -2,
    .underline_thickness = 1,
    .dsc = &s_font_dsc,
    .fallback = &lv_font_montserrat_16,
};

esp_err_t bsp_font_cn_init(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, FONT_PART_SUBTYPE, FONT_PART_LABEL);
    if (!part) {
        ESP_LOGE(TAG, "Partition '%s' not found; falling back to ASCII font", FONT_PART_LABEL);
        font_cn_16 = lv_font_montserrat_16;
        return ESP_ERR_NOT_FOUND;
    }

    const void *mapped_ptr = NULL;
    esp_err_t err = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &mapped_ptr, &s_mmap_handle);
    if (err != ESP_OK || !mapped_ptr) {
        ESP_LOGE(TAG, "esp_partition_mmap('%s') failed: %s", FONT_PART_LABEL, esp_err_to_name(err));
        font_cn_16 = lv_font_montserrat_16;
        return err;
    }

    const uint8_t *base = (const uint8_t *)mapped_ptr;
    const bin_font_hdr_t *hdr = (const bin_font_hdr_t *)base;
    if (hdr->magic != FONT_BIN_MAGIC || hdr->cmap_num == 0 || hdr->cmap_num > MAX_BIN_CMAPS ||
        hdr->total_size > part->size) {
        ESP_LOGE(TAG, "Invalid binary font in '%s' (magic=0x%08lX, size=%lu)",
                 FONT_PART_LABEL, (unsigned long)hdr->magic, (unsigned long)hdr->total_size);
        font_cn_16 = lv_font_montserrat_16;
        return ESP_ERR_INVALID_STATE;
    }

    const bin_cmap_t *bc = (const bin_cmap_t *)(base + hdr->cmaps_ofs);
    memset(s_cmaps, 0, sizeof(s_cmaps));
    for (uint8_t i = 0; i < hdr->cmap_num; i++) {
        s_cmaps[i].range_start = bc[i].range_start;
        s_cmaps[i].range_length = bc[i].range_length;
        s_cmaps[i].glyph_id_start = bc[i].glyph_id_start;
        s_cmaps[i].unicode_list = (bc[i].unicode_list_ofs != 0)
                                      ? (const uint16_t *)(base + bc[i].unicode_list_ofs)
                                      : NULL;
        s_cmaps[i].glyph_id_ofs_list = NULL;
        s_cmaps[i].list_length = bc[i].list_length;
        s_cmaps[i].type = (lv_font_fmt_txt_cmap_type_t)bc[i].type_id;
    }

    memset(&s_cache, 0, sizeof(s_cache));
    memset(&s_font_dsc, 0, sizeof(s_font_dsc));
    s_font_dsc.glyph_bitmap = base + hdr->bitmap_ofs;
    s_font_dsc.glyph_dsc = (const lv_font_fmt_txt_glyph_dsc_t *)(base + hdr->glyph_dsc_ofs);
    s_font_dsc.cmaps = s_cmaps;
    s_font_dsc.kern_dsc = NULL;
    s_font_dsc.kern_scale = 0;
    s_font_dsc.cmap_num = hdr->cmap_num;
    s_font_dsc.bpp = hdr->bpp;
    s_font_dsc.kern_classes = 0;
    s_font_dsc.bitmap_format = LV_FONT_FMT_TXT_PLAIN;
    s_font_dsc.cache = &s_cache;

    font_cn_16.line_height = (lv_coord_t)hdr->line_height;
    font_cn_16.base_line = (lv_coord_t)hdr->base_line;

    ESP_LOGI(TAG, "Mapped '%s' (%lu glyphs, %ubpp, %u cmaps, %.2f MB) zero-copy OK",
             FONT_PART_LABEL, (unsigned long)hdr->glyph_count, hdr->bpp, hdr->cmap_num,
             (double)hdr->total_size / (1024.0 * 1024.0));
    return ESP_OK;
}
