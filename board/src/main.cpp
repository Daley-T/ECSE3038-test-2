#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <DHTesp.h>
#include "secrets.h"   // defines API_KEY; never committed

#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASSWORD ""
#define WIFI_CHANNEL 6

const char *API_URL = "http://daley-tea.chilecentral.cloudapp.azure.com";

const int DHT_PIN = 15;
const unsigned long SEND_EVERY_MS = 10000;
const unsigned long WIFI_TIMEOUT_MS = 15000;

DHTesp dht;
unsigned long lastSend = 0;

bool connectWiFi() {
  Serial.print("Connecting to ");
  Serial.print(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, WIFI_CHANNEL);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_TIMEOUT_MS) {
      Serial.println();
      Serial.println("No Wi-Fi after 15 s. Check the name and the password.");
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

void checkServer() {
  HTTPClient http;
  http.begin(String(API_URL) + "/fans");
  int status = http.GET();

  Serial.print("GET /fans -> ");
  Serial.println(status);
  if (status > 0) {
    Serial.println(http.getString());
  } else {
    Serial.println(http.errorToString(status));   // no answer at all
  }
  http.end();
}

void sendReading(float humidity) {
  char body[80];
  snprintf(body, sizeof(body), "{\"mac\": \"%s\", \"humidity\": %.1f}",
           WiFi.macAddress().c_str(), humidity);

  HTTPClient http;
  http.begin(String(API_URL) + "/readings");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);
  int status = http.POST(body);

  Serial.print("POST ");
  Serial.print(body);
  Serial.print(" -> ");
  Serial.println(status);
  if (status == 401) {
    Serial.println("Key missing or unknown: check API_KEY in include/secrets.h.");
  } else if (status == 403) {
    Serial.println("This key belongs to another fan.");
  } else if (status == 404) {
    Serial.println("Unknown MAC: register this board with POST /fans first.");
  } else if (status < 0) {
    Serial.println(http.errorToString(status));
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  dht.setup(DHT_PIN, DHTesp::DHT22);

  if (connectWiFi()) {
    checkServer();
  }
}

void loop() {
  if (millis() - lastSend < SEND_EVERY_MS) {
    return;
  }
  lastSend = millis();

  float humidity = dht.getHumidity();
  if (isnan(humidity)) {
    Serial.println("No reading from the sensor. Check the three wires.");
    return;
  }
  Serial.print("Humidity: ");
  Serial.println(humidity);

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi dropped. Reconnecting.");
    if (!connectWiFi()) {
      return;
    }
  }
  sendReading(humidity);
}