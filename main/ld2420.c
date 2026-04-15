// main/ld2420.c
#include "ld2420.h"
#include "esp_log.h"

static const char *TAG = "LD2420";

void ld2420_init(void)
{
    uart_config_t cfg = {
        .baud_rate = 256000,   // baud standard LD2420
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    uart_param_config(LD2420_UART, &cfg);
    uart_set_pin(LD2420_UART, LD2420_TX, LD2420_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(LD2420_UART, 2048, 0, 0, NULL, 0);

    gpio_set_direction(LD2420_OT2, GPIO_MODE_INPUT);

    ESP_LOGI(TAG, "LD2420 UART initialized");
}

void ld2420_task(void *pv)
{
    uint8_t buf[256];

    while (1) {
        int len = uart_read_bytes(LD2420_UART, buf, sizeof(buf), pdMS_TO_TICKS(50));
        if (len > 0) {
            // Frame energia: header F4 F3 F2 F1
            if (len >= 8 && buf[0] == 0xF4 && buf[1] == 0xF3) {
                uint8_t presence = buf[6];
                uint16_t distance = buf[7] | (buf[8] << 8);

                ESP_LOGI(TAG, "Presence=%d  Distance=%d cm", presence, distance);
            }
        }

        // OT2 digitale
        int ot2 = gpio_get_level(LD2420_OT2);
        if (ot2) {
            ESP_LOGI(TAG, "OT2 presence detected");
        }
    }
}
