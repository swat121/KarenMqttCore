# KarenMqttCore — Architecture & Build Variants

> Документ объясняет, как устроена библиотека внутри, как файлы зависят друг
> от друга, и как с помощью `-D` (compile-time defines) собирать разные
> прошивки под разные платы из одной и той же кодовой базы.

---

## 1. Слои и зависимости

```
                       ┌──────────────────────────────────────────┐
                       │  src/KarenMqttCore.h                     │
   single public       │  (umbrella header — единственный вход)    │
   include for users   └────────────────────┬─────────────────────┘
                                            │ pulls all components
        ┌───────────────────┬───────────────┼──────────────┬───────────────┐
        │                   │               │              │               │
        ▼                   ▼               ▼              ▼               ▼
   ┌─────────┐         ┌─────────┐    ┌─────────┐    ┌─────────┐     ┌─────────┐
   │  core/  │         │ topics/ │    │ router/ │    │transport│     │ client/ │
   │ Config  │         │ Topics  │    │ Router  │    │ over    │     │ Facade  │
   │ Debug   │         │ ClientId│    │ (table) │    │ PubSub  │     │ + Ack   │
   └────┬────┘         └─────────┘    └────┬────┘    └────┬────┘     └────┬────┘
        │                                  │              │               │
        │ KarenMqttConfig.h pulled by:     │              │               │
        ├──────────────────────────────────┘              │               │
        ├─────────────────────────────────────────────────┘               │
        └─────────────────────────────────────────────────────────────────┘

         core/ does NOT depend on anyone in the library; it is leaf.
         client/ depends on transport/ + router/ + core/ — it composes.
```

**Главные правила:**

- Потребители делают **только** `#include <KarenMqttCore.h>`. Это
  umbrella, который тянет всё нужное.
- Внутри библиотеки модули общаются по относительным путям (`"../core/X.h"`).
- `core/` — leaf (никого не тянет из библиотеки), там лежат **constants и macros**.
- `client/KarenMqttClient` — **facade** поверх `transport/` + `router/`. Это
  то, чем работает пользователь.

---

## 2. Что делает каждый модуль

### 2.1 `core/`

| Файл | Содержит | Роль |
|---|---|---|
| `KarenMqttConfig.h` | `MqttConfig`, `MqttLwt` структуры + все `KAREN_MQTT_*` defaults | Конфиг и compile-time флаги. Все остальные модули его подключают. |
| `KarenMqttDebug.h` | `KAREN_MQTT_LOG(fmt, ...)` macro | Логирование в `Serial.printf`, активируется флагом `-DKAREN_MQTT_DEBUG=1`. Без флага — `do {} while(0)`, нулевой оверхед. |

**Lifetime-контракт `MqttConfig`** (важный!):

> Все `const char*` в `MqttConfig` и `MqttLwt` — **shallow-копии указателей**.
> Библиотека НЕ копирует строки. Указатели должны оставаться валидными до
> конца жизни `KarenMqttClient`. Используй `static const char*` или
> long-lived `String` с `.c_str()`. Не передавай временные.

### 2.2 `transport/MqttTransport.{h,cpp}`

Тонкая обёртка вокруг **PubSubClient**. Знает только про TCP/TLS-сокет
и про MQTT-протокол.

| Метод | Что делает |
|---|---|
| `MqttTransport(Client& netClient)` | конструктор; принимает `WiFiClient` или `WiFiClientSecure` полиморфно через базу `Client&` |
| `void begin(const MqttConfig& cfg)` | копирует cfg в член `cfg_`, настраивает host/port/keepAlive/bufferSize, ставит callback |
| `bool connect()` | вызывает `pubsub_.connect(...)` — выбирает 4 разных overload'а в зависимости от наличия LWT и user/pass |
| `bool loop()` | вызывается из главного `loop()`. Если connected → `pubsub_.loop()`. Если нет → throttled reconnect (`reconnectIntervalMs`). |
| `bool publish/subscribe/unsubscribe` | прокидывает в PubSubClient |
| `void setMessageCallback(...)` | используется `KarenMqttClient` чтобы ловить входящие |
| `setCaCertificate(pem)` (только под `KAREN_MQTT_TLS`) | `dynamic_cast<WiFiClientSecure*>` + `setCACert`; для проверки сертификата сервера |

### 2.3 `router/MqttTopicRouter.{h,cpp}`

**Fixed-size таблица** (`KAREN_MQTT_MAX_HANDLERS = 8`) пар «топик → handler».
Без динамической памяти, exact-match (без wildcards).

| Метод | Что делает |
|---|---|
| `bool add(const char* topic, MqttHandler h)` | добавляет запись; возвращает `false` при переполнении |
| `bool remove(const char* topic)` | удаляет по точному совпадению строки |
| `bool dispatch(const char* topic, payload, len)` | вызывает handler для совпавшего топика; возвращает `false` если такого топика нет |

Длина топика ограничена `KAREN_MQTT_TOPIC_BUFFER` (по умолчанию 64). Если
твои топики длиннее, увеличь.

### 2.4 `topics/KarenMqttTopics.{h,cpp}`

Конвенция топиков Karen-экосистемы. Чистые builder'ы строк, никакой логики.

```cpp
KarenMqttTopics t("my-device");
t.command("sensor")  // "esp/my-device/sensor/command"
t.event("sensor")    // "esp/my-device/sensor/event"
t.connect()          // "esp/my-device/connect"
t.custom("ota")      // "esp/my-device/ota"
KarenMqttTopics::health();  // "esp/health"  (статик)
```

`KarenMqttClientId::fromMac(prefix)` — единственная реальная функция,
которая трогает железо: читает MAC, удаляет `:`, склеивает с префиксом.

### 2.5 `client/`

**`KarenMqttClient`** — facade поверх transport + router. Это то, чем
пишет user.

| Метод | Что делает |
|---|---|
| `KarenMqttClient(Client& net)` | конструирует `transport_(net)` + пустой `router_` |
| `void begin(const MqttConfig& cfg)` | вызывает `transport_.begin(cfg)` и регистрирует на transport свой message callback. Внутри callback'а делает `router_.dispatch(topic, payload, len)`. |
| `bool loop()` | вызывает `transport_.loop()` |
| `bool on(topic, handler)` | `router_.add(topic, handler)` + `transport_.subscribe(topic)` если уже connected |
| `void off(topic)` | `router_.remove` + `transport_.unsubscribe` |
| `bool publish(...)` | передаёт в transport |
| `transport()/router()` | escape hatch для редких случаев (TLS setup, query) |

**`KarenMqttAck.cpp`** (тот же класс, отдельная .cpp под флагом
`KAREN_MQTT_JSON_ACK`):

Реализует `publishAck(topic, requestId, ok, message)` и его JSON-overload.
Зачем отдельный .cpp: чтобы при `-DKAREN_MQTT_JSON_ACK=0` весь ArduinoJson
код выпал из сборки.

---

## 3. Sequence diagrams

### 3.1 `mqtt.begin(cfg)` → `mqtt.loop()` → подписка

```
firmware             KarenMqttClient   MqttTransport     PubSubClient   broker
  │                       │                │                  │            │
  │ begin(cfg)            │                │                  │            │
  ├──────────────────────►│                │                  │            │
  │                       │ begin(cfg)     │                  │            │
  │                       ├───────────────►│ setServer/KA/buf │            │
  │                       │                ├─────────────────►│            │
  │                       │ setMessageCb   │ setCallback      │            │
  │                       ├───────────────►├─────────────────►│            │
  │                       │                │                  │            │
  │ loop()                │                │                  │            │
  ├──────────────────────►│ loop()         │                  │            │
  │                       ├───────────────►│ connected? false │            │
  │                       │                ├─────────────────►│            │
  │                       │                │ connect(id,...)  │            │
  │                       │                ├─────────────────►│ CONNECT    │
  │                       │                │                  ├───────────►│
  │                       │                │                  │   CONNACK  │
  │                       │                │                  │◄───────────┤
  │                       │                │ true             │            │
  │                       │                │◄─────────────────┤            │
  │                       │                │ pubsub.loop()    │            │
  │                       │                ├─────────────────►│            │
  │                       │ true           │                  │            │
  │                       │◄───────────────┤                  │            │
  │                       │                │                  │            │
  │ on("esp/X/cmd", h)    │                │                  │            │
  ├──────────────────────►│                │                  │            │
  │                       │ router.add     │                  │            │
  │                       │ transport.sub  │ pubsub.subscribe │            │
  │                       │                ├─────────────────►│ SUBSCRIBE  │
  │                       │                │                  ├───────────►│
  │                       │                │                  │   SUBACK   │
  │                       │                │                  │◄───────────┤
```

### 3.2 Получение сообщения

```
broker        PubSubClient      MqttTransport         KarenMqttClient    handler (firmware)
   │                │                  │                   │                  │
   │ PUBLISH        │                  │                   │                  │
   ├───────────────►│                  │                   │                  │
   │                │ callback(t,p,l)  │                   │                  │
   │                ├─────────────────►│ messageCallback_  │                  │
   │                │                  ├──────────────────►│ router.dispatch  │
   │                │                  │                   ├─────────────────►│
   │                │                  │                   │   (your code)    │
```

### 3.3 `publishAck`

```
firmware         KarenMqttClient   ArduinoJson      MqttTransport    PubSubClient
   │                  │                │                 │                 │
   │ publishAck(...)  │                │                 │                 │
   ├─────────────────►│ JsonDocument   │                 │                 │
   │                  ├───────────────►│ doc[..] = ...   │                 │
   │                  │                │                 │                 │
   │                  │ measureJson()  │                 │                 │
   │                  ├───────────────►│ checks ≤ 256    │                 │
   │                  │ serializeJson  │                 │                 │
   │                  ├───────────────►│ → char buf[256] │                 │
   │                  │                │                 │                 │
   │                  │ publish(topic, buf, len)         │                 │
   │                  ├─────────────────────────────────►│ pubsub.publish  │
   │                  │                │                 ├────────────────►│ PUBLISH
```

---

## 4. Compile-time defines (KarenMqttCore)

Все эти флаги — **опциональны**, у каждого разумный default. Переопределяй
через `build_flags` в `platformio.ini` командой `-D<имя>=<значение>`.

| Define | Default | Где определён | Что меняет |
|---|---|---|---|
| `KAREN_MQTT_BUFFER_SIZE` | `1024` | `core/KarenMqttConfig.h:10` | Внутренний буфер PubSubClient в байтах. Увеличь если шлёшь/получаешь payload'ы > 1KB (например meta JSON). |
| `KAREN_MQTT_MAX_HANDLERS` | `8` | `core/KarenMqttConfig.h:14` | Сколько `on(topic, handler)` можно зарегистрировать. Увеличь если у тебя > 8 топиков. |
| `KAREN_MQTT_JSON_ACK` | `1` | `core/KarenMqttConfig.h:18` | `1` = `publishAck()` доступен. `0` = выключен, ArduinoJson всё ещё тянется как dep (см. limitations). |
| `KAREN_MQTT_DEBUG` | *unset* | `core/KarenMqttDebug.h` | `1` = `KAREN_MQTT_LOG()` пишет в Serial. *unset* = логирование выкошено компилятором (нулевой оверхед). |
| `KAREN_MQTT_TLS` | *unset* | `core/KarenMqttConfig.h:23` | Включает `MqttTransport::setCaCertificate(pemCa)`. Нужен только если хочешь **проверять** сертификат брокера. Без флага можно использовать `WiFiClientSecure` + `setInsecure()` и говорить TLS-у-броcker'у без проверки CA. |
| `KAREN_MQTT_WILDCARDS` | *unset* | `core/KarenMqttConfig.h:23` | **Fail-fast guard**: попытка собрать с `=1` вызывает `#error` — wildcard-роутинг не реализован, чтобы пользователь не подумал что `+`/`#` работают. |
| `KAREN_MQTT_TOPIC_BUFFER` | `64` | `core/KarenMqttConfig.h:36` | Максимальная длина строки топика, которую хранит router. Увеличь до 128/256 если топики длинные. |
| `KAREN_MQTT_DEFAULT_KEEPALIVE_SEC` | `60` | `core/KarenMqttConfig.h:26` | Default `keepAliveSec` в `MqttConfig`. Можно переопределить и в коде через `cfg.keepAliveSec = ...`. |
| `KAREN_MQTT_DEFAULT_RECONNECT_INTERVAL_MS` | `5000` | `core/KarenMqttConfig.h:30` | Default `reconnectIntervalMs` — как часто пытаться переподключиться. Аналогично можно переопределить в `cfg`. |

> **Лайфхак:** для debug-сборки добавь `-DKAREN_MQTT_DEBUG=1`, прошей,
> смотри Serial при 115200 — увидишь все `connecting to ...`, `connected`,
> `connect failed rc=-N`, `publish skipped: not connected` и т.д.

---

## 5. Как использовать defines для разных вариантов плат

PlatformIO позволяет описать **несколько `env:`** в одном `platformio.ini`,
каждый со своим набором `-D` флагов. Команда `pio run -e <env>` собирает
конкретный вариант.

### 5.1 Простейший случай — debug vs release

```ini
; platformio.ini

[env]
; общее для всех вариантов
platform        = espressif32
board           = weactstudio_esp32c3coreboard
framework       = arduino
monitor_speed   = 115200
lib_ldf_mode    = deep+
lib_deps =
    knolleary/PubSubClient@^2.8
    bblanchon/ArduinoJson@^7.0
    https://github.com/swat121/KarenWifiConnectionManager.git#v1.2.1
    https://github.com/swat121/KarenErrorHandler.git#v1.0.0

[env:debug]
build_flags =
    -std=gnu++17
    -DKAREN_MQTT_DEBUG=1
    -DCORE_DEBUG_LEVEL=5

[env:release]
build_flags =
    -std=gnu++17
    -DCORE_DEBUG_LEVEL=1
```

Сборка и заливка:

```bash
pio run -e debug -t upload         # debug-вариант
pio run -e release -t upload       # production-вариант
pio device monitor -e debug        # смотрим Serial
```

### 5.2 Варианты под разное железо

Скажем у тебя есть две версии платы:
- **karen-r1.5.2** — есть DS18B20 датчик температуры на GPIO 2
- **karen-r1.6.0** — нет датчика, но есть реле на GPIO 4

Объявляем feature-флаги уровня firmware (не KarenMqttCore — у него своих
нет) и используем `#ifdef` в коде.

```ini
[env]
platform        = espressif32
board           = weactstudio_esp32c3coreboard
framework       = arduino
monitor_speed   = 115200
lib_ldf_mode    = deep+
lib_deps =
    knolleary/PubSubClient@^2.8
    bblanchon/ArduinoJson@^7.0
    https://github.com/swat121/KarenMqttCore.git#v0.3.0
    https://github.com/swat121/KarenWifiConnectionManager.git#v1.2.1
    https://github.com/swat121/KarenErrorHandler.git#v1.0.0

; ─── вариант 1: только температурный датчик ───
[env:karen-r1.5.2]
build_flags =
    -std=gnu++17
    -DBOARD_REVISION=\"karen-r1.5.2\"
    -DFIRMWARE_VERSION=\"1.1.0\"
    -DHAS_SENSORS=1
    -DSENSOR_PIN=2
    -DHAS_SWITCHES=0
    -DKAREN_MQTT_BUFFER_SIZE=2048

; ─── вариант 2: только реле ───
[env:karen-r1.6.0]
build_flags =
    -std=gnu++17
    -DBOARD_REVISION=\"karen-r1.6.0\"
    -DFIRMWARE_VERSION=\"1.1.0\"
    -DHAS_SENSORS=0
    -DHAS_SWITCHES=1
    -DRELAY_PIN=4

; ─── вариант 3: всё включено + debug ───
[env:karen-full-debug]
build_flags =
    -std=gnu++17
    -DBOARD_REVISION=\"karen-full\"
    -DFIRMWARE_VERSION=\"1.1.0-dev\"
    -DHAS_SENSORS=1
    -DSENSOR_PIN=2
    -DHAS_SWITCHES=1
    -DRELAY_PIN=4
    -DKAREN_MQTT_DEBUG=1
    -DKAREN_MQTT_BUFFER_SIZE=4096
```

В `main.cpp`:

```cpp
#include "KarenMqttCore.h"

void setup() {
    // ... wifi, mqtt.begin() ...

#if HAS_SENSORS
    pinMode(SENSOR_PIN, INPUT);
    mqtt.on(topics.command("sensor").c_str(), handleSensorCommand);
    Serial.printf("[boot] sensors enabled on pin %d\n", SENSOR_PIN);
#endif

#if HAS_SWITCHES
    pinMode(RELAY_PIN, OUTPUT);
    mqtt.on(topics.command("switch").c_str(), handleSwitchCommand);
    Serial.printf("[boot] switches enabled on pin %d\n", RELAY_PIN);
#endif

    Serial.printf("[boot] %s fw=%s\n", BOARD_REVISION, FIRMWARE_VERSION);
}
```

Заливка конкретного варианта:

```bash
pio run -e karen-r1.5.2  -t upload     # шьём датчиковую плату
pio run -e karen-r1.6.0  -t upload     # шьём релейную плату
pio run -e karen-full-debug -t upload  # шьём debug-всё-включено
```

### 5.3 Как читать `#if` правильно

```cpp
#if HAS_SENSORS         // ✅ работает с =0 и =1
    ...
#endif

#ifdef HAS_SENSORS      // ⚠️ работает с =0 ТОЖЕ (define существует!)
    ...
#endif
```

Если в `build_flags` стоит `-DHAS_SENSORS=0`, то `#ifdef HAS_SENSORS` будет
**true** (потому что макрос определён, хоть и в `0`). Используй `#if HAS_SENSORS`
если хочешь различать `=0` и `=1`. Используй `#ifdef` только если флаг
«или есть, или его вообще нет» (как `KAREN_MQTT_TLS`).

В платформенном `core/KarenMqttConfig.h` я использую `#ifndef X #define X default`
паттерн — это означает «возьми value из `build_flags`, а если его там нет,
поставь дефолт». Это правильная идиома для всех `KAREN_MQTT_*`.

### 5.4 Условные `lib_deps`

`build_flags` идут в компилятор, а `lib_deps` — это директива PIO. Если
твоему варианту нужны разные **библиотеки** (а не только разные defines),
выноси `lib_deps` в `env:`:

```ini
[env:karen-r1.5.2]
build_flags = -DHAS_SENSORS=1 -DSENSOR_PIN=2
lib_deps =
    ${env.lib_deps}                       ; наследуем общие
    paulstoffregen/OneWire@^2.3.7         ; добавляем датчик-специфичные
    milesburton/DallasTemperature@^3.11

[env:karen-r1.6.0]
build_flags = -DHAS_SWITCHES=1 -DRELAY_PIN=4
lib_deps = ${env.lib_deps}                ; этому варианту OneWire не нужен
```

Это экономит flash на тех платах, где библиотека не используется.

### 5.5 Размер прошивки для разных вариантов

Каждый `env:` хранит свой `.pio/build/<env>/firmware.elf`. Чтобы быстро
сравнить размеры:

```bash
pio run -e karen-r1.5.2
pio run -e karen-r1.6.0
ls -lh .pio/build/*/firmware.bin
```

При `HAS_SENSORS=0` линкер выкидывает весь datasheet-код (OneWire,
DallasTemperature) если они вызываются только под `#if`. Это даёт реальную
экономию flash, не «теоретическую».

---

## 6. Что НЕ покрывает KarenMqttCore

Эти моменты намеренно вне библиотеки — они либо firmware-specific, либо
поедут в `KarenDevicePresence`:

- **Re-subscribe после reconnect** — `on()` нужно вызывать снова после
  каждого пере-CONNECT. Сейчас firmware делает это руками
  (`if (connected && !subscribed)`-паттерн в `main.cpp`). В будущей
  `KarenDevicePresence` появится `onConnect` callback.
- **Offline publish queue** — `publish()` возвращает `false` если нет
  коннекта; сообщение теряется. Если нужна гарантированная доставка —
  собирай очередь у себя.
- **Wildcard subscriptions** (`esp/+/health`) — не поддерживаются;
  `-DKAREN_MQTT_WILDCARDS=1` валит компиляцию с `#error`.
- **MQTT 5.0** — PubSubClient говорит только 3.1.1. Если брокер требует
  5.0 — нужна другая библиотека (например `ArduinoMqttClient`).
- **WebSocket transport** — нет. См. вопрос про WS в чате (если интересно,
  что для этого нужно).

---

## Приложение: куда что класть

| Хочешь… | Куда положить |
|---|---|
| общий конфиг для всех вариантов | `[env]` в `platformio.ini` |
| флаги под конкретную плату | `[env:variant-name] build_flags = ...` |
| значение размера буфера / лимита | `-DKAREN_MQTT_BUFFER_SIZE=4096` в `build_flags` |
| включение отладки в одном варианте | `-DKAREN_MQTT_DEBUG=1` только в этом `env:` |
| feature-toggle уровня прошивки | `-DHAS_X=1/0` + `#if HAS_X` в коде |
| зависимости только для одного варианта | `lib_deps` внутри `env:` |
| код, который должен скрываться без feature | `#if HAS_X ... #endif` (не `#ifdef`) |
| отладочный лог в коде библиотеки | `KAREN_MQTT_LOG("fmt", args...)` из `core/KarenMqttDebug.h` |
