#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// KarenMqttTopics — topic convention: esp/<deviceId>/<domain>/(command|event)
// ---------------------------------------------------------------------------

class KarenMqttTopics
{
public:
    explicit KarenMqttTopics(const char* deviceId)
        : base_(String("esp/") + deviceId)
    {
    }

    // esp/<deviceId>/<domain>/command
    String command(const char* domain) const
    {
        return base_ + "/" + domain + "/command";
    }

    // esp/<deviceId>/<domain>/event
    String event(const char* domain) const
    {
        return base_ + "/" + domain + "/event";
    }

    // esp/<deviceId>/availability
    String availability() const
    {
        return base_ + "/availability";
    }

    const String& base() const { return base_; }

private:
    String base_;
};

// ---------------------------------------------------------------------------
// KarenMqttClientId — generates client IDs from MAC address
// ---------------------------------------------------------------------------

class KarenMqttClientId
{
public:
    // Returns "<prefix>_<MAC>" using WiFi.macAddress(), e.g. "karen_AABBCCDDEEFF"
    static String fromMac(const char* prefix);
};
