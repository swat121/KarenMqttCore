#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <esp_system.h>
#include "KarenMqttCore.h"

// ---------------------------------------------------------------------------
// Configuration — replace with real credentials before flashing
// ---------------------------------------------------------------------------
static const char* kSsid     = "YOUR_SSID";
static const char* kPassword  = "YOUR_PASSWORD";
static const char* kDeviceId  = "demo-device-01";
static const char* kClientId  = "karen-demo-01";
static const char* kBroker    = "broker.hivemq.com";
static const uint16_t kPort   = 1883;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
WiFiClient         netClient;
KarenMqttClient    mqtt(netClient);
KarenMqttTopics    topics(kDeviceId);

// Long-lived storage. MqttConfig keeps shallow const char* pointers —
// see KarenMqttConfig.h lifetime note. Must outlive the client.
static char        bootId[9]  = {0};
static String      connectTopic;
static String      lwtPayload;
static String      macAddress;
static bool        subscribed = false;

// ---------------------------------------------------------------------------
// bootId — see docs/registration-protocol.md §6.2
// ---------------------------------------------------------------------------
static void generateBootId(char out[9])
{
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    uint16_t macTail = (uint16_t)((mac[4] << 8) | mac[5]);
    uint16_t rnd     = (uint16_t)(esp_random() & 0xFFFF);
    snprintf(out, 9, "%04x%04x", macTail, rnd);
}

static String buildConnectJson(const char* status)
{
    JsonDocument doc;
    doc["deviceId"]   = kDeviceId;
    doc["macAddress"] = macAddress;
    doc["status"]     = status;
    doc["bootId"]     = bootId;
    String out;
    serializeJson(doc, out);
    return out;
}

// ---------------------------------------------------------------------------
// Command handler — command-driven sensor pattern
// ---------------------------------------------------------------------------
void handleSensorCommand(const char* /*topic*/, const uint8_t* payload, size_t len)
{
    JsonDocument req;
    DeserializationError err = deserializeJson(req, payload, len);
    if (err) {
        KAREN_MQTT_LOG("JSON parse error: %s", err.c_str());
        return;
    }

    const char* requestId = req["requestId"] | "";

    // Build dummy sensor reading — replace with real sensor read in firmware.
    JsonDocument respDoc;
    JsonObject resp = respDoc.to<JsonObject>();
    resp["temperatureC"] = 21.5f;

    mqtt.publishAck(topics.event("sensor").c_str(), requestId, true, resp);
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setup()
{
    Serial.begin(115200);

    WiFi.begin(kSsid, kPassword);
    KAREN_MQTT_LOG("connecting to WiFi...");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
    }
    KAREN_MQTT_LOG("WiFi connected: %s", WiFi.localIP().toString().c_str());

    macAddress = WiFi.macAddress();
    generateBootId(bootId);

    // Long-lived storage for LWT — must stay valid for the lifetime of mqtt.
    connectTopic = topics.connect();
    lwtPayload   = buildConnectJson("OFFLINE");

    MqttConfig cfg;
    cfg.host        = kBroker;
    cfg.port        = kPort;
    cfg.clientId    = kClientId;
    cfg.deviceId    = kDeviceId;
    cfg.lwt.topic   = connectTopic.c_str();
    cfg.lwt.payload = lwtPayload.c_str();
    cfg.lwt.qos     = 1;
    cfg.lwt.retain  = true;
    cfg.hasLwt      = true;

    mqtt.begin(cfg);
}

void loop()
{
    bool connected = mqtt.loop();

    if (connected && !subscribed) {
        mqtt.on(topics.command("sensor").c_str(), handleSensorCommand);
        // Announce ONLINE on the connect topic (retain=true).
        String onlinePayload = buildConnectJson("ONLINE");
        mqtt.publish(connectTopic.c_str(), onlinePayload.c_str(), /*retain=*/true);
        subscribed = true;
        KAREN_MQTT_LOG("subscribed and announced ONLINE");
    } else if (!connected && subscribed) {
        subscribed = false;
    }
}
