#include <stdio.h>
#include <sensor.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
QueueHandle_t control_queue;

void app_main() {
    control_queue = xQueueCreate(1, sizeof(float));

    init_adc();

    xTaskCreate(sensor_task, "sensor_task", 2048, NULL, 5, NULL);
}
