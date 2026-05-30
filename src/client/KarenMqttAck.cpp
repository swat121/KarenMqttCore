#include "KarenMqttClient.h"
#include "../core/KarenMqttDebug.h"

#if KAREN_MQTT_JSON_ACK

#include <ArduinoJson.h>

// Serialized ACK buffer cap: requestId(~36) + ok(5) + message(~64) + small overhead.
// 256 bytes is enough for most cases; JsonObject overload callers should keep payloads
// under this cap.
static constexpr size_t kAckBufSize = 256;

bool KarenMqttClient::publishAck(const char* topic, const char* requestId, bool ok, const char* message)
{
    JsonDocument doc;
    doc["requestId"] = requestId;
    doc["ok"]        = ok;
    if (message) {
        doc["message"] = message;
    }

    // serializeJson (compact, not pretty) — avoids unnecessary whitespace on
    // constrained MQTT payloads. See ArduinoJson docs: serializeJsonPretty adds ~30%.
    char buf[kAckBufSize];
    if (measureJson(doc) >= sizeof(buf)) {
        KAREN_MQTT_LOG("publishAck: serialized ACK exceeds %d bytes (requestId too long?)", (int)kAckBufSize);
        return false;
    }
    size_t len = serializeJson(doc, buf, sizeof(buf));
    return publish(topic, reinterpret_cast<const uint8_t*>(buf), len);
}

bool KarenMqttClient::publishAck(const char* topic, const char* requestId, bool ok, const JsonObject& payload)
{
    JsonDocument doc;
    doc["requestId"]        = requestId;
    doc["ok"]               = ok;
    JsonObject payloadCopy  = doc["payload"].to<JsonObject>();
    for (JsonPair kv : payload) {
        payloadCopy[kv.key()] = kv.value();
    }

    char buf[kAckBufSize];
    if (measureJson(doc) >= sizeof(buf)) {
        KAREN_MQTT_LOG("publishAck: serialized ACK exceeds %d bytes (payload too large)", (int)kAckBufSize);
        return false;
    }
    size_t len = serializeJson(doc, buf, sizeof(buf));
    return publish(topic, reinterpret_cast<const uint8_t*>(buf), len);
}

#endif // KAREN_MQTT_JSON_ACK
