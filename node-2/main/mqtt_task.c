#include "mqtt_task.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"

static const char *TAG = "mqtt_task";

static esp_mqtt_client_handle_t s_client = NULL;
static bool s_connected = false;
static SemaphoreHandle_t s_mqtt_lock = NULL;
static EventGroupHandle_t s_wifi_event_group = NULL;
static TaskHandle_t s_mqtt_task_handle = NULL;
static QueueHandle_t s_log_queue = NULL;
static bool s_time_synced = false;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_SSID "Tongtai_Office"
#define WIFI_PASS "66668888"

#define LOG_EVENT_MAX_LEN 48
#define LOG_QUEUE_LEN 32

typedef struct {
    char event[LOG_EVENT_MAX_LEN];
    float water_level_cm;
    int64_t ts_ms;
    time_t ts_epoch;
} log_item_t;

static bool s_pending_status = false;
static bool s_last_pump_on = false;
static bool s_last_flooded = false;
static float s_last_water_level = 0.0f;

static void time_sync_notification_cb(struct timeval *tv)
{
    (void)tv;
    s_time_synced = true;
    ESP_LOGI(TAG, "SNTP time synchronized");
}

static void sntp_start_once(void)
{
    static bool sntp_started = false;
    if (sntp_started) {
        return;
    }
    sntp_started = true;

    // Set timezone to Vietnam (UTC+7)
    setenv("TZ", "ICT-7", 1);
    tzset();

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP initialized");
}

static bool format_time_iso8601(char *buf, size_t buf_len, time_t t)
{
    if (!buf || buf_len < 20) {
        return false;
    }
    struct tm tm_local;
    if (localtime_r(&t, &tm_local) == NULL) {
        return false;
    }
    size_t n = strftime(buf, buf_len, "%Y-%m-%dT%H:%M:%S%z", &tm_local);
    return n > 0;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
        ESP_LOGW(TAG, "Wi-Fi disconnected, retrying");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi connected");
        sntp_start_once();
    }
}

static void wifi_init_nonblocking(void)
{
    if (s_wifi_event_group) {
        return;
    }

    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "MQTT connected");
        if (s_pending_status) {
            mqtt_publish_status(s_last_pump_on, s_last_flooded, s_last_water_level);
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "MQTT disconnected");
        break;
    default:
        break;
    }
}

static void mqtt_task(void *pvParameters)
{
    const char *uri = (const char *)pvParameters;

    for (;;) {
        EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
        if ((bits & WIFI_CONNECTED_BIT) && !s_client) {
            esp_mqtt_client_config_t cfg = {
                .broker.address.uri = uri,
                .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
                .network.disable_auto_reconnect = false,
            };

            s_client = esp_mqtt_client_init(&cfg);
            if (s_client) {
                esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
                esp_mqtt_client_start(s_client);
                ESP_LOGI(TAG, "MQTT client started");
            } else {
                ESP_LOGE(TAG, "Failed to init MQTT client");
            }
        }

        if (!(bits & WIFI_CONNECTED_BIT) && s_client) {
            esp_mqtt_client_stop(s_client);
            esp_mqtt_client_destroy(s_client);
            s_client = NULL;
            s_connected = false;
            ESP_LOGW(TAG, "MQTT stopped (Wi-Fi down)");
        }

        if (s_client && s_connected && s_log_queue) {
            log_item_t item;
            while (xQueueReceive(s_log_queue, &item, 0) == pdTRUE) {
                char payload[160];
                char ts_iso[24] = {0};
                bool ts_ok = s_time_synced && format_time_iso8601(ts_iso, sizeof(ts_iso), item.ts_epoch);
                int len = snprintf(payload, sizeof(payload),
                                   "{\"ts_ms\":%lld,\"ts_iso\":\"%s\",\"ts_valid\":%s,"
                                   "\"event\":\"%s\",\"water_cm\":%.2f}",
                                   (long long)item.ts_ms, ts_ok ? ts_iso : "",
                                   ts_ok ? "true" : "false", item.event, item.water_level_cm);
                if (len > 0) {
                    if (s_mqtt_lock && xSemaphoreTake(s_mqtt_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
                        esp_mqtt_client_publish(s_client, MQTT_LOG_TOPIC, payload, 0, 1, 0);
                        xSemaphoreGive(s_mqtt_lock);
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void mqtt_task_start(const char *broker_uri)
{
    if (s_mqtt_task_handle) {
        return;
    }

    if (!s_mqtt_lock) {
        s_mqtt_lock = xSemaphoreCreateMutex();
    }
    if (!s_log_queue) {
        s_log_queue = xQueueCreate(LOG_QUEUE_LEN, sizeof(log_item_t));
    }

    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init_nonblocking();

    const char *uri = broker_uri ? broker_uri : MQTT_DEFAULT_BROKER_URI;
    xTaskCreate(mqtt_task, "mqtt_task", 4096, (void *)uri, 6, &s_mqtt_task_handle);
}

void mqtt_publish_status(bool pump_on, bool flooded, float water_level_cm)
{
    s_last_pump_on = pump_on;
    s_last_flooded = flooded;
    s_last_water_level = water_level_cm;
    s_pending_status = true;

    if (!s_client || !s_connected) {
        return;
    }

    char payload[128];
    int64_t ts_ms = esp_timer_get_time() / 1000;
    time_t now = time(NULL);
    char ts_iso[24] = {0};
    bool ts_ok = s_time_synced && format_time_iso8601(ts_iso, sizeof(ts_iso), now);
    int len = snprintf(payload, sizeof(payload),
                       "{\"ts_ms\":%lld,\"ts_iso\":\"%s\",\"ts_valid\":%s,"
                       "\"pump\":%s,\"flooded\":%s,\"water_cm\":%.2f}",
                       (long long)ts_ms,
                       ts_ok ? ts_iso : "",
                       ts_ok ? "true" : "false",
                       pump_on ? "true" : "false",
                       flooded ? "true" : "false",
                       water_level_cm);

    if (len <= 0) {
        return;
    }

    if (s_mqtt_lock && xSemaphoreTake(s_mqtt_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
        esp_mqtt_client_publish(s_client, MQTT_STATUS_TOPIC, payload, 0, 1, 0);
        xSemaphoreGive(s_mqtt_lock);
    }
}

void mqtt_publish_log(const char *event, float water_level_cm)
{
    if (!event) {
        return;
    }

    log_item_t item;
    item.ts_ms = esp_timer_get_time() / 1000;
    item.ts_epoch = time(NULL);
    item.water_level_cm = water_level_cm;
    memset(item.event, 0, sizeof(item.event));
    strncpy(item.event, event, sizeof(item.event) - 1);

    if (s_client && s_connected) {
        char payload[160];
        char ts_iso[24] = {0};
        bool ts_ok = s_time_synced && format_time_iso8601(ts_iso, sizeof(ts_iso), item.ts_epoch);
        int len = snprintf(payload, sizeof(payload),
                           "{\"ts_ms\":%lld,\"ts_iso\":\"%s\",\"ts_valid\":%s,"
                           "\"event\":\"%s\",\"water_cm\":%.2f}",
                           (long long)item.ts_ms, ts_ok ? ts_iso : "",
                           ts_ok ? "true" : "false", item.event, item.water_level_cm);
        if (len > 0) {
            if (s_mqtt_lock && xSemaphoreTake(s_mqtt_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
                esp_mqtt_client_publish(s_client, MQTT_LOG_TOPIC, payload, 0, 1, 0);
                xSemaphoreGive(s_mqtt_lock);
                return;
            }
        }
    }

    if (s_log_queue) {
        // Queue for later publish if offline; drop if queue is full.
        xQueueSend(s_log_queue, &item, 0);
    }
}
