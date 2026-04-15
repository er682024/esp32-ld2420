#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ld2420.h"

void app_main(void)
{
    ld2420_init();
    xTaskCreate(ld2420_task, "ld2420_task", 4096, NULL, 5, NULL);
}
