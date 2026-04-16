#pragma once
#include <stdbool.h> 
#include <stddef.h>

bool wifi_load_credentials(char *ssid, size_t ssid_len,
                            char *password, size_t pass_len);
void wifi_init(void);
void wifi_connect(const char *ssid, const char *password);
void wifi_start_ap(void);
extern bool wifi_connected;