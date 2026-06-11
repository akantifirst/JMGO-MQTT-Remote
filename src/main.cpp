#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>

#include <NimBLEDevice.h>

#include "secrets.h"

// ===================== USER CONFIG =====================
static const char* WIFI_SSID = SECRET_WIFI_SSID;
static const char* WIFI_PASS = SECRET_WIFI_PASS;
static const char* MQTT_USER = SECRET_MQTT_USER;
static const char* MQTT_PASS = SECRET_MQTT_PASS;
static const char* OTA_HOST  = SECRET_OTA_HOST;
static const char* OTA_PASS  = SECRET_OTA_PASS;

static const char* MQTT_HOST = SECRET_MQTT_HOST;
static const uint16_t MQTT_PORT = SECRET_MQTT_PORT;

static IPAddress PROJECTOR_IP(SECRET_PROJECTOR_IP_OCTETS);
static const uint16_t PROJECTOR_PORT = SECRET_PROJECTOR_PORT;

// MQTT topics
static const char* TOPIC_CMD   = "jmgo/remote/cmd";
static const char* TOPIC_STATE = "jmgo/remote/state";

// Timings: LAN projector commands
static const uint32_t PROJECTOR_LAN_TIMEOUT_MS = 3000;
static const uint32_t LAN_KEY_DELAY_MS = 120;
static const uint32_t LAN_STEP_DELAY_MS = 120;
static const uint32_t LAN_OK_DELAY_MS = 2000;
static const uint32_t LAN_POWER_OFF_STEP_DELAY_MS = 250;
static const uint32_t POWER_ON_TO_HDMI_DELAY_MS = 60000;

// Timings: HDMI1 macro tuning
static const uint32_t HDMI_AFTER_UPS_DELAY_MS = 50;
static const uint32_t HDMI_RIGHT_STEP_DELAY_MS = 300;
static const uint32_t HDMI_AFTER_RIGHTS_DELAY_MS = 2000;
static const uint32_t HDMI_AFTER_FIRST_OK_DELAY_MS = 2000;
static const uint32_t HDMI_AFTER_SECOND_OK_DELAY_MS = 0;

// Timings: BLE wake advertising
static const uint32_t WAKE_TOTAL_MS = 4000;
static const uint32_t WAKE_PAYLOAD_ADV_MS = 250;
static const uint32_t WAKE_PAYLOAD_GAP_MS = 30;

// Timings: Wi-Fi, MQTT, startup
static const uint32_t STARTUP_DELAY_MS = 3000;
static const uint32_t WIFI_CONNECT_POLL_MS = 300;
static const uint32_t WIFI_CONNECT_TIMEOUT_MS = 20000;
static const uint32_t MQTT_RECONNECT_DELAY_MS = 2000;

// ===================== MQTT =====================
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
static bool otaStarted = false;

// ===================== JMGO BLE wake advertising =====================
static const uint8_t WAKE_PAYLOADS[][14] = {
  {0x46, 0x00, 0x01, 0x69, 0xc5, 0x98, 0xc4, 0xd8, 0xe0, 0xff, 0xff, 0xff, 0xff, 0xff},
  {0x46, 0x00, 0x02, 0x69, 0xc5, 0x98, 0xc4, 0xd8, 0xe0, 0xff, 0xff, 0xff, 0xff, 0xff},
  {0x46, 0x00, 0x03, 0x69, 0xc5, 0x98, 0xc4, 0xd8, 0xe0, 0xff, 0xff, 0xff, 0xff, 0xff},
  {0x46, 0x00, 0x04, 0x69, 0xc5, 0x98, 0xc4, 0xd8, 0xe0, 0xff, 0xff, 0xff, 0xff, 0xff},
  {0x46, 0x00, 0x05, 0x69, 0xc5, 0x98, 0xc4, 0xd8, 0xe0, 0xff, 0xff, 0xff, 0xff, 0xff},
};

static const int WAKE_PAYLOAD_COUNT = sizeof(WAKE_PAYLOADS) / sizeof(WAKE_PAYLOADS[0]);

// ===================== JMGO LAN packets =====================
static const uint8_t LAN_POWER_MENU_PRESS[]   = {0x09, 0x12, 0x07, 0x0a, 0x05, 0x08, 0xdb, 0x0f, 0x10, 0x01};
static const uint8_t LAN_POWER_MENU_RELEASE[] = {0x09, 0x12, 0x07, 0x0a, 0x05, 0x08, 0xdb, 0x0f, 0x10, 0x00};

static const uint8_t LAN_UP_PRESS[]    = {0x08, 0x12, 0x06, 0x0a, 0x04, 0x08, 0x13, 0x10, 0x01};
static const uint8_t LAN_UP_RELEASE[]  = {0x08, 0x12, 0x06, 0x0a, 0x04, 0x08, 0x13, 0x10, 0x00};
static const uint8_t LAN_DOWN_PRESS[]  = {0x08, 0x12, 0x06, 0x0a, 0x04, 0x08, 0x14, 0x10, 0x01};
static const uint8_t LAN_DOWN_RELEASE[]= {0x08, 0x12, 0x06, 0x0a, 0x04, 0x08, 0x14, 0x10, 0x00};
static const uint8_t LAN_RIGHT_PRESS[] = {0x08, 0x12, 0x06, 0x0a, 0x04, 0x08, 0x16, 0x10, 0x01};
static const uint8_t LAN_RIGHT_RELEASE[]={0x08, 0x12, 0x06, 0x0a, 0x04, 0x08, 0x16, 0x10, 0x00};
static const uint8_t LAN_OK_PRESS[]    = {0x08, 0x12, 0x06, 0x0a, 0x04, 0x08, 0x17, 0x10, 0x01};
static const uint8_t LAN_OK_RELEASE[]  = {0x08, 0x12, 0x06, 0x0a, 0x04, 0x08, 0x17, 0x10, 0x00};

struct LanKey {
  const char* name;
  const uint8_t* press;
  size_t pressLen;
  const uint8_t* release;
  size_t releaseLen;
};

static const LanKey KEY_POWER_MENU = {"power_menu", LAN_POWER_MENU_PRESS, sizeof(LAN_POWER_MENU_PRESS), LAN_POWER_MENU_RELEASE, sizeof(LAN_POWER_MENU_RELEASE)};
static const LanKey KEY_UP         = {"up",         LAN_UP_PRESS,         sizeof(LAN_UP_PRESS),         LAN_UP_RELEASE,         sizeof(LAN_UP_RELEASE)};
static const LanKey KEY_DOWN       = {"down",       LAN_DOWN_PRESS,       sizeof(LAN_DOWN_PRESS),       LAN_DOWN_RELEASE,       sizeof(LAN_DOWN_RELEASE)};
static const LanKey KEY_RIGHT      = {"right",      LAN_RIGHT_PRESS,      sizeof(LAN_RIGHT_PRESS),      LAN_RIGHT_RELEASE,      sizeof(LAN_RIGHT_RELEASE)};
static const LanKey KEY_OK         = {"ok",         LAN_OK_PRESS,         sizeof(LAN_OK_PRESS),         LAN_OK_RELEASE,         sizeof(LAN_OK_RELEASE)};

static inline void waitMs(uint32_t ms) { delay(ms); }

static void connectWiFi();
static void ensureOtaReady();
static void mqttEnsureConnected();

static void publishState(const char* state, bool retained = true) {
  mqtt.publish(TOPIC_STATE, state, retained);
}

static void advertiseWakePayload(const uint8_t* payload, size_t len, uint32_t durationMs) {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();

  NimBLEAdvertisementData advData;
  advData.setManufacturerData(std::string((const char*)payload, len));

  adv->stop();
  adv->setAdvertisementData(advData);
  adv->setScanResponseData(NimBLEAdvertisementData());
  adv->setMinInterval(0x20);
  adv->setMaxInterval(0x40);
  adv->start();

  waitMs(durationMs);
  adv->stop();
  waitMs(WAKE_PAYLOAD_GAP_MS);
}

static void sendWakeAdvertisingBurst() {
  Serial.printf("Sending JMGO wake burst for %lu ms...\n", WAKE_TOTAL_MS);
  publishState("wake_burst_start");

  uint32_t start = millis();
  while (millis() - start < WAKE_TOTAL_MS) {
    for (int i = 0; i < WAKE_PAYLOAD_COUNT; i++) {
      advertiseWakePayload(WAKE_PAYLOADS[i], sizeof(WAKE_PAYLOADS[i]), WAKE_PAYLOAD_ADV_MS);
      if (millis() - start >= WAKE_TOTAL_MS) break;
    }
  }

  NimBLEDevice::getAdvertising()->stop();
  Serial.println("Wake burst finished.");
  publishState("wake_burst_done");
}

static bool sendRawLan(const uint8_t* data, size_t len) {
  WiFiClient client;
  client.setTimeout(PROJECTOR_LAN_TIMEOUT_MS);

  if (!client.connect(PROJECTOR_IP, PROJECTOR_PORT)) {
    Serial.println("Projector LAN connect failed");
    publishState("projector_lan_connect_failed");
    return false;
  }

  size_t written = client.write(data, len);
  client.flush();
  client.stop();

  if (written != len) {
    Serial.println("Projector LAN write failed");
    publishState("projector_lan_write_failed");
    return false;
  }

  return true;
}

static bool tapLanKey(const LanKey& key, uint32_t afterDelayMs = LAN_STEP_DELAY_MS) {
  Serial.print("LAN key: ");
  Serial.println(key.name);

  if (!sendRawLan(key.press, key.pressLen)) return false;
  waitMs(LAN_KEY_DELAY_MS);
  if (!sendRawLan(key.release, key.releaseLen)) return false;
  waitMs(afterDelayMs);

  return true;
}

static void macroHdmi1Lan() {
  publishState("hdmi1_start");

  tapLanKey(KEY_UP);
  tapLanKey(KEY_UP);
  waitMs(HDMI_AFTER_UPS_DELAY_MS);

  for (int i = 0; i < 4; i++) {
    tapLanKey(KEY_RIGHT, HDMI_RIGHT_STEP_DELAY_MS);
  }
  waitMs(HDMI_AFTER_RIGHTS_DELAY_MS);

  tapLanKey(KEY_OK, HDMI_AFTER_FIRST_OK_DELAY_MS);
  tapLanKey(KEY_OK, HDMI_AFTER_SECOND_OK_DELAY_MS);
  tapLanKey(KEY_OK, HDMI_AFTER_SECOND_OK_DELAY_MS);
  
  publishState("hdmi1_done");
}

static void macroPowerOffLan() {
  publishState("power_off_start");

  tapLanKey(KEY_POWER_MENU, LAN_POWER_OFF_STEP_DELAY_MS);
  tapLanKey(KEY_DOWN, LAN_POWER_OFF_STEP_DELAY_MS);
  tapLanKey(KEY_OK, 0);

  publishState("power_off_done");
}

static void macroPowerOnHdmi1() {
  publishState("power_on_start");
  sendWakeAdvertisingBurst();

  Serial.printf("Waiting %lu ms before HDMI1 macro...\n", POWER_ON_TO_HDMI_DELAY_MS);
  publishState("power_on_wait");
  waitMs(POWER_ON_TO_HDMI_DELAY_MS);

  macroHdmi1Lan();
}

// ===================== BLE init =====================
static void startBleWakeAdvertiser() {
  NimBLEDevice::init("JMGO-Wake");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::getAdvertising()->stop();
}

// ===================== Wi-Fi + MQTT =====================
static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("Wi-Fi connecting");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(WIFI_CONNECT_POLL_MS);
    Serial.print(".");
    if (millis() - t0 > WIFI_CONNECT_TIMEOUT_MS) break;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi OK, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wi-Fi FAIL");
  }
}

static void ensureOtaReady() {
  if (WiFi.status() != WL_CONNECTED || otaStarted) return;

  ArduinoOTA.setHostname(OTA_HOST);
  ArduinoOTA.setPassword(OTA_PASS);

  ArduinoOTA.onStart([]() {
    const char* type = ArduinoOTA.getCommand() == U_FLASH ? "sketch" : "filesystem";
    Serial.printf("OTA start: %s\n", type);
    publishState("ota_start");
  });

  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA done");
    publishState("ota_done");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    static uint8_t lastPct = 255;
    uint8_t pct = total ? (progress * 100U) / total : 0;
    if (pct != lastPct && (pct % 10 == 0 || pct == 100)) {
      Serial.printf("OTA progress: %u%%\n", pct);
      lastPct = pct;
    }
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA error[%u]\n", error);
    publishState("ota_error");
  });

  ArduinoOTA.begin();
  otaStarted = true;

  Serial.print("OTA ready: ");
  Serial.println(OTA_HOST);
  Serial.print("OTA IP: ");
  Serial.println(WiFi.localIP());
}

static void mqttCallback(char* topic, byte* payload, unsigned int len) {
  String t(topic);
  String msg;
  msg.reserve(len);
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];
  msg.trim();
  msg.toLowerCase();

  Serial.print("MQTT: "); Serial.print(t); Serial.print(" -> "); Serial.println(msg);

  if (t != TOPIC_CMD) return;

  if (msg == "wake") {
    sendWakeAdvertisingBurst();
  } else if (msg == "on" || msg == "wake_hdmi1" || msg == "power_on") {
    macroPowerOnHdmi1();
  } else if (msg == "hdmi1") {
    macroHdmi1Lan();
  } else if (msg == "power_menu") {
    publishState("power_menu_start");
    tapLanKey(KEY_POWER_MENU, 0);
    publishState("power_menu_done");
  } else if (msg == "power_off" || msg == "off") {
    macroPowerOffLan();
  } else if (msg == "up") {
    tapLanKey(KEY_UP);
  } else if (msg == "down") {
    tapLanKey(KEY_DOWN);
  } else if (msg == "right") {
    tapLanKey(KEY_RIGHT);
  } else if (msg == "ok" || msg == "enter") {
    tapLanKey(KEY_OK);
  } else {
    publishState("unknown_cmd");
  }
}

static void mqttEnsureConnected() {
  if (mqtt.connected()) return;

  while (!mqtt.connected()) {
    Serial.print("MQTT connecting...");
    String clientId = "jmgo-remote-" + String((uint32_t)ESP.getEfuseMac(), HEX);

    bool ok = mqtt.connect(clientId.c_str(), MQTT_USER, MQTT_PASS);

    if (ok) {
      Serial.println("OK");
      mqtt.subscribe(TOPIC_CMD);
      publishState("online");
    } else {
      Serial.print("FAIL rc=");
      Serial.println(mqtt.state());
      delay(MQTT_RECONNECT_DELAY_MS);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(STARTUP_DELAY_MS);

  Serial.println();
  Serial.println("========================================");
  Serial.println("=== JMGO LAN + BLE WAKE REMOTE ===");
  Serial.printf("Build: %s %s\n", __DATE__, __TIME__);
  Serial.printf("Chip rev: %d, heap: %u\n", ESP.getChipRevision(), ESP.getFreeHeap());
  Serial.println("========================================");
  Serial.flush();

  Serial.println("Starting BLE wake advertiser...");
  startBleWakeAdvertiser();

  Serial.println("Starting WiFi...");
  connectWiFi();
  ensureOtaReady();

  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);
  mqttEnsureConnected();

  Serial.println("Ready. Publish 'wake', 'on', 'hdmi1', 'power_menu', 'power_off', 'up', 'down', 'right', or 'ok' to jmgo/remote/cmd");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    otaStarted = false;
    connectWiFi();
    ensureOtaReady();
  }
  mqttEnsureConnected();
  mqtt.loop();
  ArduinoOTA.handle();
}
