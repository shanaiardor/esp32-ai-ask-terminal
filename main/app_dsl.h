#ifndef APP_DSL_H
#define APP_DSL_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "dsl_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_dsl_init(void);

esp_err_t app_dsl_parse_json(const char *json_str, bool is_ask_endpoint, dsl_request_t *out_req,
                             char *err_buf, size_t err_buf_sz);

esp_err_t app_dsl_submit_request(const dsl_request_t *req, uint32_t *out_session_gen);
esp_err_t app_dsl_wait_response(uint32_t session_gen, uint32_t max_wait_ms, dsl_response_t *out_resp);

bool app_dsl_get_active_snapshot(dsl_request_t *out_req, uint32_t *out_remaining_ms,
                                 uint32_t *out_session_gen, bool *out_waiting);
bool app_dsl_get_last_response(dsl_response_t *out_resp, char *out_last_title, size_t title_sz);
bool app_dsl_get_result_by_id(const char *req_id, dsl_response_t *out_resp);

void app_dsl_toggle_choice(int idx);
void app_dsl_adjust_number(int delta_steps);
void app_dsl_set_number_value(int32_t val);

void app_dsl_submit_button(int btn_idx, const char *source);
void app_dsl_submit_default_or_first(const char *source);
void app_dsl_dismiss(const char *source);

char *app_dsl_response_to_json(const dsl_response_t *resp);

const char *app_dsl_state_to_str(ai_state_t st);
const char *app_dsl_state_to_cn(ai_state_t st);
const char *app_dsl_layout_to_str(dsl_layout_t lay);
const char *app_dsl_res_status_to_str(dsl_res_status_t st);

#ifdef __cplusplus
}
#endif

#endif /* APP_DSL_H */
