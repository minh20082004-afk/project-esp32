#ifndef MQTT_TASK_H
#define MQTT_TASK_H

#include <stdbool.h>

// Default broker URI. Override by passing a non-NULL value to mqtt_task_start().
#ifndef MQTT_DEFAULT_BROKER_URI
#define MQTT_DEFAULT_BROKER_URI "mqtt://broker.emqx.io:1883"
#endif

// Default topics. Override with compile-time defines if needed.
#ifndef MQTT_STATUS_TOPIC
#define MQTT_STATUS_TOPIC "node2/status"
#endif

#ifndef MQTT_LOG_TOPIC
#define MQTT_LOG_TOPIC "node2/log"
#endif

// Start MQTT client and non-blocking Wi-Fi initialization.
void mqtt_task_start(const char *broker_uri);

// Publish current status (pump, valve, flooded, water level).
void mqtt_publish_status(bool pump_on, bool valve_on, bool flooded, float water_level_cm);

// Publish a log event when threshold is exceeded.
void mqtt_publish_log(const char *event, bool pump_on, bool valve_on, bool flooded, float water_level_cm);

#endif
