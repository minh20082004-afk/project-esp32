#include "mqtt_sub_task.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "cJSON.h"

#define WIFI_SSID        "Tongtai_Office"
#define WIFI_PASS        "66668888"
#define MQTT_BROKER_URI  "mqtt://broker.emqx.io:1883"

#define TOPIC_STATUS     "node2/status"
#define TOPIC_LOG        "node2/log"

#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "mqtt_sub";

static EventGroupHandle_t s_wifi_event_group;
static esp_mqtt_client_handle_t s_mqtt_client;

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        esp_mqtt_client_subscribe(s_mqtt_client, TOPIC_STATUS, 0);
        esp_mqtt_client_subscribe(s_mqtt_client, TOPIC_LOG, 0);
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
    case MQTT_EVENT_DATA: {
        int64_t now_us = esp_timer_get_time();
        int64_t now_ms = now_us / 1000;

        ESP_LOGI(TAG, "MQTT message received");
        ESP_LOGI(TAG, "topic: %.*s", event->topic_len, event->topic);

        char *payload = (char *)malloc(event->data_len + 1);
        if (!payload) {
            ESP_LOGE(TAG, "malloc failed for payload");
            break;
        }
        memcpy(payload, event->data, event->data_len);
        payload[event->data_len] = '\0';

        cJSON *root = cJSON_Parse(payload);
        if (!root) {
            ESP_LOGW(TAG, "Invalid JSON payload: %s", payload);
            free(payload);
            break;
        }

        cJSON *ts_ms = cJSON_GetObjectItemCaseSensitive(root, "ts_ms");
        cJSON *ts_iso = cJSON_GetObjectItemCaseSensitive(root, "ts_iso");
        cJSON *ts_valid = cJSON_GetObjectItemCaseSensitive(root, "ts_valid");
        cJSON *event_field = cJSON_GetObjectItemCaseSensitive(root, "event");
        cJSON *pump = cJSON_GetObjectItemCaseSensitive(root, "pump");
        cJSON *flooded = cJSON_GetObjectItemCaseSensitive(root, "flooded");
        cJSON *water_cm = cJSON_GetObjectItemCaseSensitive(root, "water_cm");

        if (cJSON_IsNumber(ts_ms)) {
            ESP_LOGI(TAG, "ts_ms: %lld", (long long)ts_ms->valuedouble);
        } else {
            ESP_LOGI(TAG, "ts_ms: (missing)");
        }

        if (cJSON_IsString(ts_iso) && (ts_iso->valuestring != NULL)) {
            ESP_LOGI(TAG, "ts_iso: %s", ts_iso->valuestring);
        } else {
            ESP_LOGI(TAG, "ts_iso: (missing)");
        }

        if (cJSON_IsBool(ts_valid)) {
            ESP_LOGI(TAG, "ts_valid: %s", cJSON_IsTrue(ts_valid) ? "true" : "false");
        } else {
            ESP_LOGI(TAG, "ts_valid: (missing)");
        }

        if (cJSON_IsString(event_field) && (event_field->valuestring != NULL)) {
            ESP_LOGI(TAG, "event: %s", event_field->valuestring);
        }

        if (cJSON_IsBool(pump)) {
            ESP_LOGI(TAG, "pump: %s", cJSON_IsTrue(pump) ? "true" : "false");
        } else if (cJSON_IsNumber(pump)) {
            ESP_LOGI(TAG, "pump: %d", pump->valueint);
        } else {
            ESP_LOGI(TAG, "pump: (missing)");
        }

        if (cJSON_IsBool(flooded)) {
            ESP_LOGI(TAG, "flooded: %s", cJSON_IsTrue(flooded) ? "true" : "false");
        } else if (cJSON_IsNumber(flooded)) {
            ESP_LOGI(TAG, "flooded: %d", flooded->valueint);
        } else {
            ESP_LOGI(TAG, "flooded: (missing)");
        }

        if (cJSON_IsNumber(water_cm)) {
            ESP_LOGI(TAG, "water_cm: %.2f", water_cm->valuedouble);
        } else {
            ESP_LOGI(TAG, "water_cm: (missing)");
        }

        cJSON_Delete(root);
        free(payload);
        ESP_LOGI(TAG, "rx_timestamp_ms: %lld", (long long)now_ms);
        break;
    }
    default:
        break;
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying...");
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, WIFI_PASS, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi init done");
}

static void mqtt_start(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = MQTT_BROKER_URI,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt_client));
}

static void mqtt_sub_task(void *param)
{
    wifi_init_sta();

    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    ESP_LOGI(TAG, "Wi-Fi connected, starting MQTT");

    mqtt_start();

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void mqtt_sub_task_start(void)
{
    xTaskCreate(mqtt_sub_task, "mqtt_sub_task", 4096, NULL, 5, NULL);
}
