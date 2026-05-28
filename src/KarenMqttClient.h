#pragma once

#include "KarenMqttConfig.h"
#include "MqttTransport.h"
#include "MqttTopicRouter.h"

#if KAREN_MQTT_JSON_ACK
#include <ArduinoJson.h>
#endif

// ---------------------------------------------------------------------------
// KarenMqttClient — public facade combining transport + topic router
//
// Typical usage:
//   WiFiClient net;
//   KarenMqttClient mqtt(net);
//   mqtt.begin(cfg);
//   mqtt.on(topics.command("sensor").c_str(), handler);
//   // in loop():
//   mqtt.loop();
// ---------------------------------------------------------------------------

class KarenMqttClient
{
public:
    explicit KarenMqttClient(Client& netClient);

    void begin(const MqttConfig& cfg);

    // Call from main loop. Returns true if connected.
    bool loop();

    // Register topic handler and subscribe. Returns false on capacity overflow.
    // Note (v0.1 limitation): subscriptions are NOT re-issued after reconnect.
    // Call on() again inside an onConnect callback or after loop() returns true
    // for the first time.
    bool on(const char* topic, MqttHandler handler);

    // Unregister and unsubscribe from topic.
    void off(const char* topic);

    // Publish string payload.
    bool publish(const char* topic, const char* payload, bool retain = false);

    // Publish binary payload.
    bool publish(const char* topic, const uint8_t* payload, size_t len, bool retain = false);

    // Non-const due to PubSubClient::connected() being non-const.
    bool isConnected();

    // Access underlying transport (e.g., for TLS setup).
    MqttTransport& transport();

    // Access underlying router (e.g., to query size()).
    MqttTopicRouter& router();

#if KAREN_MQTT_JSON_ACK
    // Publish JSON ACK: { "requestId": "...", "ok": true|false, "message": "..." }
    bool publishAck(const char* topic, const char* requestId, bool ok, const char* message = nullptr);

    // Publish JSON ACK with additional payload merged in.
    bool publishAck(const char* topic, const char* requestId, bool ok, const JsonObject& payload);
#endif

private:
    MqttTransport   transport_;
    MqttTopicRouter router_;
};
