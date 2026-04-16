#include <inttypes.h>

#include "ld2420.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "time.h"
#include "wifi.h"
#include "http_server.h"

extern bool wifi_connected;

static const char *TAG = "MAIN";

void ld2420_print_status_line(void);
void time_sync_init(void);
void get_time_str(char *buf, size_t len);

void app_main(void)
{
    ld2420_config_t cfg = {
        .pin_ot1   = GPIO_NUM_16,      // o -1 se non collegato
        .pin_ot2   = GPIO_NUM_4,
        .uart_num  = UART_NUM_2,
        .uart_tx   = GPIO_NUM_17,      // ESP32 TX2 -> LD2420 RX
        .uart_baud = 256000,
    };

    char ssid[64]     = {0};
    char password[64] = {0};

    wifi_init();
    
    if (wifi_load_credentials(ssid, sizeof(ssid), password, sizeof(password))) {
        ESP_LOGI(TAG, "Credenziali trovate: SSID='%s'", ssid);
        wifi_connect(ssid, password);  // loop interno — ritorna solo se connesso
    } else {
        ESP_LOGW(TAG, "Nessuna credenziale salvata");
    }

    if (wifi_connected) {
        time_sync_init();
        http_server_start();
    } else {
        /* Nessuna credenziale o connessione impossibile:
           avvia l'AP di configurazione e resta in attesa.
           Il reboot avverrà dopo il salvataggio delle credenziali
           dall'HTTP handler. Il sensore non viene letto in questa
           modalità (non c'è dove inviare i dati). */
        wifi_start_ap();
        http_server_start();
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ld2420_init(&cfg);
    vTaskDelay(pdMS_TO_TICKS(200));
    ld2420_exit_engineering_mode();
    vTaskDelay(pdMS_TO_TICKS(200));

    ld2420_task_start(5, 4096);

    while (1) {
        ld2420_print_status_line();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void ld2420_print_status_line(void)
{
    ld2420_state_t s = ld2420_get_state();

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
        "sens=%u\n",

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
        ld2420_get_sensitivity()
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

    /* Attesa sincronizzazione: max 60 tentativi × 1s = 60s.
       Se scade, ri-inizializza SNTP e riprova indefinitamente
       ogni 30s — l'ora verrà sincronizzata non appena il DNS
       sarà raggiungibile.                                      */
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

void get_time_str(char *buf, size_t len) {
    time_t    now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(buf, len, "%d/%m/%Y %H:%M:%S", &timeinfo);
}