#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <DHTesp.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "Wokwi-GUEST"
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif
#ifndef WIFI_CHANNEL
#define WIFI_CHANNEL 6
#endif
#ifndef DEVICE_KEY
#define DEVICE_KEY ""
#endif
#ifndef MQTT_NAME
#define MQTT_NAME "YOUR_NAME"
#endif

const char *API_URL = "http://daley-tea.chilecentral.cloudapp.azure.com";

const char *MQTT_BROKER = "135.232.203.144";
const int MQTT_PORT = 1883;

const unsigned long WIFI_TIMEOUT_MS = 15000;
const unsigned long RETRY_EVERY_MS = 5000;
const unsigned long SEND_EVERY_MS = 10000;
const int FAN_LED_PIN = 2;
const int DHT_PIN = 15;

DHTesp dht;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

String topicStatus, topicSettings, topicState;

unsigned long lastSend = 0;
unsigned long lastAttempt = 0;
float humidity_last = NAN;


bool haveSettings = false;    
String fanMode = "off";
float maxHumidity = 0;

int lastPublishedState = -1;   

bool connectWiFi() {
  Serial.print("Connecting to ");
  Serial.print(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_TIMEOUT_MS) {
      Serial.println();
      Serial.println("No Wi-Fi after 15 s. Trying again in 5 s.");
      return false;
    }
    delay(250);
    Serial.print(".");
  }

  Serial.println(" connected");
  Serial.print("MAC address: ");
  Serial.println(WiFi.macAddress());
  return true;
}

void applyFan() {
  bool wantOn = false;
  if (haveSettings) {
    if (fanMode == "on") {
      wantOn = true;
    } else if (fanMode == "auto") {
      wantOn = !isnan(humidity_last) && humidity_last > maxHumidity;
    }
  }
  digitalWrite(FAN_LED_PIN, wantOn ? HIGH : LOW);

  int state = wantOn ? 1 : 0;
  if (state != lastPublishedState && mqtt.connected()) {
    if (mqtt.publish(topicState.c_str(), wantOn ? "on" : "off", true)) {
      lastPublishedState = state;
      Serial.print("Fan -> ");
      Serial.println(wantOn ? "on" : "off");
    }
  }
}


void onMessage(char *topic, byte *payload, unsigned int length) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.print("Refused settings: not JSON (");
    Serial.print(err.c_str());
    Serial.println(")");
    return;
  }
  if (!doc.is<JsonObject>()) {
    Serial.println("Refused settings: JSON is not an object.");
    return;
  }
  if (!doc["mode"].is<const char *>() || !doc["max_humidity"].is<float>()) {
    Serial.println("Refused settings: a field is missing or has the wrong type.");
    return;
  }

  String mode = doc["mode"].as<String>();
  if (mode != "auto" && mode != "on" && mode != "off") {
    Serial.println("Refused settings: mode must be auto, on or off.");
    return;
  }

  fanMode = mode;
  maxHumidity = doc["max_humidity"].as<float>();
  haveSettings = true;
  Serial.print("Settings accepted: mode=");
  Serial.print(fanMode);
  Serial.print(" max_humidity=");
  Serial.println(maxHumidity);

  applyFan();  
}


bool connectMqtt() {
  String clientId = "fan-" + WiFi.macAddress();
  Serial.print("Connecting to MQTT... ");
  if (!mqtt.connect(clientId.c_str(), nullptr, nullptr,
                    topicStatus.c_str(), 1, true, "offline")) {
    Serial.print("failed, rc=");
    Serial.println(mqtt.state());
    return false;
  }
  Serial.println("connected");
  mqtt.publish(topicStatus.c_str(), "online", true);
  mqtt.subscribe(topicSettings.c_str());
  return true;
}


void sendReading(float humidity) {
  char body[100];
  snprintf(body, sizeof(body), "{\"mac\": \"%s\", \"humidity\": %.1f}",
           WiFi.macAddress().c_str(), humidity);

  HTTPClient http;
  http.begin(String(API_URL) + "/readings");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", DEVICE_KEY);
  int status = http.POST(body);

  Serial.print("POST ");
  Serial.print(body);
  Serial.print(" -> ");
  Serial.println(status);
  if (status == 401) {
    Serial.println("401: copy the MAC above, POST /fans with it, put the key in include/secrets.h.");
  } else if (status == 404) {
    Serial.println("Unknown MAC: register this board with POST /fans first.");
  } else if (status < 0) {
    Serial.println(http.errorToString(status));
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  pinMode(FAN_LED_PIN, OUTPUT);
  digitalWrite(FAN_LED_PIN, LOW);   
  dht.setup(DHT_PIN, DHTesp::DHT22);

  topicStatus = String("ecse3038/") + MQTT_NAME + "/status";
  topicSettings = String("ecse3038/") + MQTT_NAME + "/fan/settings";
  topicState = String("ecse3038/") + MQTT_NAME + "/fan/state";

  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
  mqtt.setCallback(onMessage);

  connectWiFi();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastAttempt >= RETRY_EVERY_MS) {
      lastAttempt = millis();
      connectWiFi();
    }
    return;
  }

  if (mqtt.connected()) {
    mqtt.loop();   
  } else if (millis() - lastAttempt >= RETRY_EVERY_MS) {
    lastAttempt = millis();
    connectMqtt();
  }

  applyFan();      

  if (millis() - lastSend < SEND_EVERY_MS) {
    return;
  }
  lastSend = millis();

  float humidity = dht.getHumidity();
  if (isnan(humidity)) {
    Serial.println("No reading from the sensor. Check the three wires.");
    return;
  }
  humidity_last = humidity;
  sendReading(humidity);
  applyFan();
}