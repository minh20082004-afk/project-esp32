#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "control.h"
#include "esp_err.h"
#include "esp_log.h"
#include "mqtt_task.h"
extern QueueHandle_t control_queue;

#define PUMP_GPIO GPIO_NUM_2
#define VALVE_GPIO GPIO_NUM_4
#define WATER_LEVEL_THRESHOLD 2.0
void init_control() {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PUMP_GPIO) | (1ULL << VALVE_GPIO),
        .pull_down_en = 0,
        .pull_up_en = 0
    };
    gpio_config(&io_conf);
}

void control_task(void *pvParameters) {
    float water_level;
    bool pump_on = false;
    bool was_over_threshold = false;

    while (1) {
        if (xQueueReceive(control_queue, &water_level, portMAX_DELAY) == pdPASS) {
            ESP_LOGI("SENSOR", "Water Level: %.2f cm", water_level);
            if (water_level > WATER_LEVEL_THRESHOLD) {
                gpio_set_level(PUMP_GPIO, 1);
                gpio_set_level(VALVE_GPIO, 1);
                pump_on = true;
            }
            if (water_level <= WATER_LEVEL_THRESHOLD - 0.5) {
                gpio_set_level(PUMP_GPIO, 0);
                gpio_set_level(VALVE_GPIO, 0);
                pump_on = false;
            }

            if (!was_over_threshold && water_level > WATER_LEVEL_THRESHOLD) {
                mqtt_publish_log("threshold_exceeded", water_level);
                was_over_threshold = true;
            }
            if (was_over_threshold && water_level <= WATER_LEVEL_THRESHOLD - 0.5) {
                mqtt_publish_log("threshold_cleared", water_level);
                was_over_threshold = false;
            }

            mqtt_publish_status(pump_on, water_level > WATER_LEVEL_THRESHOLD, water_level);
        }
    }
}
