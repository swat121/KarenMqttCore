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
 * Flow (per docs/registration-protocol.md v0.2):
 *   1. Generate bootId from MAC tail + random (collision-resistant per §6.2).
 *   2. KarenErrorHandler comes up first — everything else can report through it.
 *   3. KarenConnection brings up WiFi. onConnected starts MQTT, onError -> ERR_WIFI.
 *   4. MQTT begin() with LWT = JSON {status:"OFFLINE", ...} on esp/<id>/connect (retain=true).
 *   5. After connect we publish JSON {status:"ONLINE", ...} on the same topic (retain=true),
 *      subscribe to sensor/command, and periodically publish to esp/health.
 *
 * NOTE: This file is excluded from the library export via
 *       library.json -> build.srcFilter (see "-<main.cpp>"). It is here only
 *       for local development / smoke testing of the library.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_system.h>
#include <KarenConnectionLib.h>
#include <KarenErrorHandler.h>
#include "KarenMqttCore.h"

// ---------------------------------------------------------------------------
// Configuration — adjust before flashing
// ---------------------------------------------------------------------------
static const char*    kDeviceId = "demo-device-01";
static const char*    kMqttUser = "stark";
static const char*    kMqttPass = "nbxFwDcK*N%%@86@3";
static const char*    kClientId = "karen-demo-01";
static const char*    kBroker   = "9.tcp.eu.ngrok.io";
static const uint16_t kPort     = 29946;

static constexpr uint8_t  LED_PIN              = 8;
static constexpr uint8_t  BUTTON_PIN           = 9;
static constexpr uint32_t HEALTH_INTERVAL_MS   = 30000;

// Error codes -> LED blink counts
static constexpr uint8_t ERR_WIFI = 1;  // 2 blinks
static constexpr uint8_t ERR_MQTT = 2;  // 3 blinks

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
KarenErrorHandler  errors;
KarenConnection    wifi;
WiFiClientSecure   netClient;
KarenMqttClient    mqtt(netClient);
KarenMqttTopics    topics(kDeviceId);

// Long-lived storage. MqttConfig keeps shallow const char* pointers — see
// KarenMqttConfig.h lifetime note. These globals must outlive the client.
static char   bootId[9]     = {0};
static String connectTopic;     // esp/<deviceId>/connect
static String lwtPayload;       // OFFLINE JSON, used as LWT
static String macAddress;

// State machine
static volatile bool wifiUp         = false;
static bool          mqttStarted    = false;
static bool          mqttSubscribed = false;
static uint32_t      lastHealthMs   = 0;

// ---------------------------------------------------------------------------
// bootId — see docs/registration-protocol.md §6.2
// 8 hex chars = mac[4..5] (uniqueness across devices) + random (uniqueness across reboots).
// ---------------------------------------------------------------------------
static void generateBootId(char out[9])
{
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);
    uint16_t macTail = (uint16_t)((mac[4] << 8) | mac[5]);
    uint16_t rnd     = (uint16_t)(esp_random() & 0xFFFF);
    snprintf(out, 9, "%04x%04x", macTail, rnd);
}

// ---------------------------------------------------------------------------
// JSON builders for connect payloads
// ---------------------------------------------------------------------------
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

static void publishHealth()
{
    JsonDocument doc;
    doc["deviceId"]   = kDeviceId;
    doc["macAddress"] = macAddress;
    doc["bootId"]     = bootId;
    doc["uptimeMs"]   = (uint32_t)millis();
    doc["freeHeap"]   = (uint32_t)ESP.getFreeHeap();
    doc["rssi"]       = WiFi.RSSI();

    String out;
    serializeJson(doc, out);
    // esp/health is service-deferred (subscription on Java side is `esp/+/health`),
    // but firmware already publishes to the target topic — see protocol doc §3.
    mqtt.publish(KarenMqttTopics::health().c_str(), out.c_str(), /*retain=*/false);
}

// ---------------------------------------------------------------------------
// MQTT command handler — pull-based sensor pattern (target meta v2.0.0)
// ---------------------------------------------------------------------------
void handleSensorCommand(const char* /*topic*/, const uint8_t* payload, size_t len)
{
    JsonDocument req;
    if (deserializeJson(req, payload, len)) {
        KAREN_MQTT_LOG("sensor cmd: JSON parse error");
        return;
    }
    // MAC-filter (see protocol doc §9.1)
    const char* targetMac = req["macAddress"] | "";
    if (strcmp(targetMac, "*:*:*:*:*:*") != 0 &&
        strcmp(targetMac, macAddress.c_str()) != 0) {
        return;
    }
    const char* requestId = req["requestId"] | "";

    JsonDocument respDoc;
    JsonObject resp = respDoc.to<JsonObject>();
    JsonArray readings = resp["readings"].to<JsonArray>();
    JsonObject r = readings.add<JsonObject>();
    r["address"] = "demo-sensor";
    r["type"]    = "temperature";
    r["value"]   = 21.5f;
    r["unit"]    = "°C";

    mqtt.publishAck(topics.event("sensor").c_str(), requestId, true, resp);
}

// ---------------------------------------------------------------------------
// MQTT lifecycle helpers
// ---------------------------------------------------------------------------
void startMqtt()
{
    if (mqttStarted) return;

    connectTopic = topics.connect();
    lwtPayload   = buildConnectJson("OFFLINE");

    MqttConfig cfg;
    cfg.host        = kBroker;
    cfg.port        = kPort;
    cfg.clientId    = kClientId;
    cfg.user        = kMqttUser;
    cfg.pass        = kMqttPass;
    cfg.deviceId    = kDeviceId;
    cfg.lwt.topic   = connectTopic.c_str();
    cfg.lwt.payload = lwtPayload.c_str();
    cfg.lwt.qos     = 1;
    cfg.lwt.retain  = true;
    cfg.hasLwt      = true;

    mqtt.begin(cfg);
    mqttStarted    = true;
    mqttSubscribed = false;
    Serial.printf("[main] MQTT started, bootId=%s\n", bootId);
}

void stopMqtt()
{
    // KarenMqttClient v0.x has no explicit disconnect; just stop driving loop().
    mqttStarted    = false;
    mqttSubscribed = false;
}

// ---------------------------------------------------------------------------
// setup()
// ---------------------------------------------------------------------------
void setup()
{
    Serial.begin(115200);

    netClient.setInsecure();   // brokers behind ngrok require TLS but no CA check

    generateBootId(bootId);
    Serial.printf("[main] bootId=%s\n", bootId);

    // 1. Error handler — comes up first so later failures can blink.
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
            macAddress = WiFi.macAddress();
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
            // Subscribe sensor command channel (pull-pattern, see protocol doc §5.5)
            mqtt.on(topics.command("sensor").c_str(), handleSensorCommand);

            // Publish ONLINE on the connect topic with retain=true so a freshly
            // subscribed service sees current state immediately.
            String onlinePayload = buildConnectJson("ONLINE");
            mqtt.publish(connectTopic.c_str(), onlinePayload.c_str(), /*retain=*/true);

            mqttSubscribed = true;
            lastHealthMs   = millis();
            Serial.println("[mqtt] subscribed and announced ONLINE");
        } else if (!connected && mqttSubscribed) {
            // Lost broker — flag and let next loop reconnect via PubSubClient.
            mqttSubscribed = false;
            errors.reportError(ERR_MQTT);
        }

        // Heartbeat to esp/health every 30s
        if (mqttSubscribed && (millis() - lastHealthMs) >= HEALTH_INTERVAL_MS) {
            publishHealth();
            lastHealthMs = millis();
        }
    }
}
