#ifndef BSP_AUDIO_H
#define BSP_AUDIO_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUDIO_PRESET_NONE = 0,
    AUDIO_PRESET_INFO,
    AUDIO_PRESET_ASK,
    AUDIO_PRESET_SUCCESS,
    AUDIO_PRESET_WARNING,
    AUDIO_PRESET_ERROR,
    AUDIO_PRESET_URGENT,
    AUDIO_PRESET_CLICK,
} audio_preset_t;

esp_err_t bsp_audio_init(uint8_t initial_vol, bool initial_mute);

void bsp_audio_set_volume(uint8_t vol);
uint8_t bsp_audio_get_volume(void);

void bsp_audio_set_mute(bool mute);
bool bsp_audio_is_muted(void);

esp_err_t bsp_audio_play_preset(audio_preset_t preset, int repeat);
esp_err_t bsp_audio_play_melody(const char *melody_str, int repeat);
void bsp_audio_stop(void);

audio_preset_t bsp_audio_preset_from_str(const char *str);
const char *bsp_audio_preset_to_str(audio_preset_t preset);

#ifdef __cplusplus
}
#endif

#endif /* BSP_AUDIO_H */
