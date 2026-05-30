#include "KarenMqttClient.h"
#include "../core/KarenMqttDebug.h"

KarenMqttClient::KarenMqttClient(Client& netClient)
    : transport_(netClient)
{
}

void KarenMqttClient::begin(const MqttConfig& cfg)
{
    // Wire PubSubClient callback -> router dispatch before handing to transport.
    transport_.setMessageCallback([this](char* topic, uint8_t* payload, unsigned int len) {
        router_.dispatch(topic, payload, (size_t)len);
    });
    transport_.begin(cfg);
}

bool KarenMqttClient::loop()
{
    return transport_.loop();
}

bool KarenMqttClient::on(const char* topic, MqttHandler handler)
{
    // Subscribe unconditionally; transport logs if not yet connected.
    transport_.subscribe(topic);
    return router_.on(topic, handler);
}

void KarenMqttClient::off(const char* topic)
{
    transport_.unsubscribe(topic);
    router_.off(topic);
}

bool KarenMqttClient::publish(const char* topic, const char* payload, bool retain)
{
    return transport_.publish(
        topic,
        reinterpret_cast<const uint8_t*>(payload),
        strlen(payload),
        retain
    );
}

bool KarenMqttClient::publish(const char* topic, const uint8_t* payload, size_t len, bool retain)
{
    return transport_.publish(topic, payload, len, retain);
}

bool KarenMqttClient::isConnected()
{
    return transport_.isConnected();
}

MqttTransport& KarenMqttClient::transport()
{
    return transport_;
}

MqttTopicRouter& KarenMqttClient::router()
{
    return router_;
}
