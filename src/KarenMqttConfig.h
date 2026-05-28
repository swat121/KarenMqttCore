#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// Compile-time feature flags — override via build_flags in platformio.ini
// ---------------------------------------------------------------------------

#ifndef KAREN_MQTT_BUFFER_SIZE
#define KAREN_MQTT_BUFFER_SIZE 1024
#endif

#ifndef KAREN_MQTT_MAX_HANDLERS
#define KAREN_MQTT_MAX_HANDLERS 8
#endif

#ifndef KAREN_MQTT_JSON_ACK
#define KAREN_MQTT_JSON_ACK 1
#endif

// KAREN_MQTT_TLS — opt-in, undefined by default
// KAREN_MQTT_WILDCARDS — opt-in, undefined by default; causes compile-error (fail-fast)
// KAREN_MQTT_DEBUG — opt-in, undefined by default

#ifndef KAREN_MQTT_DEFAULT_KEEPALIVE_SEC
#define KAREN_MQTT_DEFAULT_KEEPALIVE_SEC 60
#endif

#ifndef KAREN_MQTT_DEFAULT_RECONNECT_INTERVAL_MS
#define KAREN_MQTT_DEFAULT_RECONNECT_INTERVAL_MS 5000
#endif

// Maximum topic string length in MqttTopicRouter entries.
// Increase via -DKAREN_MQTT_TOPIC_BUFFER=128 if your topics exceed 64 bytes.
#ifndef KAREN_MQTT_TOPIC_BUFFER
#define KAREN_MQTT_TOPIC_BUFFER 64
#endif

// ---------------------------------------------------------------------------
// Core structs
// ---------------------------------------------------------------------------

namespace karen {
namespace mqtt {

// Lifetime requirement: all const char* pointers in MqttLwt must remain valid
// for at least as long as the KarenMqttClient that uses them.
// MqttLwt (and MqttConfig) are copied shallow — the pointed-to strings are NOT
// deep-copied. Callers must keep the backing storage alive.
struct MqttLwt
{
    const char* topic   = nullptr;
    const char* payload = nullptr;
    uint8_t     qos     = 0;
    bool        retain  = false;
};

// Lifetime requirement: all const char* pointers in MqttConfig must remain valid
// for at least as long as the KarenMqttClient that uses them.
// MqttConfig is copied shallow — the pointed-to strings are NOT deep-copied.
// Callers must keep the backing storage alive (e.g., use global/static Strings
// and pass .c_str() rather than temporaries).
struct MqttConfig
{
    const char* host                = nullptr;
    uint16_t    port                = 1883;
    const char* clientId            = nullptr;
    const char* deviceId            = nullptr;
    uint16_t    keepAliveSec        = KAREN_MQTT_DEFAULT_KEEPALIVE_SEC;
    uint32_t    reconnectIntervalMs = KAREN_MQTT_DEFAULT_RECONNECT_INTERVAL_MS;
    const char* user                = nullptr;
    const char* pass                = nullptr;
    MqttLwt     lwt                 = {};
    bool        hasLwt              = false;
};

} // namespace mqtt
} // namespace karen

// Bring core types into global scope for Arduino-style convenience
using karen::mqtt::MqttConfig;
using karen::mqtt::MqttLwt;
