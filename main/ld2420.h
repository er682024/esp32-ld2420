// main/ld2420.h
#pragma once
#include "driver/uart.h"
#include "driver/gpio.h"

#define LD2420_UART UART_NUM_2
#define LD2420_TX   17
#define LD2420_RX   16
#define LD2420_OT2  4

void ld2420_init(void);
void ld2420_task(void *pv);
