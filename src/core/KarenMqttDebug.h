#pragma once

// ---------------------------------------------------------------------------
// KAREN_MQTT_LOG — debug logging macro
// Enable with -DKAREN_MQTT_DEBUG=1 in build_flags.
// ---------------------------------------------------------------------------

#ifdef KAREN_MQTT_DEBUG
#include <Arduino.h>
#define KAREN_MQTT_LOG(fmt, ...) \
    Serial.printf("[KarenMqtt] " fmt "\n", ##__VA_ARGS__)
#else
#define KAREN_MQTT_LOG(fmt, ...) \
    do {} while (0)
#endif
