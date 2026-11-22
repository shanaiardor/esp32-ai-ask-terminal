#include "bsp_audio.h"
#include "bsp_i2c.h"
#include "board_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_check.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "bsp_audio";

/* ES8311 Register Addresses */
#define ES8311_RESET_REG00       0x00
#define ES8311_CLK_MANAGER_REG01 0x01
#define ES8311_CLK_MANAGER_REG02 0x02
#define ES8311_CLK_MANAGER_REG03 0x03
#define ES8311_CLK_MANAGER_REG04 0x04
#define ES8311_CLK_MANAGER_REG05 0x05
#define ES8311_CLK_MANAGER_REG06 0x06
#define ES8311_CLK_MANAGER_REG07 0x07
#define ES8311_CLK_MANAGER_REG08 0x08
#define ES8311_SDPIN_REG09       0x09
#define ES8311_SDPOUT_REG0A      0x0A
#define ES8311_SYSTEM_REG0B      0x0B
#define ES8311_SYSTEM_REG0C      0x0C
#define ES8311_SYSTEM_REG0D      0x0D
#define ES8311_SYSTEM_REG0E      0x0E
#define ES8311_SYSTEM_REG10      0x10
#define ES8311_SYSTEM_REG11      0x11
#define ES8311_SYSTEM_REG12      0x12
#define ES8311_SYSTEM_REG13      0x13
#define ES8311_SYSTEM_REG14      0x14
#define ES8311_ADC_REG15         0x15
#define ES8311_ADC_REG16         0x16
#define ES8311_ADC_REG17         0x17
#define ES8311_ADC_REG1B         0x1B
#define ES8311_ADC_REG1C         0x1C
#define ES8311_DAC_REG31         0x31
#define ES8311_DAC_REG32         0x32
#define ES8311_DAC_REG37         0x37
#define ES8311_GPIO_REG44        0x44
#define ES8311_GP_REG45          0x45

#define MELODY_BUF_LEN           256
#define CHUNK_FRAMES             240 /* 10 ms at 24 kHz */

typedef struct {
    char melody[MELODY_BUF_LEN];
    int repeat;
    bool is_click;
} audio_cmd_t;

static i2s_chan_handle_t s_tx_chan = NULL;
static QueueHandle_t s_audio_queue = NULL;
static bool s_audio_ready = false;
static volatile uint8_t s_volume = 75;
static volatile bool s_muted = false;
static uint8_t s_dac_reg31 = 0;
static volatile bool s_cancel_play = false;

static const char *s_preset_melodies[] = {
    [AUDIO_PRESET_NONE]    = "",
    [AUDIO_PRESET_INFO]    = "E5:85,B5:145",
    [AUDIO_PRESET_ASK]     = "G5:70,C6:130,R:25,E6:170",
    [AUDIO_PRESET_SUCCESS] = "C5:65,E5:65,G5:65,C6:180",
    [AUDIO_PRESET_WARNING] = "A5:110,R:45,A5:140",
    [AUDIO_PRESET_ERROR]   = "E5:95,C5:95,A4:220",
    [AUDIO_PRESET_URGENT]  = "C6:65,R:25,C6:65,R:25,G6:150,R:70,C6:65,R:25,G6:150",
    [AUDIO_PRESET_CLICK]   = "C6:18",
};

static esp_err_t es_write(uint8_t reg, uint8_t val)
{
    return bsp_i2c0_write_reg(I2C0_DEV_ES8311, reg, &val, 1);
}

static esp_err_t es_read(uint8_t reg, uint8_t *val)
{
    return bsp_i2c0_read_reg(I2C0_DEV_ES8311, reg, val, 1);
}

static uint8_t volume_pct_to_reg(uint8_t vol_pct)
{
    if (vol_pct == 0) return 0x00;
    if (vol_pct > 100) vol_pct = 100;
    /* Map 1..100% -> 0x78 (-35.5dB) .. 0xBF (0dB) to avoid clipping on onboard speaker */
    return (uint8_t)(0x78 + ((uint32_t)vol_pct * (0xBF - 0x78)) / 100U);
}

static esp_err_t es8311_hw_init(uint8_t initial_vol, bool initial_mute)
{
    /* Double-write REG44 per ES8311 errata for I2C noise immunity */
    ESP_RETURN_ON_ERROR(es_write(ES8311_GPIO_REG44, 0x08), TAG, "es8311 probe failed");
    ESP_RETURN_ON_ERROR(es_write(ES8311_GPIO_REG44, 0x08), TAG, "es8311 reg44 failed");

    esp_err_t err = ESP_OK;
    err |= es_write(ES8311_CLK_MANAGER_REG01, 0x30);
    err |= es_write(ES8311_CLK_MANAGER_REG02, 0x00);
    err |= es_write(ES8311_CLK_MANAGER_REG03, 0x10);
    err |= es_write(ES8311_ADC_REG16, 0x24);
    err |= es_write(ES8311_CLK_MANAGER_REG04, 0x10);
    err |= es_write(ES8311_CLK_MANAGER_REG05, 0x00);
    err |= es_write(ES8311_SYSTEM_REG0B, 0x00);
    err |= es_write(ES8311_SYSTEM_REG0C, 0x00);
    err |= es_write(ES8311_SYSTEM_REG10, 0x1F);
    err |= es_write(ES8311_SYSTEM_REG11, 0x7F);
    err |= es_write(ES8311_RESET_REG00, 0x80); /* Master clock off, slave mode, chip reset release */

    /* Use external MCLK (6.144MHz = 256 * 24kHz) */
    err |= es_write(ES8311_CLK_MANAGER_REG01, 0x3F);

    /* Configure I2S Normal 16-bit DAC enabled */
    err |= es_write(ES8311_SDPIN_REG09, 0x0C);
    err |= es_write(ES8311_SDPOUT_REG0A, 0x4C); /* ADC tri-state/disabled, 16-bit */

    /* Sample rate 24kHz with 6.144MHz MCLK: pre_div=1(0), pre_mult=1(0), adc_div=1, dac_div=1, fs=0, osr=0x10, lrck=0x00FF, bclk_div=4(3) */
    err |= es_write(ES8311_CLK_MANAGER_REG02, 0x00);
    err |= es_write(ES8311_CLK_MANAGER_REG05, 0x00);
    err |= es_write(ES8311_CLK_MANAGER_REG03, 0x10);
    err |= es_write(ES8311_CLK_MANAGER_REG04, 0x10);
    err |= es_write(ES8311_CLK_MANAGER_REG07, 0x00);
    err |= es_write(ES8311_CLK_MANAGER_REG08, 0xFF);
    err |= es_write(ES8311_CLK_MANAGER_REG06, 0x03);

    err |= es_write(ES8311_SYSTEM_REG13, 0x10);
    err |= es_write(ES8311_ADC_REG1B, 0x0A);
    err |= es_write(ES8311_ADC_REG1C, 0x6A);
    err |= es_write(ES8311_GPIO_REG44, 0x58);

    /* Start DAC power-up */
    err |= es_write(ES8311_SYSTEM_REG0E, 0x02);
    err |= es_write(ES8311_SYSTEM_REG12, 0x00);
    err |= es_write(ES8311_SYSTEM_REG14, 0x1A);
    err |= es_write(ES8311_SYSTEM_REG0D, 0x01);
    err |= es_write(ES8311_ADC_REG15, 0x40);
    err |= es_write(ES8311_DAC_REG37, 0x08);
    err |= es_write(ES8311_GP_REG45, 0x00);

    uint8_t dac_reg31 = 0;
    (void)es_read(ES8311_DAC_REG31, &dac_reg31);
    dac_reg31 &= 0x9F;
    if (initial_mute) {
        dac_reg31 |= 0x60;
    }
    s_dac_reg31 = dac_reg31;
    err |= es_write(ES8311_DAC_REG31, dac_reg31);
    err |= es_write(ES8311_DAC_REG32, volume_pct_to_reg(initial_vol));

    return err;
}

static float parse_note_freq(const char *token, int *out_ms)
{
    *out_ms = 100;
    const char *colon = strchr(token, ':');
    if (colon) {
        int ms = atoi(colon + 1);
        if (ms < 10) ms = 10;
        if (ms > 3000) ms = 3000;
        *out_ms = ms;
    }

    while (*token == ' ') token++;
    char note_c = (char)toupper((unsigned char)token[0]);
    if (note_c == 'R' || note_c == 'P' || note_c == '\0' || note_c == ':') {
        return 0.0f;
    }

    int semitone = 0;
    switch (note_c) {
    case 'C': semitone = 0; break;
    case 'D': semitone = 2; break;
    case 'E': semitone = 4; break;
    case 'F': semitone = 5; break;
    case 'G': semitone = 7; break;
    case 'A': semitone = 9; break;
    case 'B': semitone = 11; break;
    default: return 0.0f;
    }

    int idx = 1;
    if (token[idx] == '#') {
        semitone += 1;
        idx++;
    } else if (token[idx] == 'b') {
        semitone -= 1;
        idx++;
    }

    int octave = 5;
    if (isdigit((unsigned char)token[idx])) {
        octave = token[idx] - '0';
    }
    if (octave < 2) octave = 2;
    if (octave > 7) octave = 7;

    int midi = (octave + 1) * 12 + semitone;
    return 440.0f * powf(2.0f, (float)(midi - 69) / 12.0f);
}

static void write_silence_frames(int ms)
{
    int16_t zeros[CHUNK_FRAMES * 2] = {0};
    int total_frames = (AUDIO_SAMPLE_RATE * ms) / 1000;
    while (total_frames > 0 && !s_cancel_play) {
        int n = (total_frames > CHUNK_FRAMES) ? CHUNK_FRAMES : total_frames;
        size_t written = 0;
        i2s_channel_write(s_tx_chan, zeros, (size_t)n * 4, &written, pdMS_TO_TICKS(100));
        total_frames -= n;
    }
}

static void synthesize_tone(float freq_hz, int duration_ms, float gain)
{
    if (freq_hz <= 10.0f) {
        write_silence_frames(duration_ms);
        return;
    }

    int16_t buf[CHUNK_FRAMES * 2];
    int total_frames = (AUDIO_SAMPLE_RATE * duration_ms) / 1000;
    if (total_frames < 1) total_frames = 1;

    int attack_frames = (AUDIO_SAMPLE_RATE * 6) / 1000;
    int release_frames = (AUDIO_SAMPLE_RATE * 14) / 1000;
    if (attack_frames + release_frames > total_frames) {
        attack_frames = total_frames / 3;
        release_frames = total_frames / 3;
    }

    const float two_pi = 6.2831853f;
    float phase = 0.0f;
    float phase_inc = two_pi * freq_hz / (float)AUDIO_SAMPLE_RATE;

    int pos = 0;
    while (pos < total_frames && !s_cancel_play) {
        int n = total_frames - pos;
        if (n > CHUNK_FRAMES) n = CHUNK_FRAMES;

        for (int i = 0; i < n; i++) {
            int f_idx = pos + i;
            float env = 1.0f;
            if (f_idx < attack_frames && attack_frames > 0) {
                env = (float)f_idx / (float)attack_frames;
            } else if (f_idx > total_frames - release_frames && release_frames > 0) {
                env = (float)(total_frames - f_idx) / (float)release_frames;
            }
            /* Gentle exponential decay for chime timbre */
            float decay = expf(-2.6f * (float)f_idx / (float)total_frames);
            float sample = sinf(phase) + 0.25f * sinf(phase * 2.0f) + 0.10f * sinf(phase * 3.0f);
            float val = sample * env * decay * gain * 16500.0f;
            if (val > 32700.0f) val = 32700.0f;
            if (val < -32700.0f) val = -32700.0f;
            int16_t s16 = (int16_t)val;
            buf[i * 2] = s16;
            buf[i * 2 + 1] = s16;

            phase += phase_inc;
            if (phase >= two_pi) phase -= two_pi;
        }
        size_t written = 0;
        i2s_channel_write(s_tx_chan, buf, (size_t)n * 4, &written, pdMS_TO_TICKS(100));
        pos += n;
    }
}

static void play_melody_string(const char *melody_str, int repeat, bool is_click)
{
    if (!s_audio_ready || s_muted || s_volume == 0 || !melody_str || melody_str[0] == '\0') {
        return;
    }
    if (repeat < 1) repeat = 1;
    if (repeat > 5) repeat = 5;

    float gain = is_click ? 0.45f : 0.85f;
    for (int r = 0; r < repeat && !s_cancel_play; r++) {
        char copy[MELODY_BUF_LEN];
        strlcpy(copy, melody_str, sizeof(copy));
        char *saveptr = NULL;
        char *tok = strtok_r(copy, ",; ", &saveptr);
        while (tok && !s_cancel_play) {
            int dur_ms = 100;
            float freq = parse_note_freq(tok, &dur_ms);
            synthesize_tone(freq, dur_ms, gain);
            tok = strtok_r(NULL, ",; ", &saveptr);
        }
        if (r + 1 < repeat && !s_cancel_play) {
            write_silence_frames(120);
        }
    }
    write_silence_frames(15);
}

static void audio_synth_task(void *arg)
{
    (void)arg;
    audio_cmd_t cmd;
    bool amp_on = false;
    for (;;) {
        if (xQueueReceive(s_audio_queue, &cmd, amp_on ? pdMS_TO_TICKS(300) : portMAX_DELAY) == pdTRUE) {
            if (!s_muted && s_volume > 0) {
                if (!amp_on) {
                    bsp_exio_set_level(EXIO_PIN_NS_MODE, true);
                    vTaskDelay(pdMS_TO_TICKS(5));
                    amp_on = true;
                }
                s_cancel_play = false;
                play_melody_string(cmd.melody, cmd.repeat, cmd.is_click);
            }
        } else if (amp_on) {
            bsp_exio_set_level(EXIO_PIN_NS_MODE, false);
            amp_on = false;
        }
    }
}

esp_err_t bsp_audio_init(uint8_t initial_vol, bool initial_mute)
{
    s_volume = (initial_vol > 100) ? 100 : initial_vol;
    s_muted = initial_mute;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL), TAG, "i2s new channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = PIN_I2S_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_chan, &std_cfg), TAG, "i2s std init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_chan), TAG, "i2s enable failed");

    /* Keep speaker amplifier off until audio playback begins */
    bsp_exio_set_level(EXIO_PIN_NS_MODE, false);
    vTaskDelay(pdMS_TO_TICKS(10));

    esp_err_t err = es8311_hw_init(s_volume, s_muted);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ES8311 init returned %s (speaker output may be unavailable)", esp_err_to_name(err));
        return err;
    }

    s_audio_queue = xQueueCreate(6, sizeof(audio_cmd_t));
    ESP_RETURN_ON_FALSE(s_audio_queue != NULL, ESP_ERR_NO_MEM, TAG, "audio queue failed");
    s_audio_ready = true;

    xTaskCreatePinnedToCore(audio_synth_task, "audio_synth", 4096, NULL, 4, NULL, 1);
    ESP_LOGI(TAG, "ES8311 + I2S audio initialized (%dHz, vol=%u%%, mute=%d)",
             AUDIO_SAMPLE_RATE, s_volume, s_muted ? 1 : 0);
    return ESP_OK;
}

void bsp_audio_set_volume(uint8_t vol)
{
    if (vol > 100) vol = 100;
    if (s_volume == vol) return;
    s_volume = vol;
    if (s_audio_ready) {
        es_write(ES8311_DAC_REG32, volume_pct_to_reg(vol));
    }
}

uint8_t bsp_audio_get_volume(void)
{
    return s_volume;
}

void bsp_audio_set_mute(bool mute)
{
    if (s_muted == mute) return;
    s_muted = mute;
    if (s_audio_ready) {
        uint8_t regv = (uint8_t)(s_dac_reg31 & 0x9F);
        if (mute) regv |= 0x60;
        s_dac_reg31 = regv;
        es_write(ES8311_DAC_REG31, regv);
    }
}

bool bsp_audio_is_muted(void)
{
    return s_muted;
}

esp_err_t bsp_audio_play_melody(const char *melody_str, int repeat)
{
    if (!s_audio_ready || !melody_str || melody_str[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    audio_cmd_t cmd = {0};
    strlcpy(cmd.melody, melody_str, sizeof(cmd.melody));
    cmd.repeat = (repeat < 1) ? 1 : (repeat > 5 ? 5 : repeat);
    cmd.is_click = false;

    s_cancel_play = true;
    xQueueReset(s_audio_queue);
    if (xQueueSend(s_audio_queue, &cmd, 0) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t bsp_audio_play_preset(audio_preset_t preset, int repeat)
{
    if (preset <= AUDIO_PRESET_NONE || preset > AUDIO_PRESET_CLICK) {
        return ESP_OK;
    }
    if (!s_audio_ready) return ESP_ERR_INVALID_STATE;

    audio_cmd_t cmd = {0};
    strlcpy(cmd.melody, s_preset_melodies[preset], sizeof(cmd.melody));
    cmd.repeat = (repeat < 1) ? 1 : (repeat > 5 ? 5 : repeat);
    cmd.is_click = (preset == AUDIO_PRESET_CLICK);

    if (!cmd.is_click) {
        s_cancel_play = true;
        xQueueReset(s_audio_queue);
    }
    xQueueSend(s_audio_queue, &cmd, 0);
    return ESP_OK;
}

void bsp_audio_stop(void)
{
    s_cancel_play = true;
    if (s_audio_queue) {
        xQueueReset(s_audio_queue);
    }
}

audio_preset_t bsp_audio_preset_from_str(const char *str)
{
    if (!str || str[0] == '\0') return AUDIO_PRESET_NONE;
    if (strcasecmp(str, "info") == 0)    return AUDIO_PRESET_INFO;
    if (strcasecmp(str, "ask") == 0)     return AUDIO_PRESET_ASK;
    if (strcasecmp(str, "success") == 0) return AUDIO_PRESET_SUCCESS;
    if (strcasecmp(str, "warning") == 0) return AUDIO_PRESET_WARNING;
    if (strcasecmp(str, "error") == 0)   return AUDIO_PRESET_ERROR;
    if (strcasecmp(str, "urgent") == 0)  return AUDIO_PRESET_URGENT;
    if (strcasecmp(str, "click") == 0)   return AUDIO_PRESET_CLICK;
    return AUDIO_PRESET_NONE;
}

const char *bsp_audio_preset_to_str(audio_preset_t preset)
{
    switch (preset) {
    case AUDIO_PRESET_INFO:    return "info";
    case AUDIO_PRESET_ASK:     return "ask";
    case AUDIO_PRESET_SUCCESS: return "success";
    case AUDIO_PRESET_WARNING: return "warning";
    case AUDIO_PRESET_ERROR:   return "error";
    case AUDIO_PRESET_URGENT:  return "urgent";
    case AUDIO_PRESET_CLICK:   return "click";
    default:                   return "none";
    }
}
