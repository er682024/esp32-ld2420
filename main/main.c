#include <inttypes.h>
#include "ld2420.h"

void app_main(void)
{
    ld2420_config_t cfg = {
        .pin_ot1   = GPIO_NUM_16,      // o -1 se non collegato
        .pin_ot2   = GPIO_NUM_4,
        .uart_num  = UART_NUM_2,
        .uart_tx   = GPIO_NUM_17,      // ESP32 TX2 -> LD2420 RX
        .uart_baud = 256000,
    };

    ld2420_init(&cfg);

    vTaskDelay(pdMS_TO_TICKS(200));
    ld2420_exit_engineering_mode();
    vTaskDelay(pdMS_TO_TICKS(200));

    ld2420_task_start(5, 4096);

    while (1) {
        ld2420_state_t s = ld2420_get_state();
        printf("Uptime:   %" PRIu32 " ms\n", ld2420_get_uptime_ms());

        printf("Presence flag: %d\n", s.presence);

        // qui puoi pubblicare via MQTT, loggare, ecc.
        printf("Presence: %" PRIu32 " ms ago\n", ld2420_ms_since_presence());
        printf("Absence:  %" PRIu32 " ms ago\n", ld2420_ms_since_absence());
        printf("Motion:   %" PRIu32 " ms ago\n", ld2420_ms_since_motion());

        printf("MinDist:  %u cm\n", ld2420_get_min_distance());
        printf("MaxDist:  %u cm\n", ld2420_get_max_distance());
        printf("Sens:     %u\n", ld2420_get_sensitivity());
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
