#pragma once
#include "esp_http_server.h"
#include <stdint.h>
void http_server_start(void);
void http_server_stop(void);
void http_server_update_data(const char *time_str);