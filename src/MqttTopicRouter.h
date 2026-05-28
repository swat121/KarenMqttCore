#pragma once

#include <functional>
#include <stddef.h>
#include <stdint.h>
#include "KarenMqttConfig.h"

#ifdef KAREN_MQTT_WILDCARDS
// Wildcards are not implemented in v0.1. Fail fast so the developer notices
// rather than silently getting exact-match-only behavior.
static_assert(false, "KAREN_MQTT_WILDCARDS is not implemented in v0.1. Remove the flag to use exact-match routing.");
#endif

using MqttHandler = std::function<void(const char* topic, const uint8_t* payload, size_t len)>;

// ---------------------------------------------------------------------------
// MqttTopicRouter — fixed-size exact-match topic dispatcher
//
// Stores up to KAREN_MQTT_MAX_HANDLERS topic->handler pairs in a flat array
// (no heap allocation). Linear scan is acceptable at capacity 8.
// ---------------------------------------------------------------------------

class MqttTopicRouter
{
public:
    MqttTopicRouter();

    // Register handler for topic. Returns false if capacity is exceeded.
    bool on(const char* topic, MqttHandler handler);

    // Unregister handler for topic (marks slot inactive).
    void off(const char* topic);

    // Unregister all handlers.
    void clear();

    // Dispatch incoming message to matching handler.
    // Returns true if a handler was found and invoked.
    bool dispatch(const char* topic, const uint8_t* payload, size_t len) const;

    size_t size() const;

    static constexpr size_t capacity() { return KAREN_MQTT_MAX_HANDLERS; }

private:
    struct Entry
    {
        // Topic buffer length controlled by KAREN_MQTT_TOPIC_BUFFER (default 64).
        // Increase via -DKAREN_MQTT_TOPIC_BUFFER=128 if your topics exceed 64 bytes.
        char        topic[KAREN_MQTT_TOPIC_BUFFER];
        MqttHandler handler;
        bool        active;
    };

    Entry  entries_[KAREN_MQTT_MAX_HANDLERS];
    size_t count_;
};
