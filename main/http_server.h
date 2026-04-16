#pragma once
#include "esp_http_server.h"
#include <stdint.h>
#include "ld2420.h"

void http_server_start(void);
void http_server_stop(void);
//void http_server_update_data(const char *time_str);
void http_server_update_data(const char *time_str, ld2420_state_t state, uint32_t min, uint32_t max);