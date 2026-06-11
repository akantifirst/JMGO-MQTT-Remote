# JMGO MQTT Remote: техническая документация

## Идея проекта

Проект превращает ESP32 в сетевой контроллер для проектора JMGO. Внешнее управление идет через MQTT, а сам адаптер выполняет три основные задачи:

- включает проектор через BLE advertising wake-пакеты;
- управляет интерфейсом проектора по LAN TCP на порту `9005`;
- принимает команды от Home Assistant или терминала через MQTT.

Главная цель: автоматизировать сценарий домашнего кинотеатра, где проектор включается, выбирает HDMI1, а затем Home Assistant выполняет остальные действия: подключение звука, переключение дисплеев, управление жалюзи, светом и столом.

## Текущее состояние архитектуры

Текущая прошивка находится в `src/main.cpp`.

Сейчас используется один основной ESP32-адаптер:

- Wi-Fi подключает адаптер к домашней сети.
- MQTT принимает команды на топике `jmgo/remote/cmd`.
- BLE используется только для отправки wake advertising-пакетов.
- LAN используется для навигации, вызова power menu и полного выключения.
- ArduinoOTA позволяет обновлять прошивку по воздуху.

Проектор находится по адресу:

```text
<projector-ip>:9005
```

MQTT broker:

```text
<mqtt-host>:1883
```

MQTT topics:

```text
jmgo/remote/cmd
jmgo/remote/state
```

## Почему архитектура стала такой

### Первая версия: BLE HID-пульт

Первоначально ESP32 работал как BLE HID keyboard/consumer remote. Он спаривался с проектором как пульт `JMGO-Remote` и отправлял:

- стрелки вверх/вправо;
- `OK`;
- consumer power toggle.

Это позволяло управлять Android TV-интерфейсом, но зависело от BLE pairing. Исходный рабочий код сохранен в `backup/main_backup.cpp`.

Проблема: процедура включения из полного выключения требовала BLE advertising wake-пакетов. Когда тот же адаптер временно переключался из HID advertising в wake advertising, проектор терял pairing с ESP32. После этого навигация через BLE становилась невозможной.

### Вторая версия: два адаптера

Чтобы обойти потерю pairing, wake advertising был вынесен на второй ESPHome-адаптер. Основной адаптер должен был оставаться BLE HID-пультом, а второй адаптер должен был только будить проектор.

Для этого временно был создан отдельный ESPHome wake-код. Он отправлял wake advertising в отдельной FreeRTOS-задаче, чтобы не ронять ESPHome API. Это решение сработало: проектор включался, а API не отваливался.

После перехода на LAN-навигацию этот временный код был удален из проекта, а второй `desktop`-адаптер больше не должен содержать JMGO-блоки.

Но архитектура становилась сложнее:

- два адаптера;
- две прошивки;
- разные точки отказа;
- все еще оставалась зависимость от BLE pairing для навигации.

### Текущая версия: BLE wake + LAN-навигация

Позже было найдено, что приложение JMGO управляет проектором по LAN через TCP-порт `9005`. Из логов были извлечены бинарные команды для кнопок:

- power menu;
- вверх;
- вниз;
- вправо;
- OK.

После этого BLE HID стал не нужен. BLE остался только для включения проектора через wake advertising, а все управление после включения переехало на LAN.

Это решение лучше, потому что:

- больше не нужен BLE pairing;
- навигация не ломается после wake;
- можно полностью выключать проектор через power menu;
- команды можно отправлять независимо от HID-профиля;
- один адаптер снова может выполнять весь основной сценарий.

## Команды MQTT

Команды отправляются в топик:

```text
jmgo/remote/cmd
```

Примеры:

```bash
mosquitto_pub -h <mqtt-host> -p 1883 -u <mqtt-user> -P '<password>' -t jmgo/remote/cmd -m on
mosquitto_pub -h <mqtt-host> -p 1883 -u <mqtt-user> -P '<password>' -t jmgo/remote/cmd -m hdmi1
mosquitto_pub -h <mqtt-host> -p 1883 -u <mqtt-user> -P '<password>' -t jmgo/remote/cmd -m power_off
```

Поддерживаемые команды:

| Команда | Что делает |
| --- | --- |
| `wake` | Только BLE wake advertising, без навигации |
| `on` | Wake advertising, ожидание загрузки, затем переход на HDMI1 |
| `wake_hdmi1` | То же, что `on` |
| `power_on` | То же, что `on` |
| `hdmi1` | Только LAN-навигация до HDMI1 |
| `power_menu` | Открывает меню питания проектора |
| `power_off` | Полное выключение: power menu, вниз, OK |
| `off` | То же, что `power_off` |
| `up` | LAN-команда вверх |
| `down` | LAN-команда вниз |
| `right` | LAN-команда вправо |
| `ok` | LAN-команда OK |
| `enter` | То же, что `ok` |

Статусы публикуются в:

```text
jmgo/remote/state
```

Примеры статусов:

- `online`;
- `wake_burst_start`;
- `wake_burst_done`;
- `power_on_wait`;
- `hdmi1_start`;
- `hdmi1_done`;
- `power_off_start`;
- `power_off_done`;
- `projector_lan_connect_failed`;
- `projector_lan_write_failed`.

## Как работает wake advertising

Включение проектора из полного выключения реализовано через BLE advertising manufacturer data. В коде это массив `WAKE_PAYLOADS`.

Функция:

```cpp
sendWakeAdvertisingBurst()
```

делает следующее:

1. публикует MQTT-статус `wake_burst_start`;
2. циклически отправляет пять wake payloads;
3. каждый payload рекламируется `WAKE_PAYLOAD_ADV_MS`;
4. между payloads выдерживается `WAKE_PAYLOAD_GAP_MS`;
5. общий burst длится `WAKE_TOTAL_MS`;
6. advertising останавливается;
7. публикуется `wake_burst_done`.

BLE-инициализация находится в:

```cpp
startBleWakeAdvertiser()
```

Адаптер больше не создает BLE HID-сервис и не пытается спариваться с проектором.

## Как работает LAN-управление

LAN-команды отправляются как raw bytes в TCP-соединение на проектор:

```cpp
PROJECTOR_IP
PROJECTOR_PORT
```

Каждая кнопка состоит из двух пакетов:

- press;
- release.

Например, `OK`:

```cpp
LAN_OK_PRESS
LAN_OK_RELEASE
```

Абстракция кнопки:

```cpp
struct LanKey {
  const char* name;
  const uint8_t* press;
  size_t pressLen;
  const uint8_t* release;
  size_t releaseLen;
};
```

Отправка одной кнопки:

```cpp
tapLanKey(KEY_OK, delayAfterMs)
```

Логика:

1. открыть TCP-соединение;
2. отправить press;
3. закрыть соединение;
4. подождать `LAN_KEY_DELAY_MS`;
5. открыть TCP-соединение;
6. отправить release;
7. закрыть соединение;
8. подождать задержку после кнопки.

Такой подход прост и надежен: каждое действие независимо, а зависшее соединение не ломает всю последовательность.

## Макрос HDMI1

Функция:

```cpp
macroHdmi1Lan()
```

Текущая последовательность:

1. `UP`;
2. `UP`;
3. пауза `HDMI_AFTER_UPS_DELAY_MS`;
4. `RIGHT` четыре раза;
5. пауза `HDMI_AFTER_RIGHTS_DELAY_MS`;
6. `OK`;
7. пауза `HDMI_AFTER_FIRST_OK_DELAY_MS`;
8. `OK`;
9. статус `hdmi1_done`.

Смысл задержек:

| Константа | Назначение |
| --- | --- |
| `HDMI_AFTER_UPS_DELAY_MS` | Пауза после выхода в верхнюю навигационную область |
| `HDMI_RIGHT_STEP_DELAY_MS` | Пауза между нажатиями вправо |
| `HDMI_AFTER_RIGHTS_DELAY_MS` | Пауза перед открытием `Inputs` |
| `HDMI_AFTER_FIRST_OK_DELAY_MS` | Пауза после открытия `Inputs`, перед выбором HDMI1 |
| `HDMI_AFTER_SECOND_OK_DELAY_MS` | Пауза после финального OK |

Если отдельная команда `hdmi1` работает, но команда `on` после холодного старта не всегда попадает в HDMI1, проблема обычно не в самом макросе, а в готовности Android TV после включения. В этом случае настраивается `POWER_ON_TO_HDMI_DELAY_MS` или добавляется стратегия повторных попыток.

## Макрос включения

Функция:

```cpp
macroPowerOnHdmi1()
```

Текущая последовательность:

1. статус `power_on_start`;
2. BLE wake advertising burst;
3. статус `power_on_wait`;
4. ожидание `POWER_ON_TO_HDMI_DELAY_MS`;
5. запуск `macroHdmi1Lan()`.

Главная настройка здесь:

```cpp
POWER_ON_TO_HDMI_DELAY_MS
```

Это задержка между включением проектора и стартом навигации к HDMI1.

## Макрос полного выключения

Функция:

```cpp
macroPowerOffLan()
```

Текущая последовательность:

1. `KEY_POWER_MENU`;
2. пауза `LAN_POWER_OFF_STEP_DELAY_MS`;
3. `KEY_DOWN`;
4. пауза `LAN_POWER_OFF_STEP_DELAY_MS`;
5. `KEY_OK`;
6. статус `power_off_done`.

Это отличается от старой BLE-команды `power`, которая просто отправляла toggle и уводила проектор в sleep. Новый вариант открывает меню питания и выбирает полное выключение.

## OTA-обновление

В прошивке используется ArduinoOTA.

Настройки:

```cpp
OTA_HOST = "jmgo-remote"
OTA_PASS = "<password>"
```

В `platformio.ini` есть отдельное окружение:

```ini
[env:esp32ota]
upload_protocol = espota
upload_port = <esp32-ip>
upload_flags =
  --auth=<ota-password>
```

Команда для OTA-прошивки:

```bash
pio run -e esp32ota -t upload
```

Команда для сборки без прошивки:

```bash
pio run -e esp32dev
```

Если команда `pio` не находится, нужно использовать полный путь выше или добавить PlatformIO в `PATH`.

## Home Assistant

Home Assistant должен отправлять MQTT-команды в `jmgo/remote/cmd`.

Пример script entity для включения:

```yaml
alias: Beamer on
sequence:
  - action: mqtt.publish
    data:
      topic: jmgo/remote/cmd
      payload: "on"
      qos: 0
      retain: false
mode: single
```

Пример script entity для полного выключения:

```yaml
alias: Beamer off
sequence:
  - action: mqtt.publish
    data:
      topic: jmgo/remote/cmd
      payload: "power_off"
      qos: 0
      retain: false
mode: single
```

Если YAML вставляется в UI-редактор конкретной автоматизации или скрипта, обычно не нужно добавлять верхний ключ `script:` или `automation:`. Если файл подключается через `scripts.yaml` или `automations.yaml`, формат может отличаться.

## Настройка с нуля или после замены адаптера

### 1. Проверить IP-адреса

В `src/main.cpp` проверить:

```cpp
MQTT_HOST
PROJECTOR_IP
PROJECTOR_PORT
```

В `platformio.ini` проверить:

```ini
upload_port
monitor_port
```

для USB и:

```ini
[env:esp32ota]
upload_port
```

для OTA.

Если новый адаптер получил другой IP, нужно обновить `upload_port` в `[env:esp32ota]`.

### 2. Проверить Wi-Fi и MQTT

В `src/main.cpp` проверить:

```cpp
WIFI_SSID
WIFI_PASS
MQTT_USER
MQTT_PASS
```

После прошивки адаптер должен опубликовать:

```text
jmgo/remote/state online
```

Проверка через терминал:

```bash
mosquitto_sub -h <mqtt-host> -p 1883 -u <mqtt-user> -P '<password>' -t jmgo/remote/state -v
```

### 3. Проверить wake

```bash
mosquitto_pub -h <mqtt-host> -p 1883 -u <mqtt-user> -P '<password>' -t jmgo/remote/cmd -m wake
```

Если проектор включается, BLE wake advertising работает.

### 4. Проверить LAN-кнопки

Когда проектор включен:

```bash
mosquitto_pub -h <mqtt-host> -p 1883 -u <mqtt-user> -P '<password>' -t jmgo/remote/cmd -m right
mosquitto_pub -h <mqtt-host> -p 1883 -u <mqtt-user> -P '<password>' -t jmgo/remote/cmd -m ok
```

Если интерфейс реагирует, LAN-команды работают.

Если не реагирует:

- проверить IP проектора;
- проверить, что проектор в той же сети;
- проверить порт `9005`;
- убедиться, что проектор уже загрузился.

### 5. Проверить HDMI1-макрос

```bash
mosquitto_pub -h <mqtt-host> -p 1883 -u <mqtt-user> -P '<password>' -t jmgo/remote/cmd -m hdmi1
```

Если `hdmi1` работает при уже включенном проекторе, но не работает после `on`, настраивать нужно в первую очередь:

```cpp
POWER_ON_TO_HDMI_DELAY_MS
```

Если `hdmi1` не работает даже на включенном проекторе, настраивать:

```cpp
HDMI_AFTER_UPS_DELAY_MS
HDMI_RIGHT_STEP_DELAY_MS
HDMI_AFTER_RIGHTS_DELAY_MS
HDMI_AFTER_FIRST_OK_DELAY_MS
HDMI_AFTER_SECOND_OK_DELAY_MS
```

### 6. Проверить полное выключение

```bash
mosquitto_pub -h <mqtt-host> -p 1883 -u <mqtt-user> -P '<password>' -t jmgo/remote/cmd -m power_off
```

Ожидаемое поведение:

1. открывается power menu;
2. выбирается пункт полного выключения;
3. подтверждается `OK`.

Если выбирается не тот пункт, нужно проверить порядок меню на проекторе и при необходимости изменить последовательность в `macroPowerOffLan()`.

## Где менять задержки

Все основные задержки вынесены вверх `src/main.cpp`.

LAN:

```cpp
PROJECTOR_LAN_TIMEOUT_MS
LAN_KEY_DELAY_MS
LAN_STEP_DELAY_MS
LAN_POWER_OFF_STEP_DELAY_MS
```

Включение:

```cpp
POWER_ON_TO_HDMI_DELAY_MS
```

HDMI1:

```cpp
HDMI_AFTER_UPS_DELAY_MS
HDMI_RIGHT_STEP_DELAY_MS
HDMI_AFTER_RIGHTS_DELAY_MS
HDMI_AFTER_FIRST_OK_DELAY_MS
HDMI_AFTER_SECOND_OK_DELAY_MS
```

BLE wake:

```cpp
WAKE_TOTAL_MS
WAKE_PAYLOAD_ADV_MS
WAKE_PAYLOAD_GAP_MS
```

## Возможные будущие улучшения

1. Найти LAN-команду прямого выбора HDMI1, чтобы не ходить по Android TV-интерфейсу.
2. Добавить `HOME`, `BACK`, `LEFT` LAN-команды, чтобы сделать HDMI1-макрос более абсолютным.
3. Реализовать несколько ранних попыток HDMI1 после wake вместо одной длинной задержки.
4. Найти более удобный способ передавать OTA-параметры без локального редактирования `platformio.ini`.
5. Найти и задокументировать прямые LAN-команды для других нужных функций проектора.
