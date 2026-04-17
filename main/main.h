#pragma once

#include <inttypes.h>
#include <string.h>

#include "ld2420.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "wifi.h"
#include "http_server.h"

static char          s_time[32]   = "--:--:--";
static bool          s_data_valid = false;
static SemaphoreHandle_t s_data_mutex = NULL;

static esp_err_t api_config_post_handler(httpd_req_t *req);

esp_err_t monitor_handler(httpd_req_t *req);

void ld2420_print_status_line(void);

httpd_handle_t server = NULL;
