#pragma once

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include <sys/time.h>

#include "esp_http_server.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// #include "ld2420.h"

#include "mbedtls/base64.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "wifi.h"


bool check_auth(httpd_req_t *req);
bool http_server_start(void);

esp_err_t config_page_handler(httpd_req_t *req);
esp_err_t favicon_handler(httpd_req_t *req);
esp_err_t get_handler(httpd_req_t *req);
esp_err_t ota_page_handler(httpd_req_t *req);
esp_err_t ota_upload_handler(httpd_req_t *req);
esp_err_t post_handler(httpd_req_t *req);

void get_time_str(char *buf, size_t len);


