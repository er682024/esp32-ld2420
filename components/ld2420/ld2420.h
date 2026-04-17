#pragma once

#include <stdio.h>
#include <string.h> 

#include "stdbool.h"
#include "stdint.h"
#include "driver/gpio.h"
#include "driver/uart.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

typedef struct {
    gpio_num_t pin_ot1;      // opzionale, può essere -1
    gpio_num_t pin_ot2;      // richiesto
    uart_port_t uart_num;    // es. UART_NUM_2
    gpio_num_t uart_tx;      // ESP32 TX -> LD2420 RX
    int uart_baud;           // es. 256000
} ld2420_config_t;

typedef struct {
    bool presence;           // presenza complessiva
    bool motion;             // movimento (OT2)
    bool static_presence;    // presenza statica (OT1)
    uint32_t distance_min;
    uint32_t distance_max;
} ld2420_state_t;

typedef struct {
    uint16_t max_gate;
    uint16_t static_threshold[16];
    uint16_t motion_threshold[16];
    uint16_t timeout;
    uint16_t output_mode;
    uint16_t sensitivity;
    bool auto_threshold;
} ld2420_params_t;

esp_err_t ld2420_init(const ld2420_config_t *cfg);
void      ld2420_task_start(UBaseType_t priority, uint32_t stack_size);

ld2420_state_t ld2420_get_state(void);

void ld2420_enter_engineering_mode(void);
void ld2420_exit_engineering_mode(void);
esp_err_t ld2420_apply_default_config(void);
esp_err_t ld2420_set_range(uint16_t min_cm, uint16_t max_cm);

uint32_t ld2420_ms_since_presence();
uint32_t ld2420_ms_since_absence();
uint32_t ld2420_ms_since_motion();
uint32_t ld2420_ms_since_static_presence(void);
uint32_t ld2420_ms_since_dynamic_presence(void);
uint32_t ld2420_ms_since_state_change(void);

uint16_t ld2420_get_min_distance();
uint16_t ld2420_get_max_distance();
uint8_t  ld2420_get_sensitivity();
uint32_t ld2420_get_uptime_ms();

void ld2420_print_params(const ld2420_params_t *params);
void ld2420_print_gate_map(uint16_t max_gate, const uint16_t *static_thr, const uint16_t *motion_thr);




