#include "MqttTransport.h"
#include "KarenMqttDebug.h"

#include <Arduino.h>

#ifdef KAREN_MQTT_TLS
#include <WiFiClientSecure.h>
#endif

MqttTransport::MqttTransport(Client& netClient)
    : pubsub_(netClient)
    , lastReconnectAttemptMs_(0)
{
}

void MqttTransport::begin(const MqttConfig& cfg)
{
    cfg_ = cfg;

    pubsub_.setServer(cfg_.host, cfg_.port);
    pubsub_.setKeepAlive(cfg_.keepAliveSec);
    // PubSubClient default buffer is 256 bytes; override to support larger payloads.
    // Users can increase further via -DKAREN_MQTT_BUFFER_SIZE=2048 if needed.
    pubsub_.setBufferSize(KAREN_MQTT_BUFFER_SIZE);

    if (messageCallback_) {
        pubsub_.setCallback([this](char* topic, uint8_t* payload, unsigned int len) {
            messageCallback_(topic, payload, len);
        });
    }
}

bool MqttTransport::connect()
{
    KAREN_MQTT_LOG("connecting to %s:%d as %s", cfg_.host, (int)cfg_.port, cfg_.clientId);

    bool ok = false;

    if (cfg_.hasLwt) {
        if (cfg_.user && cfg_.pass) {
            ok = pubsub_.connect(cfg_.clientId, cfg_.user, cfg_.pass,
                                 cfg_.lwt.topic, cfg_.lwt.qos,
                                 cfg_.lwt.retain, cfg_.lwt.payload);
        } else {
            ok = pubsub_.connect(cfg_.clientId,
                                 cfg_.lwt.topic, cfg_.lwt.qos,
                                 cfg_.lwt.retain, cfg_.lwt.payload);
        }
    } else {
        if (cfg_.user && cfg_.pass) {
            ok = pubsub_.connect(cfg_.clientId, cfg_.user, cfg_.pass);
        } else {
            ok = pubsub_.connect(cfg_.clientId);
        }
    }

    if (ok) {
        KAREN_MQTT_LOG("connected");
    } else {
        KAREN_MQTT_LOG("connect failed, rc=%d", pubsub_.state());
    }

    return ok;
}

void MqttTransport::disconnect()
{
    pubsub_.disconnect();
}

bool MqttTransport::isConnected()
{
    return pubsub_.connected();
}

bool MqttTransport::loop()
{
    if (!pubsub_.connected()) {
        uint32_t now = (uint32_t)millis();
        if (now - lastReconnectAttemptMs_ >= cfg_.reconnectIntervalMs) {
            lastReconnectAttemptMs_ = now;
            connect();
        }
    }

    if (pubsub_.connected()) {
        pubsub_.loop();
        return true;
    }

    return false;
}

bool MqttTransport::publish(const char* topic, const uint8_t* payload, size_t len, bool retain)
{
    if (!pubsub_.connected()) {
        KAREN_MQTT_LOG("publish skipped: not connected");
        return false;
    }
    return pubsub_.publish(topic, payload, (unsigned int)len, retain);
}

bool MqttTransport::subscribe(const char* topic, uint8_t qos)
{
    if (!pubsub_.connected()) {
        KAREN_MQTT_LOG("subscribe skipped: not connected (topic '%s')", topic);
        return false;
    }
    return pubsub_.subscribe(topic, qos);
}

bool MqttTransport::unsubscribe(const char* topic)
{
    if (!pubsub_.connected()) {
        return false;
    }
    return pubsub_.unsubscribe(topic);
}

void MqttTransport::setMessageCallback(std::function<void(char*, uint8_t*, unsigned int)> cb)
{
    messageCallback_ = cb;
}

#ifdef KAREN_MQTT_TLS
void MqttTransport::setCaCertificate(const char* pemCa)
{
    // dynamic_cast requires RTTI. ESP32 Arduino core enables RTTI by default.
    // If RTTI is disabled in the build, replace with a separate
    // MqttTransport(WiFiClientSecure&) constructor under #if KAREN_MQTT_TLS.
    Client* rawClient = pubsub_.getClient();
    WiFiClientSecure* secure = dynamic_cast<WiFiClientSecure*>(rawClient);
    if (!secure) {
        KAREN_MQTT_LOG("setCaCertificate: netClient is not WiFiClientSecure — TLS skipped");
        return;
    }
    secure->setCACert(pemCa);
    KAREN_MQTT_LOG("CA certificate set");
}
#endif
