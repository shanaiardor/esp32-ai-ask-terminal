#include "app_dsl.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "app_dsl";

static SemaphoreHandle_t s_mux = NULL;
static SemaphoreHandle_t s_reply_sem = NULL;
static esp_timer_handle_t s_tick_timer = NULL;

static bool s_has_active = false;
static bool s_waiting_reply = false;
static dsl_request_t s_active_req;
static dsl_response_t s_active_resp;
static dsl_response_t s_last_resp;
static char s_last_title[DSL_STR_TITLE_LEN] = "";
static int64_t s_start_ms = 0;
static uint32_t s_session_gen = 0;
static uint32_t s_req_seq = 100;

static ai_state_t parse_state_str(const char *s, bool is_ask)
{
    if (!s || !s[0]) return is_ask ? AI_STATE_ASKING : AI_STATE_RUNNING;
    if (strcasecmp(s, "idle") == 0)     return AI_STATE_IDLE;
    if (strcasecmp(s, "thinking") == 0) return AI_STATE_THINKING;
    if (strcasecmp(s, "asking") == 0)   return AI_STATE_ASKING;
    if (strcasecmp(s, "running") == 0)  return AI_STATE_RUNNING;
    if (strcasecmp(s, "success") == 0)  return AI_STATE_SUCCESS;
    if (strcasecmp(s, "warning") == 0)  return AI_STATE_WARNING;
    if (strcasecmp(s, "error") == 0)    return AI_STATE_ERROR;
    return is_ask ? AI_STATE_ASKING : AI_STATE_RUNNING;
}

static dsl_layout_t parse_layout_str(const char *s)
{
    if (!s || !s[0]) return DSL_LAYOUT_TEXT;
    if (strcasecmp(s, "text") == 0)     return DSL_LAYOUT_TEXT;
    if (strcasecmp(s, "kv") == 0)       return DSL_LAYOUT_KV;
    if (strcasecmp(s, "progress") == 0) return DSL_LAYOUT_PROGRESS;
    if (strcasecmp(s, "choice") == 0)   return DSL_LAYOUT_CHOICE;
    if (strcasecmp(s, "number") == 0)   return DSL_LAYOUT_NUMBER;
    return DSL_LAYOUT_TEXT;
}

static dsl_btn_style_t parse_btn_style_str(const char *s)
{
    if (!s || !s[0]) return DSL_BTN_PRIMARY;
    if (strcasecmp(s, "primary") == 0)   return DSL_BTN_PRIMARY;
    if (strcasecmp(s, "success") == 0)   return DSL_BTN_SUCCESS;
    if (strcasecmp(s, "danger") == 0)    return DSL_BTN_DANGER;
    if (strcasecmp(s, "warning") == 0)   return DSL_BTN_WARNING;
    if (strcasecmp(s, "secondary") == 0) return DSL_BTN_SECONDARY;
    return DSL_BTN_PRIMARY;
}

static void copy_cjson_str(const cJSON *obj, const char *field, char *dst, size_t dst_sz, const char *def_val)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, field);
    if (cJSON_IsString(it) && it->valuestring) {
        strlcpy(dst, it->valuestring, dst_sz);
    } else if (def_val) {
        strlcpy(dst, def_val, dst_sz);
    } else {
        dst[0] = '\0';
    }
}

static void finalize_response_locked(dsl_res_status_t st, int btn_idx, const char *source)
{
    int64_t now_ms = esp_timer_get_time() / 1000LL;
    uint32_t elapsed = (now_ms > s_start_ms) ? (uint32_t)(now_ms - s_start_ms) : 0;

    memset(&s_active_resp, 0, sizeof(s_active_resp));
    strlcpy(s_active_resp.req_id, s_active_req.req_id, sizeof(s_active_resp.req_id));
    s_active_resp.status = st;
    s_active_resp.elapsed_ms = elapsed;
    strlcpy(s_active_resp.source, source ? source : "api", sizeof(s_active_resp.source));

    if (btn_idx >= 0 && btn_idx < s_active_req.button_count) {
        strlcpy(s_active_resp.action_id, s_active_req.buttons[btn_idx].id, sizeof(s_active_resp.action_id));
        strlcpy(s_active_resp.action_label, s_active_req.buttons[btn_idx].label, sizeof(s_active_resp.action_label));
    }

    if (s_active_req.layout == DSL_LAYOUT_CHOICE) {
        for (int i = 0; i < s_active_req.choice_count && s_active_resp.selected_count < DSL_MAX_CHOICES; i++) {
            if (s_active_req.choices[i].selected) {
                strlcpy(s_active_resp.selected_choices[s_active_resp.selected_count],
                        s_active_req.choices[i].id, DSL_STR_ID_LEN);
                s_active_resp.selected_count++;
            }
        }
    } else if (s_active_req.layout == DSL_LAYOUT_NUMBER) {
        s_active_resp.has_number_value = true;
        s_active_resp.number_value = s_active_req.num_value;
    }

    s_last_resp = s_active_resp;
    strlcpy(s_last_title, s_active_req.title, sizeof(s_last_title));
    s_waiting_reply = false;
    s_has_active = false;
}

static void dsl_tick_cb(void *arg)
{
    (void)arg;
    if (!s_mux) return;

    bool need_give = false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_has_active && s_waiting_reply && s_active_req.timeout_sec > 0) {
        int64_t now_ms = esp_timer_get_time() / 1000LL;
        int64_t limit_ms = (int64_t)s_active_req.timeout_sec * 1000LL;
        if ((now_ms - s_start_ms) >= limit_ms) {
            int def_idx = -1;
            if (s_active_req.default_button_id[0] != '\0') {
                for (int i = 0; i < s_active_req.button_count; i++) {
                    if (strcmp(s_active_req.buttons[i].id, s_active_req.default_button_id) == 0) {
                        def_idx = i;
                        break;
                    }
                }
            }
            if (def_idx < 0) {
                for (int i = 0; i < s_active_req.button_count; i++) {
                    if (s_active_req.buttons[i].is_default) {
                        def_idx = i;
                        break;
                    }
                }
            }

            if (def_idx >= 0) {
                finalize_response_locked(DSL_RES_TIMEOUT_DEFAULT, def_idx, "timeout");
            } else {
                finalize_response_locked(DSL_RES_TIMEOUT, -1, "timeout");
            }
            need_give = true;
        }
    }
    xSemaphoreGive(s_mux);

    if (need_give) {
        xSemaphoreGive(s_reply_sem);
    }
}

esp_err_t app_dsl_init(void)
{
    s_mux = xSemaphoreCreateMutex();
    s_reply_sem = xSemaphoreCreateBinary();
    memset(&s_active_req, 0, sizeof(s_active_req));
    memset(&s_active_resp, 0, sizeof(s_active_resp));
    memset(&s_last_resp, 0, sizeof(s_last_resp));

    esp_timer_create_args_t t_args = {
        .callback = dsl_tick_cb,
        .name = "dsl_tick",
    };
    ESP_ERROR_CHECK(esp_timer_create(&t_args, &s_tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_tick_timer, 100 * 1000)); /* 100ms */
    return ESP_OK;
}

esp_err_t app_dsl_parse_json(const char *json_str, bool is_ask_endpoint, dsl_request_t *out_req,
                             char *err_buf, size_t err_buf_sz)
{
    if (!json_str || !out_req) {
        if (err_buf) snprintf(err_buf, err_buf_sz, "empty payload");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *root = cJSON_Parse(json_str);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        if (err_buf) snprintf(err_buf, err_buf_sz, "invalid json object");
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_req, 0, sizeof(*out_req));
    out_req->is_ask = is_ask_endpoint;

    const cJSON *j_wait = cJSON_GetObjectItemCaseSensitive(root, "wait");
    if (cJSON_IsBool(j_wait)) {
        out_req->wait = cJSON_IsTrue(j_wait);
    } else {
        out_req->wait = is_ask_endpoint;
    }

    copy_cjson_str(root, "req_id", out_req->req_id, sizeof(out_req->req_id), NULL);
    if (out_req->req_id[0] == '\0') {
        s_req_seq++;
        snprintf(out_req->req_id, sizeof(out_req->req_id), "req_%lu", (unsigned long)s_req_seq);
    }

    const cJSON *j_state = cJSON_GetObjectItemCaseSensitive(root, "state");
    out_req->state = parse_state_str(cJSON_IsString(j_state) ? j_state->valuestring : NULL, is_ask_endpoint);

    copy_cjson_str(root, "agent", out_req->agent, sizeof(out_req->agent), "AI Agent");
    copy_cjson_str(root, "title", out_req->title, sizeof(out_req->title),
                   is_ask_endpoint ? "等待用户确认" : "AI 状态更新");
    copy_cjson_str(root, "subtitle", out_req->subtitle, sizeof(out_req->subtitle), "");

    const cJSON *j_layout = cJSON_GetObjectItemCaseSensitive(root, "layout");
    out_req->layout = parse_layout_str(cJSON_IsString(j_layout) ? j_layout->valuestring : NULL);

    /* Layout specific fields */
    copy_cjson_str(root, "body", out_req->body, sizeof(out_req->body), "");
    copy_cjson_str(root, "callout", out_req->callout, sizeof(out_req->callout), "");

    /* KV items (max 4) */
    const cJSON *j_kv = cJSON_GetObjectItemCaseSensitive(root, "kv");
    if (cJSON_IsArray(j_kv)) {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, j_kv) {
            if (out_req->kv_count >= DSL_MAX_KV_ITEMS) break;
            if (!cJSON_IsObject(item)) continue;
            dsl_kv_item_t *kv = &out_req->kv_items[out_req->kv_count];
            copy_cjson_str(item, "k", kv->key, sizeof(kv->key), "");
            if (kv->key[0] == '\0') copy_cjson_str(item, "key", kv->key, sizeof(kv->key), "项");
            copy_cjson_str(item, "v", kv->val, sizeof(kv->val), "");
            if (kv->val[0] == '\0') copy_cjson_str(item, "val", kv->val, sizeof(kv->val), "");
            const cJSON *hl = cJSON_GetObjectItemCaseSensitive(item, "highlight");
            kv->highlight = cJSON_IsTrue(hl);
            out_req->kv_count++;
        }
    }

    /* Progress items */
    const cJSON *j_pct = cJSON_GetObjectItemCaseSensitive(root, "percent");
    if (cJSON_IsNumber(j_pct)) {
        int p = (int)j_pct->valuedouble;
        if (p < 0) p = 0;
        if (p > 100) p = 100;
        out_req->progress_percent = (uint8_t)p;
    }
    copy_cjson_str(root, "step_text", out_req->step_text, sizeof(out_req->step_text), "");
    const cJSON *j_steps = cJSON_GetObjectItemCaseSensitive(root, "steps");
    if (cJSON_IsArray(j_steps)) {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, j_steps) {
            if (out_req->step_count >= DSL_MAX_STEPS) break;
            if (cJSON_IsString(item) && item->valuestring) {
                strlcpy(out_req->steps[out_req->step_count].label, item->valuestring, DSL_STR_LABEL_LEN);
                out_req->step_count++;
            } else if (cJSON_IsObject(item)) {
                dsl_step_item_t *st = &out_req->steps[out_req->step_count];
                copy_cjson_str(item, "label", st->label, sizeof(st->label), "步骤");
                st->done = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "done"));
                st->active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "active"));
                out_req->step_count++;
            }
        }
    }

    /* Choice items (max 6) */
    out_req->choice_multi = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "multi"));
    const cJSON *j_choices = cJSON_GetObjectItemCaseSensitive(root, "choices");
    if (cJSON_IsArray(j_choices)) {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, j_choices) {
            if (out_req->choice_count >= DSL_MAX_CHOICES) break;
            dsl_choice_item_t *ch = &out_req->choices[out_req->choice_count];
            if (cJSON_IsString(item) && item->valuestring) {
                snprintf(ch->id, sizeof(ch->id), "opt_%d", out_req->choice_count + 1);
                strlcpy(ch->label, item->valuestring, sizeof(ch->label));
                ch->selected = (out_req->choice_count == 0 && !out_req->choice_multi);
                out_req->choice_count++;
            } else if (cJSON_IsObject(item)) {
                copy_cjson_str(item, "id", ch->id, sizeof(ch->id), NULL);
                if (ch->id[0] == '\0') {
                    snprintf(ch->id, sizeof(ch->id), "opt_%d", out_req->choice_count + 1);
                }
                copy_cjson_str(item, "label", ch->label, sizeof(ch->label), ch->id);
                ch->selected = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "selected"));
                out_req->choice_count++;
            }
        }
    }

    /* Number adjuster */
    const cJSON *j_min = cJSON_GetObjectItemCaseSensitive(root, "min");
    const cJSON *j_max = cJSON_GetObjectItemCaseSensitive(root, "max");
    const cJSON *j_step = cJSON_GetObjectItemCaseSensitive(root, "step");
    const cJSON *j_val = cJSON_GetObjectItemCaseSensitive(root, "value");
    out_req->num_min = cJSON_IsNumber(j_min) ? (int32_t)j_min->valuedouble : 0;
    out_req->num_max = cJSON_IsNumber(j_max) ? (int32_t)j_max->valuedouble : 100;
    if (out_req->num_max < out_req->num_min) out_req->num_max = out_req->num_min + 10;
    out_req->num_step = cJSON_IsNumber(j_step) ? (int32_t)j_step->valuedouble : 1;
    if (out_req->num_step <= 0) out_req->num_step = 1;
    out_req->num_value = cJSON_IsNumber(j_val) ? (int32_t)j_val->valuedouble : out_req->num_min;
    if (out_req->num_value < out_req->num_min) out_req->num_value = out_req->num_min;
    if (out_req->num_value > out_req->num_max) out_req->num_value = out_req->num_max;
    copy_cjson_str(root, "unit", out_req->num_unit, sizeof(out_req->num_unit), "");

    /* Buttons (strictly constrained 0..4 fixed slots) */
    copy_cjson_str(root, "default_button_id", out_req->default_button_id, sizeof(out_req->default_button_id), "");
    const cJSON *j_btns = cJSON_GetObjectItemCaseSensitive(root, "buttons");
    if (cJSON_IsArray(j_btns)) {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, j_btns) {
            if (out_req->button_count >= DSL_MAX_BUTTONS) break;
            dsl_button_t *b = &out_req->buttons[out_req->button_count];
            if (cJSON_IsString(item) && item->valuestring) {
                strlcpy(b->id, item->valuestring, sizeof(b->id));
                strlcpy(b->label, item->valuestring, sizeof(b->label));
                b->style = (out_req->button_count == 0) ? DSL_BTN_PRIMARY : DSL_BTN_SECONDARY;
                b->is_default = (out_req->button_count == 0);
                out_req->button_count++;
            } else if (cJSON_IsObject(item)) {
                copy_cjson_str(item, "id", b->id, sizeof(b->id), NULL);
                if (b->id[0] == '\0') {
                    snprintf(b->id, sizeof(b->id), "btn_%d", out_req->button_count + 1);
                }
                copy_cjson_str(item, "label", b->label, sizeof(b->label), b->id);
                const cJSON *st = cJSON_GetObjectItemCaseSensitive(item, "style");
                b->style = parse_btn_style_str(cJSON_IsString(st) ? st->valuestring : NULL);
                b->is_default = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "is_default"));
                if (b->is_default && out_req->default_button_id[0] == '\0') {
                    strlcpy(out_req->default_button_id, b->id, sizeof(out_req->default_button_id));
                }
                out_req->button_count++;
            }
        }
    }

    if (is_ask_endpoint && out_req->button_count == 0) {
        out_req->button_count = 2;
        strlcpy(out_req->buttons[0].id, "confirm", DSL_STR_ID_LEN);
        strlcpy(out_req->buttons[0].label, "确认", DSL_STR_LABEL_LEN);
        out_req->buttons[0].style = DSL_BTN_PRIMARY;
        out_req->buttons[0].is_default = true;

        strlcpy(out_req->buttons[1].id, "cancel", DSL_STR_ID_LEN);
        strlcpy(out_req->buttons[1].label, "取消", DSL_STR_LABEL_LEN);
        out_req->buttons[1].style = DSL_BTN_SECONDARY;
        out_req->buttons[1].is_default = false;
    }

    /* Timeout */
    const cJSON *j_to = cJSON_GetObjectItemCaseSensitive(root, "timeout_sec");
    if (cJSON_IsNumber(j_to)) {
        int t = (int)j_to->valuedouble;
        if (t < 0) t = 0;
        if (t > 3600) t = 3600;
        out_req->timeout_sec = (uint16_t)t;
    } else {
        out_req->timeout_sec = is_ask_endpoint ? 60 : 0;
    }

    /* Sound: either string preset or object {"preset": "...", "melody": "...", "repeat": 1} */
    out_req->sound_repeat = 1;
    out_req->sound_preset = AUDIO_PRESET_NONE;
    const cJSON *j_snd = cJSON_GetObjectItemCaseSensitive(root, "sound");
    if (cJSON_IsString(j_snd) && j_snd->valuestring) {
        out_req->sound_preset = bsp_audio_preset_from_str(j_snd->valuestring);
    } else if (cJSON_IsObject(j_snd)) {
        const cJSON *jp = cJSON_GetObjectItemCaseSensitive(j_snd, "preset");
        if (cJSON_IsString(jp) && jp->valuestring) {
            out_req->sound_preset = bsp_audio_preset_from_str(jp->valuestring);
        }
        copy_cjson_str(j_snd, "melody", out_req->sound_melody, sizeof(out_req->sound_melody), "");
        const cJSON *jr = cJSON_GetObjectItemCaseSensitive(j_snd, "repeat");
        if (cJSON_IsNumber(jr)) {
            int rp = (int)jr->valuedouble;
            if (rp < 1) rp = 1;
            if (rp > 5) rp = 5;
            out_req->sound_repeat = (uint8_t)rp;
        }
    } else if (is_ask_endpoint) {
        out_req->sound_preset = AUDIO_PRESET_ASK;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t app_dsl_submit_request(const dsl_request_t *req, uint32_t *out_session_gen)
{
    if (!req) return ESP_ERR_INVALID_ARG;

    bool wake_superseded = false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_has_active && s_waiting_reply) {
        finalize_response_locked(DSL_RES_SUPERSEDED, -1, "superseded");
        wake_superseded = true;
    }

    s_session_gen++;
    s_active_req = *req;
    s_start_ms = esp_timer_get_time() / 1000LL;
    s_has_active = true;
    s_waiting_reply = req->is_ask;

    memset(&s_active_resp, 0, sizeof(s_active_resp));
    strlcpy(s_active_resp.req_id, req->req_id, sizeof(s_active_resp.req_id));
    s_active_resp.status = req->is_ask ? DSL_RES_PENDING : DSL_RES_NONE;

    if (out_session_gen) {
        *out_session_gen = s_session_gen;
    }
    xSemaphoreGive(s_mux);

    if (wake_superseded) {
        xSemaphoreGive(s_reply_sem);
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    /* Clear stale semaphore token */
    xSemaphoreTake(s_reply_sem, 0);

    /* Wake display & play notification sound */
    bsp_display_sleep(false);
    bsp_display_reset_idle_timer();
    if (req->sound_melody[0] != '\0') {
        bsp_audio_play_melody(req->sound_melody, req->sound_repeat);
    } else if (req->sound_preset != AUDIO_PRESET_NONE) {
        bsp_audio_play_preset(req->sound_preset, req->sound_repeat);
    }

    ESP_LOGI(TAG, "DSL active: req_id=%s state=%s layout=%s btns=%u wait=%d",
             req->req_id, app_dsl_state_to_str(req->state),
             app_dsl_layout_to_str(req->layout), req->button_count, req->wait ? 1 : 0);
    return ESP_OK;
}

esp_err_t app_dsl_wait_response(uint32_t session_gen, uint32_t max_wait_ms, dsl_response_t *out_resp)
{
    if (!out_resp) return ESP_ERR_INVALID_ARG;

    int64_t deadline = (esp_timer_get_time() / 1000LL) + (int64_t)max_wait_ms;
    for (;;) {
        xSemaphoreTake(s_mux, portMAX_DELAY);
        if (s_session_gen != session_gen) {
            *out_resp = s_last_resp;
            xSemaphoreGive(s_mux);
            return ESP_OK;
        }
        if (!s_waiting_reply) {
            *out_resp = s_active_resp;
            xSemaphoreGive(s_mux);
            return ESP_OK;
        }
        xSemaphoreGive(s_mux);

        int64_t now_ms = esp_timer_get_time() / 1000LL;
        if (now_ms >= deadline) {
            xSemaphoreTake(s_mux, portMAX_DELAY);
            if (s_session_gen == session_gen && s_waiting_reply) {
                finalize_response_locked(DSL_RES_TIMEOUT, -1, "timeout");
                *out_resp = s_active_resp;
            } else {
                *out_resp = s_last_resp;
            }
            xSemaphoreGive(s_mux);
            return ESP_OK;
        }

        uint32_t slice = (uint32_t)(deadline - now_ms);
        if (slice > 250) slice = 250;
        xSemaphoreTake(s_reply_sem, pdMS_TO_TICKS(slice));
    }
}

bool app_dsl_get_active_snapshot(dsl_request_t *out_req, uint32_t *out_remaining_ms,
                                 uint32_t *out_session_gen, bool *out_waiting)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    bool active = s_has_active;
    if (active && out_req) {
        *out_req = s_active_req;
    }
    if (out_remaining_ms) {
        if (active && s_active_req.timeout_sec > 0) {
            int64_t now_ms = esp_timer_get_time() / 1000LL;
            int64_t total_ms = (int64_t)s_active_req.timeout_sec * 1000LL;
            int64_t rem = total_ms - (now_ms - s_start_ms);
            *out_remaining_ms = (rem > 0) ? (uint32_t)rem : 0;
        } else {
            *out_remaining_ms = 0;
        }
    }
    if (out_session_gen) {
        *out_session_gen = s_session_gen;
    }
    if (out_waiting) {
        *out_waiting = s_waiting_reply;
    }
    xSemaphoreGive(s_mux);
    return active;
}

bool app_dsl_get_last_response(dsl_response_t *out_resp, char *out_last_title, size_t title_sz)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    bool valid = (s_last_resp.req_id[0] != '\0');
    if (out_resp) *out_resp = s_last_resp;
    if (out_last_title && title_sz > 0) {
        strlcpy(out_last_title, s_last_title, title_sz);
    }
    xSemaphoreGive(s_mux);
    return valid;
}

bool app_dsl_get_result_by_id(const char *req_id, dsl_response_t *out_resp)
{
    if (!req_id || !out_resp) return false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_has_active && strcmp(s_active_req.req_id, req_id) == 0) {
        *out_resp = s_active_resp;
        xSemaphoreGive(s_mux);
        return true;
    }
    if (strcmp(s_last_resp.req_id, req_id) == 0) {
        *out_resp = s_last_resp;
        xSemaphoreGive(s_mux);
        return true;
    }
    xSemaphoreGive(s_mux);
    return false;
}

void app_dsl_toggle_choice(int idx)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_has_active && s_active_req.layout == DSL_LAYOUT_CHOICE &&
        idx >= 0 && idx < s_active_req.choice_count) {
        if (s_active_req.choice_multi) {
            s_active_req.choices[idx].selected = !s_active_req.choices[idx].selected;
        } else {
            for (int i = 0; i < s_active_req.choice_count; i++) {
                s_active_req.choices[i].selected = (i == idx);
            }
        }
    }
    xSemaphoreGive(s_mux);
    bsp_audio_play_preset(AUDIO_PRESET_CLICK, 1);
}

void app_dsl_adjust_number(int delta_steps)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_has_active && s_active_req.layout == DSL_LAYOUT_NUMBER) {
        int32_t v = s_active_req.num_value + delta_steps * s_active_req.num_step;
        if (v < s_active_req.num_min) v = s_active_req.num_min;
        if (v > s_active_req.num_max) v = s_active_req.num_max;
        s_active_req.num_value = v;
    }
    xSemaphoreGive(s_mux);
    bsp_audio_play_preset(AUDIO_PRESET_CLICK, 1);
}

void app_dsl_set_number_value(int32_t val)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_has_active && s_active_req.layout == DSL_LAYOUT_NUMBER) {
        if (val < s_active_req.num_min) val = s_active_req.num_min;
        if (val > s_active_req.num_max) val = s_active_req.num_max;
        s_active_req.num_value = val;
    }
    xSemaphoreGive(s_mux);
    bsp_audio_play_preset(AUDIO_PRESET_CLICK, 1);
}

void app_dsl_submit_button(int btn_idx, const char *source)
{
    bool wake = false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_has_active && btn_idx >= 0 && btn_idx < s_active_req.button_count) {
        wake = s_waiting_reply;
        finalize_response_locked(DSL_RES_ANSWERED, btn_idx, source);
    }
    xSemaphoreGive(s_mux);

    bsp_audio_play_preset(AUDIO_PRESET_CLICK, 1);
    if (wake) {
        xSemaphoreGive(s_reply_sem);
    }
}

void app_dsl_submit_default_or_first(const char *source)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    int target = -1;
    if (s_has_active && s_active_req.button_count > 0) {
        target = 0;
        for (int i = 0; i < s_active_req.button_count; i++) {
            if (s_active_req.buttons[i].is_default ||
                (s_active_req.default_button_id[0] != '\0' &&
                 strcmp(s_active_req.buttons[i].id, s_active_req.default_button_id) == 0)) {
                target = i;
                break;
            }
        }
    }
    xSemaphoreGive(s_mux);

    if (target >= 0) {
        app_dsl_submit_button(target, source);
    } else {
        app_dsl_dismiss(source);
    }
}

void app_dsl_dismiss(const char *source)
{
    bool wake = false;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (s_has_active) {
        wake = s_waiting_reply;
        finalize_response_locked(DSL_RES_DISMISSED, -1, source);
    }
    xSemaphoreGive(s_mux);

    bsp_audio_stop();
    if (wake) {
        xSemaphoreGive(s_reply_sem);
    }
}

char *app_dsl_response_to_json(const dsl_response_t *resp)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "req_id", resp->req_id);
    cJSON_AddStringToObject(root, "status", app_dsl_res_status_to_str(resp->status));
    if (resp->action_id[0] != '\0') {
        cJSON_AddStringToObject(root, "action_id", resp->action_id);
        cJSON_AddStringToObject(root, "action_label", resp->action_label);
    } else {
        cJSON_AddNullToObject(root, "action_id");
    }

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < resp->selected_count; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(resp->selected_choices[i]));
    }
    cJSON_AddItemToObject(root, "selected_choices", arr);

    if (resp->has_number_value) {
        cJSON_AddNumberToObject(root, "number_value", resp->number_value);
    }
    cJSON_AddNumberToObject(root, "elapsed_ms", resp->elapsed_ms);
    cJSON_AddStringToObject(root, "source", resp->source);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

const char *app_dsl_state_to_str(ai_state_t st)
{
    switch (st) {
    case AI_STATE_IDLE:     return "idle";
    case AI_STATE_THINKING: return "thinking";
    case AI_STATE_ASKING:   return "asking";
    case AI_STATE_RUNNING:  return "running";
    case AI_STATE_SUCCESS:  return "success";
    case AI_STATE_WARNING:  return "warning";
    case AI_STATE_ERROR:    return "error";
    default:                return "idle";
    }
}

const char *app_dsl_state_to_cn(ai_state_t st)
{
    switch (st) {
    case AI_STATE_IDLE:     return "待机中";
    case AI_STATE_THINKING: return "AI 思考中";
    case AI_STATE_ASKING:   return "等待确认";
    case AI_STATE_RUNNING:  return "执行中";
    case AI_STATE_SUCCESS:  return "已完成";
    case AI_STATE_WARNING:  return "注意警告";
    case AI_STATE_ERROR:    return "发生错误";
    default:                return "待机中";
    }
}

const char *app_dsl_layout_to_str(dsl_layout_t lay)
{
    switch (lay) {
    case DSL_LAYOUT_TEXT:     return "text";
    case DSL_LAYOUT_KV:       return "kv";
    case DSL_LAYOUT_PROGRESS: return "progress";
    case DSL_LAYOUT_CHOICE:   return "choice";
    case DSL_LAYOUT_NUMBER:   return "number";
    default:                  return "text";
    }
}

const char *app_dsl_res_status_to_str(dsl_res_status_t st)
{
    switch (st) {
    case DSL_RES_PENDING:         return "pending";
    case DSL_RES_ANSWERED:        return "answered";
    case DSL_RES_TIMEOUT_DEFAULT: return "timeout_default";
    case DSL_RES_TIMEOUT:         return "timeout";
    case DSL_RES_DISMISSED:       return "dismissed";
    case DSL_RES_SUPERSEDED:      return "superseded";
    default:                      return "none";
    }
}
