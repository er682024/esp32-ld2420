#pragma once
#include <stdbool.h> 
#include <stddef.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"

#include "nvs_flash.h"

#include "freertos/event_groups.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY          5

#define AP_SSID     "ESP32-LD2420"
#define AP_PASSWORD "12345678"
#define AP_CHANNEL  1
#define AP_MAX_CONN 4


bool wifi_load_credentials(char *ssid, size_t ssid_len,
                            char *password, size_t pass_len);
void wifi_init(void);
void wifi_connect(const char *ssid, const char *password);
void wifi_start_ap(void);
extern bool wifi_connected;
void time_sync_init(void);
