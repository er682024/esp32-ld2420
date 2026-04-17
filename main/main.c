#include <inttypes.h>
#include <string.h>

#include "ld2420.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "time.h"
#include "wifi.h"
#include "http_server.h"

extern bool wifi_connected;

static const char *TAG = "MAIN";

void ld2420_print_status_line(void);
void time_sync_init(void);
const char* get_build_date(void);

const char* get_build_date(void)
{
    static char build_date[32] = {0};
    if (build_date[0] == 0) {
        snprintf(build_date, sizeof(build_date), "%s %s", __DATE__, __TIME__);
    }
    return build_date;
}

void app_main(void)
{
    ld2420_config_t cfg = {
        .pin_ot1   = GPIO_NUM_16,
        .pin_ot2   = GPIO_NUM_4,
        .uart_num  = UART_NUM_2,
        .uart_tx   = GPIO_NUM_17,
        .uart_baud = 256000,
    };

    char ssid[64]     = {0};
    char password[64] = {0};

    ESP_LOGI(TAG, "Firmware compilato: %s", get_build_date());

    ld2420_init(&cfg);
    vTaskDelay(pdMS_TO_TICKS(500));
    ld2420_apply_default_config();
    vTaskDelay(pdMS_TO_TICKS(500));
    // ld2420_exit_engineering_mode();
    // vTaskDelay(pdMS_TO_TICKS(200));
    ld2420_task_start(5, 4096);
    vTaskDelay(pdMS_TO_TICKS(200));

    wifi_init();

    if (wifi_load_credentials(ssid, sizeof(ssid), password, sizeof(password))) {
        ESP_LOGI(TAG, "Credenziali trovate: SSID='%s'", ssid);
        wifi_connect(ssid, password);
    } else {
        ESP_LOGW(TAG, "Nessuna credenziale salvata");
    }

    if (wifi_connected) {
        time_sync_init();
        if (http_server_start())
        {
            vTaskDelay(pdMS_TO_TICKS(5000));
            ESP_LOGI(TAG, "Sistema stabile → confermo OTA");
            // esp_ota_mark_app_valid_cancel_rollback();
        }
    } else {
        wifi_start_ap();
        http_server_start();
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    while (1) {
        // ld2420_print_status_line();
        putchar('.');
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ------------------------------------------------------------------
 * ld2420_print_status_line
 * Stampa su seriale lo stato del sensore + informazioni WiFi:
 *   SSID, RSSI (dBm), qualità segnale (%), etichetta qualità,
 *   canale, BSSID.
 * ------------------------------------------------------------------ */
void ld2420_print_status_line(void)
{
    ld2420_state_t s = ld2420_get_state();

    /* --- Lettura info WiFi --- */
    char    wifi_ssid[33]  = "--";
    char    wifi_bssid[18] = "--";
    int8_t  wifi_rssi      = 0;
    uint8_t wifi_channel   = 0;
    int     wifi_quality   = 0;   /* 0-100 % */
    const char *wifi_label = "N/A";

    wifi_ap_record_t ap_info = {0};
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        strncpy(wifi_ssid, (char *)ap_info.ssid, sizeof(wifi_ssid) - 1);
        wifi_ssid[sizeof(wifi_ssid) - 1] = '\0';

        snprintf(wifi_bssid, sizeof(wifi_bssid),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 ap_info.bssid[0], ap_info.bssid[1], ap_info.bssid[2],
                 ap_info.bssid[3], ap_info.bssid[4], ap_info.bssid[5]);

        wifi_rssi    = ap_info.rssi;
        wifi_channel = ap_info.primary;

        /* RSSI → percentuale: -100 dBm = 0%, -40 dBm = 100% */
        int q = (wifi_rssi + 100) * 100 / 60;
        if (q < 0)   q = 0;
        if (q > 100) q = 100;
        wifi_quality = q;

        if      (wifi_rssi >= -50) wifi_label = "Ottimo";
        else if (wifi_rssi >= -65) wifi_label = "Buono";
        else if (wifi_rssi >= -75) wifi_label = "Discreto";
        else if (wifi_rssi >= -85) wifi_label = "Scarso";
        else                       wifi_label = "Pessimo";
    }

    printf(
        "uptime=%" PRIu32 "ms "
        "presence=%d "
        "presence_ms=%" PRIu32 " "
        "absence_ms=%" PRIu32 " "
        "motion_ms=%" PRIu32 " "
        "static_ms=%" PRIu32 " "
        "dynamic_ms=%" PRIu32 " "
        "change_ms=%" PRIu32 " "
        "min=%u "
        "max=%u "
        "sens=%u "
        "wifi_ssid=%s "
        "wifi_rssi=%d "
        "wifi_quality=%d%% "
        "wifi_label=%s "
        "wifi_ch=%u "
        "wifi_bssid=%s\n",
        ld2420_get_uptime_ms(),
        s.presence,
        ld2420_ms_since_presence(),
        ld2420_ms_since_absence(),
        ld2420_ms_since_motion(),
        ld2420_ms_since_static_presence(),
        ld2420_ms_since_dynamic_presence(),
        ld2420_ms_since_state_change(),
        ld2420_get_min_distance(),
        ld2420_get_max_distance(),
        ld2420_get_sensitivity(),
        wifi_ssid,
        (int)wifi_rssi,
        wifi_quality,
        wifi_label,
        wifi_channel,
        wifi_bssid
    );
}

void time_sync_init(void) {
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.cloudflare.com");
    esp_sntp_setservername(2, "time.google.com");
    esp_sntp_init();

    int retry = 0;
    const int MAX_RETRY = 60;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET) {
        ESP_LOGI("NTP", "Attesa sincronizzazione... (%d/%d)", ++retry, MAX_RETRY);
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (retry >= MAX_RETRY) {
            ESP_LOGW("NTP", "Timeout — ri-inizializzo SNTP e riprovo tra 30s");
            esp_sntp_stop();
            vTaskDelay(pdMS_TO_TICKS(30000));
            esp_sntp_init();
            retry = 0;
        }
    }
    ESP_LOGI("NTP", "Ora sincronizzata");
}


