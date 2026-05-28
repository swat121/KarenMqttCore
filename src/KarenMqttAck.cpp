#include "KarenMqttClient.h"
#include "KarenMqttDebug.h"

#if KAREN_MQTT_JSON_ACK

#include <ArduinoJson.h>

// ACK document size: requestId(~36) + ok(5) + message(~64) + small overhead = 256 is sufficient
// for most cases. Callers needing larger payloads should use the JsonObject overload.
static constexpr size_t kAckDocSize = 256;

bool KarenMqttClient::publishAck(const char* topic, const char* requestId, bool ok, const char* message)
{
    StaticJsonDocument<kAckDocSize> doc;
    doc["requestId"] = requestId;
    doc["ok"]        = ok;
    if (message) {
        doc["message"] = message;
    }

    if (doc.overflowed()) {
        KAREN_MQTT_LOG("publishAck: JSON document overflowed (requestId too long?)");
        return false;
    }

    // serializeJson (compact, not pretty) — avoids unnecessary whitespace on
    // constrained MQTT payloads. See ArduinoJson docs: serializeJsonPretty adds ~30%.
    char buf[kAckDocSize];
    size_t len = serializeJson(doc, buf, sizeof(buf));
    return publish(topic, reinterpret_cast<const uint8_t*>(buf), len);
}

bool KarenMqttClient::publishAck(const char* topic, const char* requestId, bool ok, const JsonObject& payload)
{
    StaticJsonDocument<kAckDocSize> doc;
    doc["requestId"]        = requestId;
    doc["ok"]               = ok;
    JsonObject payloadCopy  = doc.createNestedObject("payload");
    for (JsonPair kv : payload) {
        payloadCopy[kv.key()] = kv.value();
    }

    if (doc.overflowed()) {
        KAREN_MQTT_LOG("publishAck: JSON document overflowed (payload too large for kAckDocSize=%d)", (int)kAckDocSize);
        return false;
    }

    char buf[kAckDocSize];
    size_t len = serializeJson(doc, buf, sizeof(buf));
    return publish(topic, reinterpret_cast<const uint8_t*>(buf), len);
}

#endif // KAREN_MQTT_JSON_ACK
