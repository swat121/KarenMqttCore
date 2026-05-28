#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
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

// availTopic is kept as a global String so its c_str() pointer remains valid
// for the lifetime of the MqttConfig / mqtt object.
// MqttConfig and MqttLwt hold shallow const char* pointers — see KarenMqttConfig.h
// lifetime note.
static String      availTopic;

// Tracks whether we have already subscribed (re-subscribe on reconnect pattern).
static bool        subscribed = false;

// ---------------------------------------------------------------------------
// Command handler — command-driven sensor pattern
// ---------------------------------------------------------------------------
void handleSensorCommand(const char* /*topic*/, const uint8_t* payload, size_t len)
{
    StaticJsonDocument<256> req;
    DeserializationError err = deserializeJson(req, payload, len);
    if (err) {
        KAREN_MQTT_LOG("JSON parse error: %s", err.c_str());
        return;
    }

    const char* requestId = req["requestId"] | "";

    // Build dummy sensor reading — replace with real sensor read in firmware.
    StaticJsonDocument<128> respDoc;
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

    // Store the availability topic in a long-lived String so c_str() stays valid.
    availTopic = topics.availability();

    MqttConfig cfg;
    cfg.host     = kBroker;
    cfg.port     = kPort;
    cfg.clientId = kClientId;
    cfg.deviceId = kDeviceId;
    cfg.lwt.topic   = availTopic.c_str();
    cfg.lwt.payload = "offline";
    cfg.lwt.qos     = 1;
    cfg.lwt.retain  = true;
    cfg.hasLwt   = true;

    mqtt.begin(cfg);

    // Note: do NOT call mqtt.on() here — subscriptions must be issued after
    // the MQTT connection is established. See loop() for the connect-transition
    // pattern. In v0.2 this will be handled via an onConnect callback.
}

void loop()
{
    bool connected = mqtt.loop();

    // Re-subscribe on every transition from disconnected → connected.
    // v0.1 pattern: user is responsible for re-issuing subscriptions after reconnect.
    // v0.2 will provide an onConnect callback to automate this.
    if (connected && !subscribed) {
        mqtt.on(topics.command("sensor").c_str(), handleSensorCommand);
        // Announce device online after successful subscribe.
        mqtt.publish(availTopic.c_str(), "online", /*retain=*/true);
        subscribed = true;
        KAREN_MQTT_LOG("subscribed and announced online");
    } else if (!connected && subscribed) {
        subscribed = false;
    }
}
