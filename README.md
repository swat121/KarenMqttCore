# KarenMqttCore

Modular MQTT transport and topic router library for the Karen ESP32 firmware family.
PlatformIO + Arduino framework, C++11, target ESP32-C3.

## Features

- Thin `MqttTransport` over PubSubClient: connect, reconnect, publish, subscribe.
- Exact-match `MqttTopicRouter` with fixed-size handler storage (no heap fragmentation).
- `KarenMqttClient` facade: one object, one `loop()` call.
- LWT (Last Will and Testament) support.
- Synchronous polling reconnect with configurable interval.
- Compile-time feature flags with documented defaults.
- `publishAck` — JSON ACK helper for command-driven sensor pattern (requires ArduinoJson v7).
- TLS-ready: code guarded under `#if KAREN_MQTT_TLS` compiles when flag is set.
- Topic convention: `esp/<deviceId>/<domain>/(command|event)` + `esp/<deviceId>/connect` + broadcast `esp/health`.

## Installation

### PlatformIO (recommended)

Add to your `platformio.ini`:

```ini
lib_deps =
    https://github.com/swat121/KarenMqttCore.git
    knolleary/PubSubClient @ ^2.8
    bblanchon/ArduinoJson @ ^7.0
```

### Arduino IDE

Download the repository as a ZIP and use Sketch > Include Library > Add .ZIP Library.

## Quick Start

```cpp
#include <WiFi.h>
#include <WiFiClient.h>
#include "KarenMqttCore.h"

WiFiClient         netClient;
KarenMqttClient    mqtt(netClient);
KarenMqttTopics    topics("my-device-01");

// Keep as globals — c_str() must remain valid for the mqtt object's lifetime.
static String      connectTopic;
static String      lwtPayload;       // JSON OFFLINE payload, used as LWT
static bool        subscribed = false;

void setup() {
    WiFi.begin("SSID", "PASSWORD");
    while (WiFi.status() != WL_CONNECTED) delay(500);

    // See docs/registration-protocol.md for the full connect/LWT payload shape:
    //   { "deviceId", "macAddress", "status": "ONLINE"|"OFFLINE", "bootId" }
    connectTopic = topics.connect();
    lwtPayload   = "{\"deviceId\":\"my-device-01\",\"status\":\"OFFLINE\"}";  // simplified

    MqttConfig cfg;
    cfg.host     = "broker.hivemq.com";
    cfg.port     = 1883;
    cfg.clientId = "karen-my-device-01";
    cfg.deviceId = "my-device-01";
    cfg.lwt.topic   = connectTopic.c_str();
    cfg.lwt.payload = lwtPayload.c_str();
    cfg.lwt.qos     = 1;
    cfg.lwt.retain  = true;
    cfg.hasLwt   = true;

    mqtt.begin(cfg);
    // Note: call on() AFTER connecting — see loop() for the connect-transition pattern.
}

void loop() {
    bool connected = mqtt.loop();

    // Subscribe on every transition disconnected → connected (v0.1 re-subscribe pattern).
    // v0.2 will provide an onConnect callback.
    if (connected && !subscribed) {
        mqtt.on(topics.command("sensor").c_str(), [](const char* topic, const uint8_t* payload, size_t len) {
            // handle command
        });
        subscribed = true;
    } else if (!connected && subscribed) {
        subscribed = false;
    }
}
```

See `examples/basic/` for a complete command-driven sensor example with `publishAck`.

## API Reference

### `KarenMqttClient`

| Method | Description |
|--------|-------------|
| `KarenMqttClient(Client& net)` | Constructor. Pass a `WiFiClient` or `WiFiClientSecure`. |
| `void begin(const MqttConfig& cfg)` | Configure and prepare for connection. Call once in `setup()`. |
| `bool loop()` | Must be called from `loop()`. Drives reconnect and PubSubClient loop. Returns `true` if connected. |
| `bool on(const char* topic, MqttHandler handler)` | Subscribe and register handler. Returns `false` if capacity exceeded. |
| `void off(const char* topic)` | Unsubscribe and remove handler. |
| `bool publish(const char* topic, const char* payload, bool retain)` | Publish string payload. |
| `bool publish(const char* topic, const uint8_t* payload, size_t len, bool retain)` | Publish binary payload. |
| `bool isConnected()` | Returns current connection state. |
| `bool publishAck(const char* topic, const char* requestId, bool ok, const char* message)` | Publish JSON ACK. Requires `KAREN_MQTT_JSON_ACK=1` (default). |
| `bool publishAck(const char* topic, const char* requestId, bool ok, const JsonObject& payload)` | Publish JSON ACK with nested payload object. |
| `MqttTransport& transport()` | Access underlying transport (e.g., for TLS setup). |
| `MqttTopicRouter& router()` | Access underlying router. |

### `KarenMqttTopics`

| Method | Description |
|--------|-------------|
| `KarenMqttTopics(const char* deviceId)` | Constructor. |
| `String command(const char* domain)` | Returns `esp/<deviceId>/<domain>/command`. |
| `String event(const char* domain)` | Returns `esp/<deviceId>/<domain>/event`. |
| `String connect()` | Returns `esp/<deviceId>/connect` — used for LWT and ONLINE state. |
| `String custom(const char* leaf)` | Returns `esp/<deviceId>/<leaf>` — free-form one-level topic (e.g. `esp/<id>/sensor` legacy push). |
| `static String health()` | Returns `esp/health` — broadcast heartbeat channel (no deviceId in path). |
| `const String& base()` | Returns `esp/<deviceId>`. |

### `MqttConfig`

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `host` | `const char*` | `nullptr` | Broker hostname or IP. |
| `port` | `uint16_t` | `1883` | Broker port. |
| `clientId` | `const char*` | `nullptr` | MQTT client ID. |
| `deviceId` | `const char*` | `nullptr` | Device ID (used by KarenMqttTopics). |
| `keepAliveSec` | `uint16_t` | `60` | MQTT keep-alive interval. |
| `reconnectIntervalMs` | `uint32_t` | `5000` | Reconnect polling interval. |
| `user` / `pass` | `const char*` | `nullptr` | Optional broker credentials. |
| `lwt` | `MqttLwt` | — | Last Will message. |
| `hasLwt` | `bool` | `false` | Enable LWT. |

## Compile-time Flags

Override in `build_flags` in your `platformio.ini`:

| Flag | Default | Description |
|------|---------|-------------|
| `KAREN_MQTT_BUFFER_SIZE` | `1024` | PubSubClient payload buffer in bytes. Increase for large payloads (e.g., `-DKAREN_MQTT_BUFFER_SIZE=2048`). |
| `KAREN_MQTT_MAX_HANDLERS` | `8` | Maximum registered topic handlers. |
| `KAREN_MQTT_TOPIC_BUFFER` | `64` | Maximum topic string length in router entries. Increase via `-DKAREN_MQTT_TOPIC_BUFFER=128` for long topics. |
| `KAREN_MQTT_JSON_ACK` | `1` | Enable `publishAck` helpers. Requires ArduinoJson. Set to `0` to exclude the API methods. Note: disabling `KAREN_MQTT_JSON_ACK` removes the API methods but does not remove the ArduinoJson dependency (v0.2 will track). |
| `KAREN_MQTT_DEBUG` | unset | Enable `KAREN_MQTT_LOG` output on Serial. |
| `KAREN_MQTT_TLS` | unset | Enable TLS code paths (WiFiClientSecure + `setCaCertificate`). Unset = disabled. |
| `KAREN_MQTT_WILDCARDS` | unset | Wildcard routing — causes compile-error in v0.1 (not implemented). |

## Topic Convention

```
esp/<deviceId>/<domain>/command   <- device receives commands
esp/<deviceId>/<domain>/event     <- device publishes readings / results / errors
esp/<deviceId>/connect            <- LWT + ONLINE/OFFLINE JSON (retain=true)
esp/<deviceId>/<leaf>             <- free-form one-level (legacy push patterns)
esp/health                        <- broadcast heartbeat (deviceId in payload)
```

Example for `deviceId = "sensor-01"` and `domain = "sensor"`:

```
esp/sensor-01/sensor/command
esp/sensor-01/sensor/event
esp/sensor-01/connect
esp/health
```

For the full Karen ecosystem registration protocol (payload shapes, `bootId`
semantics, capabilities, sensor pull pattern), see
[`docs/registration-protocol.md`](docs/registration-protocol.md).

## Building

To build the example locally:

```bash
pio run -d examples/basic
```

## Limitations

- **No wildcard routing** — exact-match only. Building with `-DKAREN_MQTT_WILDCARDS=1` causes a compile-error.
- **No re-subscribe after reconnect** — subscriptions must be re-issued after reconnect. Use the connect-transition pattern shown in Quick Start (`subscribed` flag in `loop()`). An `onConnect` callback is planned for the upcoming `KarenDevicePresence` library.
- **No offline publish queue** — `publish()` returns `false` when disconnected; messages are dropped.
- **No high-level registration flow** — building the connect/LWT JSON, generating `bootId`, and publishing meta is left to the firmware. The `KarenDevicePresence` library will encapsulate this.

## Examples

- `examples/basic/` — command-driven sensor pattern using `publishAck`.

## License

MIT. See [LICENSE](LICENSE).
