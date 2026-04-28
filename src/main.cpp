#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <esp_wifi.h>

// ESP32-WROOM-32D does NOT have a built-in LED.
// Connect an external LED to GPIO 2:
// GPIO 2 ---[220Ω Resistor]---|>|--- GND
//                              LED (long leg = +)
#define LED_PIN 2

// ----- Compile-time configuration (override via platformio.ini build_flags) -----
#ifndef WIFI_SSID
#define WIFI_SSID "reokto-net"
#endif
#ifndef WIFI_PSK
#define WIFI_PSK "reokto-pass-change-me"
#endif
#ifndef MQTT_HOST
#define MQTT_HOST "10.42.0.1"
#endif
#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif
#ifndef MQTT_CLIENT_ID
#define MQTT_CLIENT_ID "reokto-esp32"
#endif

static constexpr int FREQ_MIN = 1;
static constexpr int FREQ_MAX = 50;
static constexpr int FREQ_DEFAULT = 20;

static constexpr const char* TOPIC_FREQ_SET = "led/freq/set";
static constexpr const char* TOPIC_FREQ_STATE = "led/freq/state";
static constexpr const char* TOPIC_STATUS = "led/status";

// ----- Runtime state -----
WiFiClient g_wifi;
PubSubClient g_mqtt(g_wifi);

volatile int g_freq_hz = FREQ_DEFAULT;
unsigned long g_half_period_ms = 1000UL / (FREQ_DEFAULT * 2);
unsigned long g_last_toggle_ms = 0;
bool g_led_on = false;

unsigned long g_last_mqtt_attempt_ms = 0;

static int clampFreq(int hz) {
  if (hz < FREQ_MIN) return FREQ_MIN;
  if (hz > FREQ_MAX) return FREQ_MAX;
  return hz;
}

static void setFrequency(int hz, bool publish_state) {
  hz = clampFreq(hz);
  g_freq_hz = hz;
  // Half-period in ms; floor-rounded but >= 1ms so we never spin.
  unsigned long hp = 1000UL / (static_cast<unsigned long>(hz) * 2UL);
  if (hp == 0) hp = 1;
  g_half_period_ms = hp;

  Serial.printf("[led] frequency = %d Hz (half-period %lums)\n", hz, hp);

  if (publish_state && g_mqtt.connected()) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", hz);
    g_mqtt.publish(TOPIC_FREQ_STATE, buf, true);
  }
}

static void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, TOPIC_FREQ_SET) != 0) return;

  // PubSubClient hands us a non-NUL-terminated buffer; copy to a small
  // stack buffer so we can strtol it.
  char buf[16];
  unsigned int n = length < sizeof(buf) - 1 ? length : sizeof(buf) - 1;
  memcpy(buf, payload, n);
  buf[n] = '\0';

  char* end = nullptr;
  long val = strtol(buf, &end, 10);
  if (end == buf) {
    Serial.printf("[mqtt] ignoring non-integer freq payload '%s'\n", buf);
    return;
  }
  setFrequency(static_cast<int>(val), /*publish_state=*/true);
}

static const char* authModeStr(wifi_auth_mode_t a) {
  switch (a) {
    case WIFI_AUTH_OPEN: return "open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-EAP";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
    default: return "?";
  }
}

static void scanForTarget() {
  Serial.println("[wifi] scanning...");
  int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/true);
  bool found = false;
  for (int i = 0; i < n; ++i) {
    bool match = WiFi.SSID(i) == WIFI_SSID;
    Serial.printf("  %s%-32s ch=%d rssi=%d auth=%s\n",
                  match ? "* " : "  ",
                  WiFi.SSID(i).c_str(),
                  WiFi.channel(i),
                  WiFi.RSSI(i),
                  authModeStr(WiFi.encryptionType(i)));
    if (match) found = true;
  }
  if (!found) {
    Serial.printf("[wifi] target SSID '%s' NOT visible in scan\n", WIFI_SSID);
  }
  WiFi.scanDelete();
}

static void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.printf("\n[wifi] associated, ch=%u\n",
                    info.wifi_sta_connected.channel);
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("\n[wifi] disconnected, reason=%u\n",
                    info.wifi_sta_disconnected.reason);
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[wifi] got ip=%s\n",
                    WiFi.localIP().toString().c_str());
      break;
    default:
      break;
  }
}

static void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, true);
  WiFi.onEvent(onWifiEvent);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  // Worldwide regulatory domain (channels 1-13). Stops the ESP32 from
  // silently ignoring an AP placed on channels 12/13.
  wifi_country_t country = {
      .cc = "01",
      .schan = 1,
      .nchan = 13,
      .max_tx_power = 20,
      .policy = WIFI_COUNTRY_POLICY_MANUAL,
  };
  esp_wifi_set_country(&country);

  scanForTarget();

  Serial.printf("[wifi] connecting to '%s'", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PSK);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print('.');
    if (millis() - start > 30000UL) {
      Serial.printf("\n[wifi] connect timeout (status=%d), restarting\n",
                    WiFi.status());
      ESP.restart();
    }
  }
  Serial.printf("\n[wifi] connected, ip=%s rssi=%d\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI());
}

static bool connectMqttOnce() {
  Serial.printf("[mqtt] connecting to %s:%d ...\n", MQTT_HOST, MQTT_PORT);
  // LWT: retained "offline" on TOPIC_STATUS so the dashboard sees us
  // disappear within keepalive.
  bool ok = g_mqtt.connect(
      MQTT_CLIENT_ID,
      /*user=*/nullptr, /*pass=*/nullptr,
      TOPIC_STATUS, /*willQos=*/1, /*willRetain=*/true, "offline");
  if (!ok) {
    Serial.printf("[mqtt] connect failed, state=%d\n", g_mqtt.state());
    return false;
  }
  Serial.println("[mqtt] connected");

  g_mqtt.publish(TOPIC_STATUS, "online", true);
  // Publish current frequency so the dashboard reflects reality.
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", g_freq_hz);
  g_mqtt.publish(TOPIC_FREQ_STATE, buf, true);

  g_mqtt.subscribe(TOPIC_FREQ_SET, 1);
  return true;
}

static void serviceMqtt() {
  if (g_mqtt.connected()) {
    g_mqtt.loop();
    return;
  }
  // Reconnect with backoff so we don't burn the CPU between attempts.
  unsigned long now = millis();
  if (now - g_last_mqtt_attempt_ms < 2000UL) return;
  g_last_mqtt_attempt_ms = now;
  connectMqttOnce();
}

static void serviceBlink() {
  unsigned long now = millis();
  if (now - g_last_toggle_ms < g_half_period_ms) return;
  g_last_toggle_ms = now;
  g_led_on = !g_led_on;
  digitalWrite(LED_PIN, g_led_on ? HIGH : LOW);
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.println("\nESP32 reokto-demo: MQTT-controlled blink");
  Serial.printf("LED on GPIO %d, default %d Hz\n", LED_PIN, FREQ_DEFAULT);

  setFrequency(FREQ_DEFAULT, /*publish_state=*/false);

  connectWifi();

  g_mqtt.setServer(MQTT_HOST, MQTT_PORT);
  g_mqtt.setCallback(onMqttMessage);
  g_mqtt.setKeepAlive(15);
  g_mqtt.setBufferSize(256);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    // Still blink while we wait for WiFi to come back, but defer MQTT.
    serviceBlink();
    delay(50);
    return;
  }
  serviceMqtt();
  serviceBlink();
}
