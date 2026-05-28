/**
 * src/main.cpp — KarenMqttCore integration sketch
 *
 * Demonstrates the canonical Karen firmware stack on ESP32-C3:
 *   - KarenConnection (WifiConnectionManager) — WiFi with web UI + button mode switch
 *   - KarenErrorHandler — LED blink patterns for runtime errors
 *   - KarenMqttCore — MQTT transport, topic router and JSON ACK helpers
 *
 * Wiring (WeAct ESP32-C3 core board):
 *   - LED:    GPIO 8 (on-board, active LOW)
 *   - Button: GPIO 9 (on-board BOOT)
 *
 * Flow:
 *   1. KarenErrorHandler is configured first so any later failure can blink an error.
 *   2. KarenConnection brings up WiFi. Its onConnected callback starts MQTT,
 *      onDisconnected stops it, onError raises ERR_WIFI.
 *   3. Once MQTT is connected we subscribe to a "sensor" command and publish "online".
 *   4. Incoming command -> publishAck with a dummy sensor reading.
 *
 * NOTE: This file is excluded from the library export via
 *       library.json -> build.srcFilter (see "-<main.cpp>"). It is here only
 *       for local development / smoke testing of the library.
 */

#include <Arduino.h>
#include <WiFiClient.h>
#include <KarenConnectionLib.h>
#include "KarenErrorHandler/KarenErrorHandler.h"
#include "KarenMqttCore.h"

// ---------------------------------------------------------------------------
// Configuration — adjust before flashing
// ---------------------------------------------------------------------------
static const char*    kDeviceId = "demo-device-01";
static const char*    kClientId = "karen-demo-01";
static const char*    kBroker   = "broker.hivemq.com";
static const uint16_t kPort     = 1883;

static constexpr uint8_t LED_PIN    = 8;
static constexpr uint8_t BUTTON_PIN = 9;

// Error codes -> LED blink counts
static constexpr uint8_t ERR_WIFI = 1;  // 2 blinks
static constexpr uint8_t ERR_MQTT = 2;  // 3 blinks

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
KarenErrorHandler  errors;
KarenConnection    wifi;
WiFiClient         netClient;
KarenMqttClient    mqtt(netClient);
KarenMqttTopics    topics(kDeviceId);

// Long-lived storage for the availability topic — MqttConfig keeps a shallow
// const char* pointer (see KarenMqttConfig.h lifetime note).
static String availTopic;

// State machine flags
static volatile bool wifiUp        = false;
static bool          mqttStarted   = false;
static bool          mqttSubscribed = false;

// ---------------------------------------------------------------------------
// MQTT command handler — command-driven sensor pattern
// ---------------------------------------------------------------------------
void handleSensorCommand(const char* /*topic*/, const uint8_t* payload, size_t len)
{
    JsonDocument req;
    if (deserializeJson(req, payload, len)) {
        KAREN_MQTT_LOG("sensor cmd: JSON parse error");
        return;
    }
    const char* requestId = req["requestId"] | "";

    JsonDocument respDoc;
    JsonObject resp = respDoc.to<JsonObject>();
    resp["temperatureC"] = 21.5f;
    mqtt.publishAck(topics.event("sensor").c_str(), requestId, true, resp);
}

// ---------------------------------------------------------------------------
// MQTT lifecycle helpers
// ---------------------------------------------------------------------------
void startMqtt()
{
    if (mqttStarted) return;

    availTopic = topics.availability();

    MqttConfig cfg;
    cfg.host        = kBroker;
    cfg.port        = kPort;
    cfg.clientId    = kClientId;
    cfg.deviceId    = kDeviceId;
    cfg.lwt.topic   = availTopic.c_str();
    cfg.lwt.payload = "offline";
    cfg.lwt.qos     = 1;
    cfg.lwt.retain  = true;
    cfg.hasLwt      = true;

    mqtt.begin(cfg);
    mqttStarted    = true;
    mqttSubscribed = false;
    Serial.println("[main] MQTT started");
}

void stopMqtt()
{
    // KarenMqttClient v0.1 has no explicit disconnect; simply stop driving loop().
    mqttStarted    = false;
    mqttSubscribed = false;
}

// ---------------------------------------------------------------------------
// setup()
// ---------------------------------------------------------------------------
void setup()
{
    Serial.begin(115200);

    // 1. Error handler comes up first — everything else can report through it.
    errors
        .setLedPin(LED_PIN)
        .setDebugMode(true)
        .registerError(ERR_WIFI, 2)
        .registerError(ERR_MQTT, 3)
        .begin();

    // 2. WiFi with web UI + button-controlled mode switch.
    wifi
        .setDebugMode(true)
        .setButtonPin(BUTTON_PIN, 3000)
        .enableWebUI(80)
        .onConnected([]() {
            Serial.println("[wifi] connected");
            wifiUp = true;
            errors.clearActive();
        })
        .onDisconnected([]() {
            Serial.println("[wifi] disconnected");
            wifiUp = false;
            stopMqtt();
        })
        .onError([]() {
            Serial.println("[wifi] error");
            errors.reportError(ERR_WIFI);
        })
        .begin();

    Serial.println("[main] setup done");
}

// ---------------------------------------------------------------------------
// loop()
// ---------------------------------------------------------------------------
void loop()
{
    wifi.loop();

    if (wifiUp) {
        if (!mqttStarted) {
            startMqtt();
        }

        bool connected = mqtt.loop();

        if (connected && !mqttSubscribed) {
            mqtt.on(topics.command("sensor").c_str(), handleSensorCommand);
            mqtt.publish(availTopic.c_str(), "online", /*retain=*/true);
            mqttSubscribed = true;
            Serial.println("[mqtt] subscribed and announced online");
        } else if (!connected && mqttSubscribed) {
            // Lost broker — flag and let next loop reconnect via PubSubClient.
            mqttSubscribed = false;
            errors.reportError(ERR_MQTT);
        }
    }
}
