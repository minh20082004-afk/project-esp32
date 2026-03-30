#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sensor.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "esp_log.h"

adc_oneshot_unit_handle_t adc1_handle;
int adc_value = 0;

// Receive queue from control
extern QueueHandle_t control_queue;

void init_adc() {
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
    };

    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    adc_oneshot_chan_cfg_t config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };

    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, ADC_CHANNEL_6, &config));
}

void sensor_task(void *pvParameters)
{
    float water_level;

    while (1)
    {
        ESP_ERROR_CHECK(adc_oneshot_read(adc1_handle, ADC_CHANNEL_6, &adc_value));

        ESP_LOGI("SENSOR", "ADC Value: %d", adc_value);

        water_level = (adc_value / 4095.0) * 4.0 + 0.5;

        xQueueOverwrite(control_queue, &water_level);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
