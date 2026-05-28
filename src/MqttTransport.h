#pragma once

#include <functional>
#include <stdint.h>
#include <PubSubClient.h>
#include "KarenMqttConfig.h"

#ifdef KAREN_MQTT_TLS
#include <WiFiClientSecure.h>
#endif

// ---------------------------------------------------------------------------
// MqttTransport — thin MQTT transport over PubSubClient
//
// Owns the PubSubClient instance. Handles connect/reconnect/loop/publish/
// subscribe. No topic routing or business logic here.
// ---------------------------------------------------------------------------

class MqttTransport
{
public:
    explicit MqttTransport(Client& netClient);

    void begin(const MqttConfig& cfg);

    // Call from main loop. Returns true if connected after the call.
    bool loop();

    bool connect();
    void disconnect();

    // Non-const due to PubSubClient::connected() being non-const.
    bool isConnected();

    bool publish(const char* topic, const uint8_t* payload, size_t len, bool retain = false);
    bool subscribe(const char* topic, uint8_t qos = 0);
    bool unsubscribe(const char* topic);

    void setMessageCallback(std::function<void(char*, uint8_t*, unsigned int)> cb);

#ifdef KAREN_MQTT_TLS
    // Provides the CA certificate PEM string to the underlying WiFiClientSecure.
    // Must be called before begin(). Logs an error if netClient is not WiFiClientSecure.
    void setCaCertificate(const char* pemCa);
#endif

private:
    PubSubClient pubsub_;
    MqttConfig   cfg_;
    uint32_t     lastReconnectAttemptMs_;

    std::function<void(char*, uint8_t*, unsigned int)> messageCallback_;
};
