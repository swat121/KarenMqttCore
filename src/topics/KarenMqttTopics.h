#pragma once

#include <Arduino.h>

// ---------------------------------------------------------------------------
// KarenMqttTopics — topic conventions for Karen ecosystem
//
//   esp/<deviceId>/<domain>/(command|event)   — per-device request/response
//   esp/<deviceId>/connect                    — LWT + ONLINE/OFFLINE
//   esp/<deviceId>/<leaf>                     — free-form one-level topic
//   esp/health                                — broadcast heartbeat
//
// Note: `health()` returns a topic WITHOUT deviceId — heartbeat is a single
// shared channel; deviceId is carried in the payload.
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

    // esp/<deviceId>/connect — LWT + ONLINE/OFFLINE state, retain=true
    String connect() const
    {
        return base_ + "/connect";
    }

    // esp/<deviceId>/<leaf> — free-form, used for legacy/one-off topics
    // such as esp/<id>/sensor (push pattern) or esp/<id>/error.
    String custom(const char* leaf) const
    {
        return base_ + "/" + leaf;
    }

    // esp/health — single broadcast channel for all devices. NOTE: no deviceId
    // in the path; the publisher must include deviceId in the JSON payload.
    static String health()
    {
        return String("esp/health");
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
