#ifndef APP_HTTP_SERVER_H
#define APP_HTTP_SERVER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_http_server_start(void);
esp_err_t app_portal_start(void);
void app_portal_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_HTTP_SERVER_H */
