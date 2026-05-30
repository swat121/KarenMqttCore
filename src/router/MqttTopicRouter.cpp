#include "MqttTopicRouter.h"
#include "../core/KarenMqttDebug.h"

#include <string.h>

MqttTopicRouter::MqttTopicRouter()
    : count_(0)
{
    for (size_t i = 0; i < KAREN_MQTT_MAX_HANDLERS; ++i) {
        entries_[i].topic[0] = '\0';
        entries_[i].active   = false;
    }
}

bool MqttTopicRouter::on(const char* topic, MqttHandler handler)
{
    if (!handler) {
        KAREN_MQTT_LOG("on() rejected: null handler for topic '%s'", topic);
        return false;
    }

    // Reuse an existing inactive slot for the same topic first.
    for (size_t i = 0; i < KAREN_MQTT_MAX_HANDLERS; ++i) {
        if (entries_[i].active && strcmp(entries_[i].topic, topic) == 0) {
            entries_[i].handler = handler;
            return true;
        }
    }

    // Find a free (inactive) slot.
    for (size_t i = 0; i < KAREN_MQTT_MAX_HANDLERS; ++i) {
        if (!entries_[i].active) {
            strncpy(entries_[i].topic, topic, KAREN_MQTT_TOPIC_BUFFER - 1);
            entries_[i].topic[KAREN_MQTT_TOPIC_BUFFER - 1] = '\0';
            entries_[i].handler = handler;
            entries_[i].active  = true;
            ++count_;
            return true;
        }
    }

    KAREN_MQTT_LOG("on() overflow: cannot register topic '%s', capacity %d reached", topic, (int)KAREN_MQTT_MAX_HANDLERS);
    return false;
}

void MqttTopicRouter::off(const char* topic)
{
    for (size_t i = 0; i < KAREN_MQTT_MAX_HANDLERS; ++i) {
        if (entries_[i].active && strcmp(entries_[i].topic, topic) == 0) {
            entries_[i].active = false;
            if (count_ > 0) --count_;
            return;
        }
    }
}

void MqttTopicRouter::clear()
{
    for (size_t i = 0; i < KAREN_MQTT_MAX_HANDLERS; ++i) {
        entries_[i].active = false;
    }
    count_ = 0;
}

bool MqttTopicRouter::dispatch(const char* topic, const uint8_t* payload, size_t len) const
{
    for (size_t i = 0; i < KAREN_MQTT_MAX_HANDLERS; ++i) {
        if (entries_[i].active && strcmp(entries_[i].topic, topic) == 0) {
            entries_[i].handler(topic, payload, len);
            return true;
        }
    }
    KAREN_MQTT_LOG("dispatch(): no handler for topic '%s'", topic);
    return false;
}

size_t MqttTopicRouter::size() const
{
    return count_;
}
