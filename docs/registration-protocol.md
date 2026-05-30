# Karen Device Registration Protocol — Design Doc

> Цель этого документа: зафиксировать **протокол** регистрации ESP-устройства в
> экосистеме Karen (через сервис `karen-device-registration`) и спроектировать
> **разделение библиотек**, при котором `KarenMqttCore` остаётся низкоуровневым
> MQTT-транспортом, а высокоуровневая логика registration / capabilities / meta
> уходит в отдельную надстройку (`KarenDevicePresence`).
>
> Документ **design proposal** — он описывает целевое состояние, а не текущий
> код. Часть пунктов требует изменений в `karen-device-registration` (Java) и
> новой библиотеки в этом репозитории.

## Document version

| | |
|---|---|
| **Version** | **v0.2** |
| **Date**    | 2026-05-30 |
| **Status**  | Design — pending implementation |

### Changelog

**v0.2 (2026-05-30)** — фиксация решений после ревью v0.1:
- Java-сервис **в этой итерации не трогаем**. Все изменения в `karen-device-registration` уходят в отдельный roadmap «Phase 2 — service catch-up».
- `esp/health` остаётся как target topic для firmware, но помечен как **service-deferred** — сервис подписан на `esp/+/health` и пока этих сообщений не увидит. Это нормально, обработчик и так заглушка.
- Sensor pull-pattern (§8) — firmware-ready, сервис не поддерживает. Помечено deferred.
- `error/event` теперь публикуется с **retain=true** (dashboard сразу видит last error).
- JSON по умолчанию **compact** (не pretty). Опциональный pretty через `-DKAREN_MQTT_JSON_PRETTY=1` для debug.
- `/switch/failed` и `/switch/complete` **слиты** в один `switch/event` (различаются полем `status`).
- OTA — общий топик `esp/ota/command` + `esp/ota/event`. Контроллер фильтрует по `macAddress` + `deviceId` в payload.
- `KarenDevicePresence` стартует с **v1.0.0** (а не 0.1.0).
- Firmware mitigation для unique-constraint `bootId`: bootId формируется как `mac[3:6] + esp_random()[0..3]` — практическая гарантия уникальности при сохранении 32-битного хранения. См. §6.2.

**v0.1 (2026-05-30)** — первая версия.

---

## 1. Контекст

**Текущее положение дел (по состоянию на 2026-05-30):**

| Сторона | Что есть | Что не так |
|---|---|---|
| ESP firmware (старая `MQTTManager.cpp`) | Полностью работающая регистрация: connect, meta, push-by-timer sensor | Монолитный 1000-строчный класс, всё захардкожено |
| `KarenMqttCore` v0.2.0 | Минимальный транспорт + topic helpers + ACK | Topics не совместимы с сервисом: `availability` вместо `connect`; нет `health`; нет meta builder'а; payload контракты не зафиксированы |
| `karen-device-registration` | Принимает `esp/+/connect`, `esp/+/meta/event`, `esp/+/health`; шлёт `esp/{id}/meta/command` | health-handler — заглушка; QoS=0 на outbound; ObjectMapper не как Spring Bean |

**Чего хотим:**

1. Контракт регистрации зафиксирован отдельной библиотекой — не повторять старую монолитность.
2. `KarenMqttCore` остаётся транспортом без знаний о Karen-экосистеме.
3. Sensor переезжает с push-by-timer на pull-by-command (симметрия со switch).
4. `bootId` имеет ясную семантику с обеих сторон.
5. Компилируемые фичи (`SENSOR_FLAG`, `SWITCH_FLAG`) меняют **только** meta + подписки, без правок в business logic.

---

## 2. Архитектура слоёв

```
   ┌────────────────────────────────────────────────────────────────┐
   │  Firmware (application code)                                    │
   │  - main.cpp: pin map, sensor/switch business logic              │
   │  - on*Command() callbacks                                       │
   │  - compile-time defines: HAS_SENSORS, HAS_SWITCHES, FW_VERSION  │
   └──────────────────────────┬─────────────────────────────────────┘
                              │ uses
   ┌──────────────────────────▼─────────────────────────────────────┐
   │  KarenDevicePresence  (NEW library — этот документ)             │
   │  - registration handshake (connect → meta → ongoing)            │
   │  - LWT payload + connect/online payload                         │
   │  - DeviceMeta builder (sensors/switches/settings/errors/caps)   │
   │  - Pull-based sensor request handler                            │
   │  - MAC filter + requestId checks                                │
   │  - bootId generation                                            │
   │  - subscribes to: meta/command, sensor/command, switch/command  │
   └──────────────────────────┬─────────────────────────────────────┘
                              │ depends on
   ┌──────────────────────────▼─────────────────────────────────────┐
   │  KarenMqttCore  (existing v0.2.0)                              │
   │  - MqttTransport (PubSubClient)                                 │
   │  - MqttTopicRouter                                              │
   │  - KarenMqttTopics                                              │
   │  - publishAck (ArduinoJson v7)                                  │
   └─────────────────────────────────────────────────────────────────┘
```

**Принципы разделения:**

- `KarenMqttCore` НЕ знает про Karen-сервис. Его можно использовать в любом MQTT-проекте.
- `KarenDevicePresence` НЕ дублирует низкоуровневые операции (publish/subscribe/loop). Делегирует в `KarenMqttCore`.
- Firmware код задаёт только бизнес-логику: пины, чтение датчиков, реакцию на команды.

**Что НЕ входит** в `KarenDevicePresence` (остаётся в firmware):
- работа с конкретным железом (DS18B20, PCF8574, relay pins);
- хранение settings в NVS;
- работа с расписанием (`Quartz`-аналог);
- OTA — это отдельная библиотека (`KarenOTA`).

---

## 3. Топики — полная карта

> ✏️ Изменения относительно текущего сервиса помечены **🔧 NEEDS SERVICE UPDATE**.

| Топик | Direction | Назначение | Статус |
|---|---|---|---|
| `esp/<deviceId>/connect` | device → service | LWT + ONLINE/OFFLINE статус | ✅ совместимо |
| `esp/health` | device → service | Heartbeat от всех устройств | ⏸ **service-deferred** — сервис подписан на `esp/+/health`, сообщения не получит. См. Phase 2. |
| `esp/<deviceId>/meta/command` | service → device | Запрос полной меты | ✅ совместимо |
| `esp/<deviceId>/meta/event` | device → service | Полная мета (capabilities) | ✅ совместимо |
| `esp/<deviceId>/sensor/command` | service → device | Запрос на чтение датчика | ⏸ **service-deferred** — pull pattern, сервис ещё не шлёт |
| `esp/<deviceId>/sensor/event` | device → service | Ответ с показаниями датчика | ⏸ **service-deferred** — handler ещё не написан |
| `esp/<deviceId>/switch/command` | service → device | Команда переключателю | ✅ совместимо |
| `esp/<deviceId>/switch/event` | device → service | Подтверждение, состояние, ошибки, complete (всё через `status`) | ✅ совместимо |
| `esp/<deviceId>/setting/command` | service → device | Изменение настроек | ✅ совместимо |
| `esp/<deviceId>/setting/event` | device → service | Подтверждение настроек | ✅ совместимо |
| `esp/<deviceId>/error/event` | device → service | Публикация ошибок (retain=true) | ✅ совместимо |
| `esp/ota/command` | service → device | OTA-команда (broadcast); фильтр по `macAddress` + `deviceId` в payload | ⏸ deferred — OTA либа отдельно |
| `esp/ota/event` | device → service | OTA-ответ от конкретного устройства | ⏸ deferred |

> **`esp/health` без deviceId** — осознанный выбор: один единый канал для
> heartbeat всех устройств, payload содержит `deviceId`. Сейчас сервис подписан
> на `esp/+/health` (с wildcard в позиции deviceId) и **не получит** сообщения
> на одноуровневый `esp/health`. Это известный gap, в этой итерации оставляем
> — сервисный handler всё равно заглушка (`handleHealth` игнорирует результат).
> Догоним в Phase 2.
>
> **`esp/ota/*` без deviceId** — OTA-канал общий для всех. В payload
> присутствуют `macAddress` и `deviceId`, контроллер на устройстве проверяет
> совпадение и реагирует только на свои сообщения. Так дашборду не нужно
> знать deviceId, чтобы триггернуть OTA для конкретного устройства.

### 3.1 QoS и retain

| Сообщение | QoS | Retain | Почему |
|---|---|---|---|
| `connect` (online publish) | 1 | true | retain=true чтобы новый subscriber увидел текущее состояние |
| `connect` (LWT, offline) | 1 | true | retain=true так же как online — брокер заменит online на offline |
| `health` | 0 | false | fire-and-forget; следующий heartbeat через 30s всё восстановит |
| `meta/event` | 1 | false | важно доставить, но retain не нужен (сервис всегда сам запрашивает) |
| `meta/command` | 1 | false | сервис должен убедиться что устройство получило |
| `sensor/event` | 1 | false | актуальность важна, retain бессмыслен |
| `switch/event` | 1 | false | подтверждение должно дойти |
| `error/event` | 1 | **true** | dashboard сразу видит last error при подписке (v0.2) |
| `ota/command` | 1 | false | broadcast OTA-команда |
| `ota/event` | 1 | false | per-device OTA-ответ |

> **JSON payload formatting** — по умолчанию **compact** (`serializeJson`). Это
> экономит ~30% bandwidth по сравнению с pretty. Для отладки можно собрать
> прошивку с `-DKAREN_MQTT_JSON_PRETTY=1` — тогда `KarenDevicePresence` будет
> использовать `serializeJsonPretty`. В production всегда compact.

---

## 4. Registration handshake — пошагово

```
ESP                    Broker                    karen-device-registration
 │                       │                                │
 │  CONNECT(LWT=connect, │                                │
 │          payload=OFFLINE,                              │
 │          retain=true) │                                │
 ├──────────────────────►│                                │
 │                       │                                │
 │  PUB connect ONLINE   │      esp/<id>/connect          │
 │  (retain=true, QoS=1) ├───────────────────────────────►│ handleLwtConnect
 │                       │                                │   ├─ saveOrUpdateFromLwt
 │                       │                                │   │  └─ Kafka: device.registration.connect
 │                       │                                │   └─ if ONLINE: send RequestMeta
 │                       │   esp/<id>/meta/command        │
 │                       │◄───────────────────────────────┤
 │  SUB ◄────────────────┤                                │
 │  {requestId, deviceId, macAddress}                     │
 │                       │                                │
 │  PUB meta event       │      esp/<id>/meta/event       │
 │  (full capabilities)  ├───────────────────────────────►│ handleMeta
 │                       │                                │   ├─ saveOrUpdateFromMeta
 │                       │                                │   └─ if metaFormat changed:
 │                       │                                │       Kafka: device.registration.meta
 │                       │                                │
 │  PUB health (30s)     │            esp/health          │
 ├──────────────────────►│───────────────────────────────►│ handleHealth (NOP — пока)
 │                       │                                │
 │  ▲ sensor/command     │                                │
 │  │ {requestId, READ}  │      esp/<id>/sensor/command   │
 │  │                    │◄───────────────────────────────┤
 │  │ PUB sensor/event   │      esp/<id>/sensor/event     │
 │  │ {requestId, value} ├───────────────────────────────►│ (future handler)
 │                       │                                │
 │  ▲ switch/command     │                                │
 │  │ {requestId, FORCE} │      esp/<id>/switch/command   │
 │  │                    │◄───────────────────────────────┤
 │  │ PUB switch/event   │      esp/<id>/switch/event     │
 │  │ {requestId, state} ├───────────────────────────────►│ (future handler)
 │                       │                                │
 │  X  (power loss)      │                                │
 │ ──┼─►                 │      esp/<id>/connect (LWT)    │
 │   ▼                   ├───────────────────────────────►│ handleLwtConnect (OFFLINE)
 │                       │ broker fires LWT                │   └─ status=OFFLINE in DB
```

### 4.1 Старт устройства

1. Boot → сгенерировать `bootId` (см. §6).
2. Прочитать `deviceId` (из NVS или sentinel первого старта).
3. Подключиться к WiFi (вне scope этого документа — `KarenConnection`).
4. Настроить MQTT:
   - LWT topic = `esp/<deviceId>/connect`
   - LWT payload = JSON `{deviceId, macAddress, status:"OFFLINE", bootId}`
   - LWT QoS=1, retain=true
5. MQTT connect к брокеру.
6. Подписки:
   - `esp/<deviceId>/meta/command` (QoS=1)
   - `esp/<deviceId>/sensor/command` (QoS=1) — **только если** `HAS_SENSORS`
   - `esp/<deviceId>/switch/command` (QoS=1) — **только если** `HAS_SWITCHES`
   - `esp/<deviceId>/setting/command` (QoS=1) — если есть settings
7. Опубликовать на `esp/<deviceId>/connect` payload `{...status:"ONLINE"...}` (retain=true).

### 4.2 Получение `meta/command`

1. Распарсить payload, проверить `macAddress` совпадает с локальным.
2. Запомнить `requestId`.
3. Собрать meta-payload через `KarenDevicePresence::buildMeta()` (см. §5.3).
4. Опубликовать на `esp/<deviceId>/meta/event` (QoS=1, retain=false).

### 4.3 Дальше — request/response per команда

Для каждой команды (sensor/switch/setting):
1. RX `command` → проверить `macAddress`, извлечь `requestId`, `command`, `payload`.
2. Выполнить локально (callback в firmware).
3. TX `event` с `{requestId, status:"success"|"error", payload:{...}, macAddress, ...}`.

### 4.4 Health (heartbeat)

Каждые **30 секунд** (`KAREN_HEALTH_INTERVAL_MS`) публиковать:
- topic: `esp/health` (без deviceId — единый канал)
- payload: `{deviceId, macAddress, uptimeMs, freeHeap, rssi, bootId}`
- QoS=0, retain=false

> Сервис будет использовать это для liveness detection (вне сегодняшнего scope).

---

## 5. JSON-контракты с примерами

### 5.1 `esp/<deviceId>/connect` (online)

```json
{
  "deviceId":   "karen-r1.5.2-fw1.1.0-s6-001",
  "macAddress": "AA:BB:CC:DD:EE:FF",
  "status":     "ONLINE",
  "bootId":     "b3f0a829"
}
```

### 5.2 `esp/<deviceId>/connect` (LWT — OFFLINE)

Идентичен формату online, но `status: "OFFLINE"`. Публикуется **брокером**
автоматически при разрыве соединения. `bootId` тот же что и при online —
важно, чтобы сервис мог отличить «то же boot» от «новый boot после ребута».

```json
{
  "deviceId":   "karen-r1.5.2-fw1.1.0-s6-001",
  "macAddress": "AA:BB:CC:DD:EE:FF",
  "status":     "OFFLINE",
  "bootId":     "b3f0a829"
}
```

### 5.3 `esp/<deviceId>/meta/event`

> **В v0.2 firmware шлёт meta v1.0.0** (sensors через старое `topic`-поле),
> чтобы Java-сервис без правок мог его сохранить. Ниже показан
> **target-формат v2.0.0** — переходим на него после Phase 2.

```json
{
  "fwVersion":  "1.1.0",
  "metaFormat": "2.0.0",
  "requestId":  "0e30d3bb-9d05-4d8f-a98b-bdfaf86e9d61",
  "hardware":   {
    "mcu":      "ESP32-C3 WeAct Studio",
    "revision": "1.5.2"
  },
  "sensors":    [
    {
      "address":      "28:ff:64:1d:c1:01:23:45",
      "type":         "temperature",
      "unit":         "°C",
      "subtype":      "ds18b20",
      "commandTopic": "esp/karen-r1.5.2-fw1.1.0-s6-001/sensor/command",
      "eventTopic":   "esp/karen-r1.5.2-fw1.1.0-s6-001/sensor/event",
      "errorTopic":   "esp/karen-r1.5.2-fw1.1.0-s6-001/error/event",
      "supportedCommands": [
        { "id": "READ",     "version": 1 },
        { "id": "READ_ALL", "version": 1 }
      ]
    }
  ],
  "switches":   [
    {
      "pin":          4,
      "switchId":     1,
      "inverted":     false,
      "subtype":      "relay",
      "commandTopic": "esp/karen-r1.5.2-fw1.1.0-s6-001/switch/command",
      "eventTopic":   "esp/karen-r1.5.2-fw1.1.0-s6-001/switch/event",
      "errorTopic":   "esp/karen-r1.5.2-fw1.1.0-s6-001/error/event",
      "supportedCommands": [
        { "id": "UPDATE_SCHEDULE",   "version": 1 },
        { "id": "GET_SCHEDULE",      "version": 1 },
        { "id": "CLEAR_SCHEDULE",    "version": 1 },
        { "id": "GET_ALL_SWITCHES",  "version": 1 },
        { "id": "FORSE_SWITCH_STATE","version": 1 },
        { "id": "TOGGLE_LOCK",       "version": 1 }
      ]
    }
  ],
  "settings":     {
    "commandTopic": "esp/karen-r1.5.2-fw1.1.0-s6-001/setting/command",
    "eventTopic":   "esp/karen-r1.5.2-fw1.1.0-s6-001/setting/event"
  },
  "errors":       {
    "eventTopic":   "esp/karen-r1.5.2-fw1.1.0-s6-001/error/event"
  },
  "capabilities": {
    "supportsOTA":       false,
    "supportsScheduler": true,
    "qos":               1,
    "retainSupported":   true
  }
}
```

**Условные поля:**
- `sensors[]` — присутствует **только** если `HAS_SENSORS` определён в firmware.
- `switches[]` — присутствует **только** если `HAS_SWITCHES`.
- При отсутствии sensors блок просто пропускается, а не публикуется пустым массивом. Это позволит сервису различать «нет датчиков» (нет блока) и «есть, но 0 штук».

#### 5.3.1 Текущий формат v1.0.0 (что реально шлёт firmware в этой итерации)

```json
{
  "fwVersion":  "1.1.0",
  "metaFormat": "1.0.0",
  "requestId":  "0e30d3bb-9d05-4d8f-a98b-bdfaf86e9d61",
  "hardware":   {
    "mcu":      "ESP32-C3 WeAct Studio",
    "revision": "1.5.2"
  },
  "sensors":    [
    {
      "address": "28:ff:64:1d:c1:01:23:45",
      "type":    "temperature",
      "unit":    "°C",
      "topic":   "esp/karen-r1.5.2-fw1.1.0-s6-001/sensor"
    }
  ],
  "switches":   [
    {
      "pin":          4,
      "switchId":     1,
      "inverted":     false,
      "subtype":      "relay",
      "commandTopic": "esp/karen-r1.5.2-fw1.1.0-s6-001/switch/command",
      "eventTopic":   "esp/karen-r1.5.2-fw1.1.0-s6-001/switch/event",
      "errorTopic":   "esp/karen-r1.5.2-fw1.1.0-s6-001/switch/event",
      "supportedCommands": [
        { "id": "UPDATE_SCHEDULE",   "version": 1 },
        { "id": "GET_SCHEDULE",      "version": 1 },
        { "id": "CLEAR_SCHEDULE",    "version": 1 },
        { "id": "GET_ALL_SWITCHES",  "version": 1 },
        { "id": "FORSE_SWITCH_STATE","version": 1 },
        { "id": "TOGGLE_LOCK",       "version": 1 }
      ]
    }
  ],
  "settings":     {
    "commandTopic": "esp/karen-r1.5.2-fw1.1.0-s6-001/setting/command",
    "eventTopic":   "esp/karen-r1.5.2-fw1.1.0-s6-001/setting/event"
  },
  "errors":       {
    "eventTopic":   "esp/karen-r1.5.2-fw1.1.0-s6-001/error/event"
  },
  "capabilities": {
    "supportsOTA":       false,
    "supportsScheduler": true,
    "qos":               1,
    "retainSupported":   true
  }
}
```

Отличия v1.0.0 от target v2.0.0:
- `sensors[].topic` (push-pattern) вместо `commandTopic/eventTopic/supportedCommands`.
- `switch[].errorTopic` указывает на тот же `switch/event` — в v0.2 мы слили `/failed` в общий event с `status: "error"`. Раньше был отдельный `esp/<id>/switch/failed`.

### 5.4 `esp/<deviceId>/meta/command` (от сервиса)

```json
{
  "requestId":  "0e30d3bb-9d05-4d8f-a98b-bdfaf86e9d61",
  "deviceId":   "karen-r1.5.2-fw1.1.0-s6-001",
  "macAddress": "AA:BB:CC:DD:EE:FF"
}
```

Поле `macAddress` поддерживается прошивкой как фильтр: либо точное совпадение,
либо wildcard `"*:*:*:*:*:*"` (broadcast — отвечают все).

### 5.5 Sensor (новый pull-pattern)

**Команда** `esp/<deviceId>/sensor/command`:

```json
{
  "requestId":  "8a1f2c3d-4e5f-6789-0abc-def012345678",
  "deviceId":   "karen-r1.5.2-fw1.1.0-s6-001",
  "macAddress": "AA:BB:CC:DD:EE:FF",
  "command":    "READ",
  "payload":    {
    "sensorAddress": "28:ff:64:1d:c1:01:23:45"
  }
}
```

Поддерживаемые `command`:
| `command`   | `payload`                  | Семантика |
|-------------|----------------------------|-----------|
| `READ`      | `{sensorAddress}`          | Прочитать конкретный датчик |
| `READ_ALL`  | `{}`                       | Прочитать все доступные датчики |

**Ответ** `esp/<deviceId>/sensor/event` (успех):

```json
{
  "requestId":  "8a1f2c3d-4e5f-6789-0abc-def012345678",
  "deviceId":   "karen-r1.5.2-fw1.1.0-s6-001",
  "macAddress": "AA:BB:CC:DD:EE:FF",
  "status":     "success",
  "payload":    {
    "readings": [
      {
        "address":   "28:ff:64:1d:c1:01:23:45",
        "type":      "temperature",
        "value":     22.3,
        "unit":      "°C",
        "timestamp": 1748602800
      }
    ]
  }
}
```

**Ответ — ошибка чтения:**

```json
{
  "requestId":  "8a1f2c3d-4e5f-6789-0abc-def012345678",
  "deviceId":   "karen-r1.5.2-fw1.1.0-s6-001",
  "macAddress": "AA:BB:CC:DD:EE:FF",
  "status":     "error",
  "errorMessage": "Sensor not found: 28:ff:64:00:00:00:00:00",
  "payload":    { "sensorAddress": "28:ff:64:00:00:00:00:00" }
}
```

### 5.6 Switch (без изменений vs старая прошивка)

См. старый `MQTTManager::mqttMessageCallback` для полного списка команд.
Здесь фиксируется только новое требование: ответ **обязан** содержать
`requestId` исходной команды.

**Пример команды `FORSE_SWITCH_STATE`:**

```json
{
  "requestId":  "abc-123",
  "deviceId":   "karen-r1.5.2-fw1.1.0-s6-001",
  "macAddress": "AA:BB:CC:DD:EE:FF",
  "command":    "FORSE_SWITCH_STATE",
  "switchId":   1,
  "state":      "on"
}
```

**Ответ:**

```json
{
  "requestId":  "abc-123",
  "deviceId":   "karen-r1.5.2-fw1.1.0-s6-001",
  "macAddress": "AA:BB:CC:DD:EE:FF",
  "status":     "success",
  "message":    "Switch 1 forced to on"
}
```

### 5.7 `esp/health`

```json
{
  "deviceId":   "karen-r1.5.2-fw1.1.0-s6-001",
  "macAddress": "AA:BB:CC:DD:EE:FF",
  "bootId":     "b3f0a829",
  "uptimeMs":   3600000,
  "freeHeap":   210432,
  "rssi":       -54
}
```

---

## 6. `bootId` — семантика и edge cases

### 6.1 Что это

`bootId` — уникальный 8-символьный hex-идентификатор **текущей boot-сессии
устройства**. Меняется при каждом старте прошивки. **НЕ персистится между
ребутами.**

### 6.2 Как генерировать (на ESP)

В Java-сервисе на колонке `bootId` стоит `@Column(unique = true)` (см. §6.4).
Это значит даже **разные устройства** не могут одновременно иметь один и тот
же `bootId` — при коллизии сервис отбрасывает connect-сообщение. Чтобы
практически исключить риск, формируем `bootId` так, чтобы он включал часть
MAC-адреса (уникален в LAN) **плюс** random:

```cpp
// 8 hex chars = 4 байта. Берём 2 байта из MAC + 2 байта random.
// MAC ESP32 имеет вид AA:BB:CC:DD:EE:FF — последние 3 байта device-specific.
uint8_t mac[6];
esp_efuse_mac_get_default(mac);            // или WiFi.macAddress() и распарсить
uint16_t macTail = (mac[4] << 8) | mac[5]; // младшие 2 байта MAC
uint16_t rnd     = (uint16_t)(esp_random() & 0xFFFF);

char bootId[9];
snprintf(bootId, sizeof(bootId), "%04x%04x", macTail, rnd);
```

Это даёт:
- **Коллизия между разными устройствами**: невозможна на одну boot-сессию,
  потому что префикс из MAC уникален в сети.
- **Коллизия с собственным предыдущим `bootId`**: 1 на ~65 тысяч (16-битный
  random tail) — на практике почти невозможно, потому что для коллизии нужно
  ребутнуться и попасть в тот же random seed.
- **Хранение**: 8 hex chars, помещается в существующий тип колонки.

Альтернатива «чистый random `esp_random()` → 8 hex» — проще, но в большой
сети теоретически возможна коллизия между двумя устройствами при
одновременном boot. С нашим вариантом таких рисков нет.

### 6.3 Как сервис интерпретирует

Логика в `DeviceService.getDeviceIfExist` (`:277-326`):

```
            ┌─────────────────────────────────────────┐
            │ RX connect {deviceId, mac, status, bootId}│
            └────────────────────┬────────────────────┘
                                 │
                  ┌──────────────▼──────────────┐
                  │ device exists in DB?         │
                  └──┬───────────────────────┬───┘
                     │ no                    │ yes
                     ▼                       ▼
            ┌─────────────────┐   ┌──────────────────────────────┐
            │ CREATE Device   │   │ bootId == saved bootId?       │
            │ with payload    │   └──┬─────────────────────────┬──┘
            └─────────────────┘      │ yes                     │ no
                                     ▼                         ▼
                          ┌──────────────────────┐  ┌──────────────────────┐
                          │ status changed?       │  │ REBOOT DETECTED       │
                          └──┬───────────────┬───┘  │ Update status, mac,    │
                             │ yes           │ no   │ bootId, lastSeen       │
                             ▼               ▼      └──────────────────────┘
                ┌─────────────────────┐  ┌────────────┐
                │ Update status only  │  │ No-op upsert│
                │ (graceful disconnect│  │ (idempotent) │
                │  без перезагрузки)  │  └────────────┘
                └─────────────────────┘
```

### 6.4 Edge cases

- **Sentinel `"000000"`** — если в БД `bootId == null` (старый девайс или
  миграция), сервис записывает `"000000"` (`DeviceService.java:302`). **Поэтому
  устройство не должно публиковать `"000000"` как реальный bootId** — это
  поломает reboot detection.
- **`bootId UNIQUE` в БД** (`Device.java:23`) — `@Column(unique = true)`. Это
  значит **два разных устройства не могут иметь одинаковый `bootId`
  одновременно**. При коллизии сервис кинет SQL constraint violation и
  `saveOrUpdateFromLwt` залогирует warn — `connect`-сообщение потеряется.
  **В v0.2 не трогаем Java**; вместо снятия constraint реализуем
  firmware-mitigation: `bootId = mac[4..5] + esp_random()[0..1]` (см. §6.2).
  Это делает коллизию практически невозможной без правок схемы БД. *Будущее
  улучшение (Phase 2): снять constraint или сделать unique по
  `(deviceId, bootId)`.*
- **Network blip без reboot** — устройство теряет MQTT, потом восстанавливает.
  `bootId` тот же → сервис не считает это перезагрузкой, просто обновляет
  status, kafka-событие connect отправляется заново.
- **LWT доставлен с задержкой** — брокер шлёт LWT через `keepAlive * 1.5`
  после потери. До этого момента сервис считает устройство ONLINE. Это
  нормально, mitigates через `health` (раз в 30s).
- **Reboot во время отправки meta** — устройство перезагрузится, новый
  `bootId`, всё начнётся с handshake.

---

## 7. Capabilities и compile-time defines

### 7.1 Defines в firmware

```cpp
// platformio.ini → build_flags
-DFIRMWARE_VERSION=\"1.1.0\"
-DMETA_FORMAT_VERSION=\"2.0.0\"
-DBOARD_REVISION=\"karen-r1.5.2\"

-DHAS_SENSORS=1        // включает sensor/* топики и блок sensors в meta
-DHAS_SWITCHES=1       // включает switch/* топики и блок switches в meta
-DHAS_SETTINGS=1       // включает setting/* топики и settings блок в meta
-DSUPPORTS_OTA=0
-DSUPPORTS_SCHEDULER=1
```

### 7.2 Как defines влияют на регистрацию

```cpp
#if HAS_SENSORS
  meta.beginSensors();
  meta.addSensor(...);          // в firmware-коде, после begin()
  meta.endSensors();
  presence.subscribe(topics.command("sensor"), onSensorCommand);
#endif

#if HAS_SWITCHES
  meta.beginSwitches();
  meta.addSwitch(...);
  meta.endSwitches();
  presence.subscribe(topics.command("switch"), onSwitchCommand);
#endif
```

Если `HAS_SENSORS=0` — блок `sensors` **не появляется** в meta JSON, подписки
на `sensor/command` не выполняются, RAM/flash экономятся.

### 7.3 metaFormat — версия схемы

`metaFormat` — это **версия структуры meta JSON**. Меняется при:
- добавлении новых полей в meta;
- изменении семантики `supportedCommands`;
- breaking-изменении (например, переход sensor с push на pull → `"1.0.0"` → `"2.0.0"`).

Сервис **публикует kafka-событие `device.registration.meta`** только при
смене `metaFormat`. Это значит при увеличении версии все consumers (другие
сервисы Karen) узнают, что мета обновилась.

**Предложенная последовательность:**
- `"1.0.0"` — текущая (`sensors[].topic` для push, без `supportedCommands`)
- `"2.0.0"` — новая (`sensors[].commandTopic/eventTopic/supportedCommands`)

---

## 8. Sensor: переход push → pull

### 8.1 Старый паттерн (push by timer)

ESP по таймеру (`publishTemperatureInterval`) сам публикует на
`esp/<deviceId>/sensor`:

```json
{ "address": "28:ff:...", "value": 22.3, "unit": "°C" }
```

**Минусы:**
- Сервис не контролирует частоту опроса.
- Нет requestId — нельзя коррелировать запрос/ответ.
- Простаивает датчик когда никто не смотрит.
- Тратит wifi/flash/energy.

### 8.2 Новый паттерн (pull by command)

Сервис (Quartz cron, REST endpoint, или user action) шлёт
`esp/<deviceId>/sensor/command` с `READ` или `READ_ALL`. ESP читает датчик
и публикует ответ на `esp/<deviceId>/sensor/event` с тем же `requestId`.

**Плюсы:**
- Симметрично со switch.
- requestId позволяет коррелировать.
- ESP не тратит ресурсы вхолостую.
- Можно вычитывать конкретный датчик, не все.
- Кэширование на стороне сервиса (если нужно low-frequency reading — кешем).

### 8.3 Что меняется в `karen-device-registration` (Phase 2 — deferred)

> ⏸ **В v0.2 эти изменения НЕ делаем.**
>
> **Важно:** сервис парсит meta через `new ObjectMapper()` (не Spring bean),
> то есть `FAIL_ON_UNKNOWN_PROPERTIES=true` по умолчанию. Любое неизвестное
> поле в `sensors[]` → `JsonProcessingException` → мета **не сохраняется**.
>
> Поэтому **firmware в v0.2 публикует meta v1.0.0** с текущей схемой:
> ```json
> "sensors": [ {"address": "...", "type": "...", "unit": "...", "topic": "esp/<id>/sensor"} ]
> ```
> Pull-pattern остаётся в дизайне (см. §5.5), но активируется ТОЛЬКО когда
> сервис обновит модель и поднимет ожидаемый `metaFormat` до `"2.0.0"`.
>
> Альтернатива (на будущее, не сейчас): сменить `new ObjectMapper()` →
> Spring bean с `spring.jackson.deserialization.fail-on-unknown-properties=false`.
> Тогда можно будет лить гибридные meta (старые + новые поля одновременно)
> и спокойно мигрировать.

**Java модель `Sensor.java`** (`src/main/java/.../model/sensors/Sensor.java`):

```java
// Было:
private String topic;

// Станет:
private String commandTopic;
private String eventTopic;
private String errorTopic;
private String subtype;            // "ds18b20", "bme280", ...

@ElementCollection
@CollectionTable(name = "sensor_commands", joinColumns = @JoinColumn(name = "sensor_id"))
private List<Command> supportedCommands;

@Data @NoArgsConstructor @AllArgsConstructor @Builder @Embeddable
public static class Command {
    private String id;        // "READ", "READ_ALL"
    private Integer version;  // 1
}
```

**Schema migration (Hibernate ddl-auto=update):**
- Поля добавятся автоматически.
- Старое поле `topic` останется висеть — нужна manual migration:
  ```sql
  -- если есть legacy данные с метой v1
  ALTER TABLE sensor DROP COLUMN topic;
  ```
- Альтернатива: оставить `topic` как deprecated, заполнять `null` в meta v2.

**Новый Kafka event `DeviceRegistrationMetaEvent.Sensor`** должен включать
новые поля.

**Новый handler** (опционально, не для этой итерации) на
`esp/+/sensor/event` — пока в `karen-device-registration` его нет.
Думаю отдельный сервис `karen-sensor-data` будет consumer'ом этих топиков —
вне scope регистрации.

---

## 9. MAC-фильтр и `requestId`

### 9.1 MAC-фильтр на входящие

Все входящие команды (`*/command`) **должны** проверяться на `macAddress`:

```cpp
const char* mac = doc["macAddress"] | "";
if (strcmp(mac, "*:*:*:*:*:*") != 0 &&
    strcmp(mac, WiFi.macAddress().c_str()) != 0) {
    // не для меня — молча игнорируем
    return;
}
```

Это защита от того, что разные devices могут случайно подписаться на
пересекающиеся топики (например, при wildcard-subscriptions в дашборде).

**Wildcard `"*:*:*:*:*:*"`** — broadcast: отвечают все устройства, которые
получили команду. Используется для discovery.

### 9.2 `requestId` в ответах

Все исходящие `*/event` сообщения **должны** содержать `requestId`, который
был в команде. Это даёт сервису корреляцию запрос ↔ ответ. Если устройство
шлёт event инициативно (не в ответ на команду) — `requestId` опускается или
ставится `null`.

---

## 10. `KarenDevicePresence` — публичный API (предложение)

> Это **draft** API новой библиотеки. Не реализован, требует ревью.

### 10.1 Header (черновик)

```cpp
#pragma once

#include "KarenMqttCore.h"

namespace karen {
namespace presence {

struct DeviceIdentity {
    const char* deviceId;
    const char* macAddress;
    const char* bootId;
    const char* firmwareVersion;
    const char* metaFormat;
    const char* boardRevision;
    const char* mcu;
};

struct SensorDescriptor {
    const char* address;
    const char* type;       // "temperature"
    const char* unit;       // "°C"
    const char* subtype;    // "ds18b20"
};

struct SwitchDescriptor {
    uint16_t    pin;
    uint8_t     switchId;
    bool        inverted;
    const char* subtype;    // "relay"
};

using SensorReader  = std::function<bool(const char* address, /*out*/ float& value)>;
using SwitchAction  = std::function<bool(uint8_t switchId, bool state)>;
using MetaCommandHook = std::function<void(const char* requestId)>;

class KarenDevicePresence {
public:
    KarenDevicePresence(KarenMqttClient& mqtt, KarenMqttTopics& topics, DeviceIdentity id);

    // Builder
    KarenDevicePresence& addSensor(const SensorDescriptor& s);
    KarenDevicePresence& addSwitch(const SwitchDescriptor& s);
    KarenDevicePresence& enableSettings(bool enabled);
    KarenDevicePresence& enableScheduler(bool enabled);
    KarenDevicePresence& enableOta(bool enabled);
    KarenDevicePresence& healthIntervalMs(uint32_t ms);

    // Hooks (вызываются ПОСЛЕ MAC-фильтра и парсинга requestId)
    KarenDevicePresence& onSensorRead(SensorReader  cb);
    KarenDevicePresence& onSwitchAction(SwitchAction cb);
    KarenDevicePresence& onMetaCommand(MetaCommandHook cb);  // optional

    // Lifecycle
    void begin();   // настраивает LWT (через cfg), подписывается, publish ONLINE, sends meta on request
    void loop();    // mqtt.loop() + heartbeat scheduler

    // Manual triggers
    void publishMeta();              // переотправить meta event (без запроса)
    void publishHealth();            // переотправить heartbeat
    void reportError(const char* code, const char* message);

private:
    KarenMqttClient&  mqtt_;
    KarenMqttTopics&  topics_;
    DeviceIdentity    id_;
    std::vector<SensorDescriptor> sensors_;
    std::vector<SwitchDescriptor> switches_;
    // ...
};

} // namespace presence
} // namespace karen
```

### 10.2 Использование (firmware-side, концепт)

```cpp
#include <KarenMqttCore.h>
#include <KarenDevicePresence.h>

WiFiClientSecure netClient;
KarenMqttClient  mqtt(netClient);
KarenMqttTopics  topics(DEVICE_ID);

KarenDevicePresence presence(mqtt, topics, {
    .deviceId        = DEVICE_ID,
    .macAddress      = nullptr,           // заполнится из WiFi.macAddress() внутри begin()
    .bootId          = nullptr,           // сгенерируется внутри
    .firmwareVersion = FIRMWARE_VERSION,
    .metaFormat      = META_FORMAT_VERSION,
    .boardRevision   = BOARD_REVISION,
    .mcu             = "ESP32-C3 WeAct Studio",
});

void setup() {
    // ... wifi, errors handler ...
    netClient.setInsecure();

#if HAS_SENSORS
    presence.addSensor({"28:ff:64:1d:c1:01:23:45", "temperature", "°C", "ds18b20"});
    presence.onSensorRead([](const char* addr, float& val) {
        val = ds18b20.readTempByAddress(addr);
        return !isnan(val);
    });
#endif

#if HAS_SWITCHES
    presence.addSwitch({.pin=4, .switchId=1, .inverted=false, .subtype="relay"});
    presence.onSwitchAction([](uint8_t id, bool state) {
        return relay.set(id, state);
    });
#endif

    presence.enableScheduler(true)
            .healthIntervalMs(30000)
            .begin();   // подписки + LWT + publish ONLINE
}

void loop() {
    wifi.loop();
    presence.loop();
}
```

---

## 11. Изменения в `karen-device-registration` (Phase 2 — deferred)

> ⏸ В Phase 1 (v0.2 этого документа) **ничего не меняем** на стороне сервиса.
> Все пункты ниже — Phase 2.

Сводка для будущей итерации:

| Файл | Изменение | Зачем |
|---|---|---|
| `application.yml: mqtt.topics.health` | `esp/+/health` → `esp/health` | чтобы получать heartbeat от устройств |
| `MqttMessageHandler.java` | подписка на одноуровневый `esp/health`; новый handler `handleSensorEvent` | поддержка pull sensor pattern |
| `model/sensors/Sensor.java` | + `commandTopic`, `eventTopic`, `errorTopic`, `subtype`, `supportedCommands`; удалить `topic` | переход на pull pattern |
| `model/dto/SensorDto.java` | синхронизировать с моделью | |
| `model/dto/kafka/DeviceRegistrationMetaEvent.java` | синхронизировать Sensor DTO | consumers должны увидеть новую структуру |
| `service/DeviceService.java:144-153` | парсить meta v2 в новые поля Sensor | |
| DB migration | `ALTER TABLE sensor` — добавить новые колонки, drop `topic` | |
| `Device.java` | снять `@Column(unique=true)` с `bootId`, оставить unique по `(deviceId, mac_address)` | устранить SQL constraint violation при крайне маловероятной коллизии |
| `config/MqttConfig.java` или `DeviceService.java` | заменить `new ObjectMapper()` на Spring bean + `spring.jackson.deserialization.fail-on-unknown-properties=false` | гибридные/forward-compatible meta форматы |

---

## 12. Open questions (после v0.2)

Решённые в v0.2 (для истории):
- ✅ Java сервис в этой итерации не трогаем.
- ✅ `KarenDevicePresence` стартует с **v1.0.0**.
- ✅ OTA — общий топик `esp/ota/*`, фильтр по MAC+deviceId в payload.
- ✅ `error/event` — `retain=true`.
- ✅ JSON по умолчанию **compact**.
- ✅ `/switch/failed` и `/switch/complete` слиты в общий `switch/event`.
- ✅ Discovery (broadcast) — **не делаем** (сервис и так знает все devices из БД).
- ✅ `bootId` коллизии — mitigation на firmware (MAC tail + random), Java не правим.

Остались:

1. **Sentinel `"000000"`** — стоит ли firmware публиковать что-то особое для первой инициализации, чтобы отличить от штатного reboot? *(Думаю не надо — сервис на первом connect создаёт device, ему всё равно.)*
2. **Кеширование sensor reading на сервисе** — если несколько consumers хотят current temperature, нужен ли middleware-кеш или каждый сам шлёт `READ`? *(Phase 2+ вопрос.)*

---

## 13. Roadmap

### Phase 1 — firmware-side (v0.2 этого документа)

| Шаг | Артефакт | Зависимости |
|---|---|---|
| **0** | Этот документ — ревью, корректировки | — |
| **1** | `KarenMqttCore`: добавить `connect()`, `health()`, `custom(leaf)` в `KarenMqttTopics`; удалить `availability()` | bump KarenMqttCore v0.3.0 (breaking) |
| **2** | Создать новый репо/папку `KarenDevicePresence` (v1.0.0) | KarenMqttCore v0.3.0 |
| **3** | Реализовать `KarenDevicePresence`: builder + handshake + meta builder + heartbeat + MAC-filter + bootId-gen + LWT | step 2 |
| **4** | Имплементация шлёт meta v1.0.0 (compat с сервисом), но кодовый builder уже готов к v2.0.0 | step 3 |
| **5** | Обновить firmware `karen-r1.5.2-fw1.1.0-s6` на использование `KarenDevicePresence` | step 4 |
| **6** | E2E smoke-test: устройство → текущий Java-сервис → БД → REST | всё выше |

### Phase 2 — service catch-up (отложено, отдельный milestone)

| Шаг | Артефакт | Зависимости |
|---|---|---|
| **A** | `karen-device-registration`: подписка на `esp/health` | Phase 1 done |
| **B** | Sensor модель Java + DB migration | A |
| **C** | Handler для `sensor/event` (или передача в отдельный сервис `karen-sensor-data`) | B |
| **D** | Bump `metaFormat` ожидаемого в сервисе до `"2.0.0"`; firmware bumps meta до 2.0.0 | C |
| **E** | Снять `bootId UNIQUE` constraint | A |
| **F** | `ObjectMapper` → Spring bean + ignore-unknown-properties | A |

---

## Приложение A: маппинг старого `MQTTTopics.h` → новый протокол

| Старый макрос | Новый эквивалент | Изменение |
|---|---|---|
| `MQTT_BASE_TOPIC` | `topics.base()` | без изменений |
| `MQTT_SENSOR_DATA` | `topics.event("sensor")` | была push, стала pull-ответ |
| `MQTT_ERROR_DATA` | `topics.event("error")` | без изменений |
| `MQTT_CONNECT_TOPIC` | `topics.connect()` | без изменений (был `/connect`) |
| `MQTT_META_COMMAND_TOPIC` | `topics.command("meta")` | без изменений |
| `MQTT_META_EVENT_TOPIC` | `topics.event("meta")` | без изменений |
| `MQTT_SETTINGS_UPDATE` | `topics.command("setting")` | без изменений |
| `MQTT_SETTINGS_FEEDBACK` | `topics.event("setting")` | без изменений |
| `MQTT_SWITCH_COMMAND` | `topics.command("switch")` | без изменений |
| `MQTT_SWITCH_EVENT` | `topics.event("switch")` | без изменений |
| `MQTT_SWITCH_FAILED` | `topics.event("switch")` с `status:"error"` | **breaking — решено в v0.2** |
| `MQTT_SWITCH_SCHEDULE_COMPLETE` | `topics.event("switch")` с `status:"success", payload.complete:true` | **breaking — решено в v0.2** |

> ✅ Зафиксировано в v0.2: вместо отдельных топиков `/failed` и `/complete`
> используем единый `event`-топик с разным `status`. Это упрощает консумирование
> на стороне сервиса и убирает 2 лишних топика из подписок.

---

## Приложение B: Примеры payload объединённого `switch/event`

В v0.2 все ответы от switch-домена идут на один топик `esp/<id>/switch/event`,
а различаются полями `status` и (опционально) `payload`.

**Успех (был `event` в старой схеме):**

```json
{
  "requestId":  "abc-123",
  "deviceId":   "karen-r1.5.2-fw1.1.0-s6-001",
  "macAddress": "AA:BB:CC:DD:EE:FF",
  "status":     "success",
  "message":    "Switch 1 forced to on"
}
```

**Ошибка (был `failed` в старой схеме):**

```json
{
  "requestId":  "abc-123",
  "deviceId":   "karen-r1.5.2-fw1.1.0-s6-001",
  "macAddress": "AA:BB:CC:DD:EE:FF",
  "status":     "error",
  "errorMessage": "Invalid time format. Expected HH:MM",
  "payload":    { "invalidTime": "25:99" }
}
```

**Расписание выполнено (был `complete`):**

```json
{
  "requestId":  null,
  "deviceId":   "karen-r1.5.2-fw1.1.0-s6-001",
  "macAddress": "AA:BB:CC:DD:EE:FF",
  "status":     "success",
  "event":      "schedule_complete",
  "payload":    { "switchId": 1, "completedAt": 1748602800 }
}
```

> `event` — дискриминатор события для случая «device инициирует» (не ответ
> на команду, поэтому `requestId: null`). Сервис различает по комбинации
> `status` + наличию `event`.
