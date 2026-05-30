#include "KarenMqttTopics.h"
#include <WiFi.h>

String KarenMqttClientId::fromMac(const char* prefix)
{
    String mac = WiFi.macAddress();
    // Remove colons: "AA:BB:CC:DD:EE:FF" -> "AABBCCDDEEFF"
    mac.replace(":", "");
    return String(prefix) + "_" + mac;
}
