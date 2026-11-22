#ifndef DSL_TYPES_H
#define DSL_TYPES_H

#include <stdbool.h>
#include <stdint.h>
#include "bsp_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DSL_MAX_BUTTONS     4
#define DSL_MAX_KV_ITEMS    4
#define DSL_MAX_STEPS       4
#define DSL_MAX_CHOICES     6

#define DSL_STR_ID_LEN      32
#define DSL_STR_AGENT_LEN   24
#define DSL_STR_TITLE_LEN   128
#define DSL_STR_SUB_LEN     128
#define DSL_STR_BODY_LEN    512
#define DSL_STR_CALLOUT_LEN 192
#define DSL_STR_KEY_LEN     40
#define DSL_STR_VAL_LEN     160
#define DSL_STR_LABEL_LEN   48
#define DSL_STR_UNIT_LEN    16
#define DSL_STR_MELODY_LEN  256

typedef enum {
    AI_STATE_IDLE = 0,
    AI_STATE_THINKING,
    AI_STATE_ASKING,
    AI_STATE_RUNNING,
    AI_STATE_SUCCESS,
    AI_STATE_WARNING,
    AI_STATE_ERROR,
} ai_state_t;

typedef enum {
    DSL_LAYOUT_TEXT = 0,
    DSL_LAYOUT_KV,
    DSL_LAYOUT_PROGRESS,
    DSL_LAYOUT_CHOICE,
    DSL_LAYOUT_NUMBER,
} dsl_layout_t;

typedef enum {
    DSL_BTN_PRIMARY = 0,
    DSL_BTN_SUCCESS,
    DSL_BTN_DANGER,
    DSL_BTN_WARNING,
    DSL_BTN_SECONDARY,
} dsl_btn_style_t;

typedef enum {
    DSL_RES_NONE = 0,
    DSL_RES_PENDING,
    DSL_RES_ANSWERED,
    DSL_RES_TIMEOUT_DEFAULT,
    DSL_RES_TIMEOUT,
    DSL_RES_DISMISSED,
    DSL_RES_SUPERSEDED,
} dsl_res_status_t;

typedef struct {
    char id[DSL_STR_ID_LEN];
    char label[DSL_STR_LABEL_LEN];
    dsl_btn_style_t style;
    bool is_default;
} dsl_button_t;

typedef struct {
    char key[DSL_STR_KEY_LEN];
    char val[DSL_STR_VAL_LEN];
    bool highlight;
} dsl_kv_item_t;

typedef struct {
    char label[DSL_STR_LABEL_LEN];
    bool done;
    bool active;
} dsl_step_item_t;

typedef struct {
    char id[DSL_STR_ID_LEN];
    char label[DSL_STR_LABEL_LEN];
    bool selected;
} dsl_choice_item_t;

typedef struct {
    char req_id[DSL_STR_ID_LEN];
    bool is_ask;
    bool wait;
    ai_state_t state;
    char agent[DSL_STR_AGENT_LEN];
    char title[DSL_STR_TITLE_LEN];
    char subtitle[DSL_STR_SUB_LEN];
    dsl_layout_t layout;

    /* Layout: text */
    char body[DSL_STR_BODY_LEN];
    char callout[DSL_STR_CALLOUT_LEN];

    /* Layout: kv */
    dsl_kv_item_t kv_items[DSL_MAX_KV_ITEMS];
    uint8_t kv_count;

    /* Layout: progress */
    uint8_t progress_percent;
    char step_text[DSL_STR_SUB_LEN];
    dsl_step_item_t steps[DSL_MAX_STEPS];
    uint8_t step_count;

    /* Layout: choice */
    dsl_choice_item_t choices[DSL_MAX_CHOICES];
    uint8_t choice_count;
    bool choice_multi;

    /* Layout: number */
    int32_t num_value;
    int32_t num_min;
    int32_t num_max;
    int32_t num_step;
    char num_unit[DSL_STR_UNIT_LEN];

    /* Right action dock (0..4 buttons) */
    dsl_button_t buttons[DSL_MAX_BUTTONS];
    uint8_t button_count;
    char default_button_id[DSL_STR_ID_LEN];

    /* Timing & Sound */
    uint16_t timeout_sec;
    audio_preset_t sound_preset;
    char sound_melody[DSL_STR_MELODY_LEN];
    uint8_t sound_repeat;
} dsl_request_t;

typedef struct {
    char req_id[DSL_STR_ID_LEN];
    dsl_res_status_t status;
    char action_id[DSL_STR_ID_LEN];
    char action_label[DSL_STR_LABEL_LEN];
    char selected_choices[DSL_MAX_CHOICES][DSL_STR_ID_LEN];
    uint8_t selected_count;
    int32_t number_value;
    bool has_number_value;
    uint32_t elapsed_ms;
    char source[16]; /* "touch", "button", "timeout", "api" */
} dsl_response_t;

#ifdef __cplusplus
}
#endif

#endif /* DSL_TYPES_H */
