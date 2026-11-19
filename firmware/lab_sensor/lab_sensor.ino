#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include "secrets.h"

// ---------- Pins ----------
#define DHT_PIN      4
#define DHT_TYPE     DHT11      // change to DHT22 if needed
#define OLED_SDA_PIN 21
#define OLED_SCL_PIN 19

// ---------- OLED ----------
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT 64        // change to 32 if your screen is 128x32
#define OLED_ADDRESS  0x3C

// ---------- Configuration ----------
const char*         DEVICE_ID        = "esp32-lab";
const unsigned long SEND_INTERVAL_MS = 10000;   // send every 10 s
const unsigned long WIFI_TIMEOUT_MS  = 15000;

// ---------- Global state ----------
DHT              dht(DHT_PIN, DHT_TYPE);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
bool             displayReady  = false;
unsigned long    lastSendMs    = 0;
float            lastTemp      = NAN;
float            lastHumidity  = NAN;
String           lastStatus    = "Starting...";

// Draw current values and status on the OLED
void updateDisplay() {
  if (!displayReady) {
    return;
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("Lab Climate ");
  display.print(WiFi.status() == WL_CONNECTED ? "WiFi OK" : "No WiFi");

  display.setTextSize(2);
  display.setCursor(0, 16);
  if (isnan(lastTemp)) {
    display.print("-- C");
  } else {
    display.printf("%.1f C", lastTemp);
  }

  display.setCursor(0, 34);
  if (isnan(lastHumidity)) {
    display.print("-- %");
  } else {
    display.printf("%.0f %%", lastHumidity);
  }

  display.setTextSize(1);
  display.setCursor(0, 56);
  display.print(lastStatus);

  display.display();
}

// Connect (or reconnect) to the Wi-Fi network
bool connectWiFi() {
  unsigned long startMs;

  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  lastStatus = "Connecting WiFi...";
  updateDisplay();

  Serial.printf("Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < WIFI_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected. ESP32 IP: ");
    Serial.println(WiFi.localIP());
    lastStatus = "IP " + WiFi.localIP().toString();
    updateDisplay();
    return true;
  }

  Serial.println("Wi-Fi connection failed");
  lastStatus = "WiFi failed";
  updateDisplay();
  return false;
}

// Send one reading to the API as JSON over HTTP POST
void sendReading(float temperature, float humidity) {
  HTTPClient http;
  String     url;
  String     body;
  int        statusCode;

  url  = String("http://") + API_HOST + ":" + String(API_PORT) + "/api/readings";
  body = String("{\"device_id\":\"") + DEVICE_ID +
         "\",\"temperature\":" + String(temperature, 1) +
         ",\"humidity\":" + String(humidity, 1) + "}";

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);
  http.setTimeout(5000);

  statusCode = http.POST(body);

  if (statusCode == 201) {
    Serial.printf("Saved: %.1f C, %.1f %%\n", temperature, humidity);
    lastStatus = "Sent to Docker OK";
  } else {
    Serial.printf("Error %d: %s\n", statusCode, http.getString().c_str());
    lastStatus = "API error " + String(statusCode);
  }

  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Lab Climate Monitor - ESP32");

  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  displayReady = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);
  if (!displayReady) {
    Serial.println("OLED not found, continuing without display");
  }

  dht.begin();
  updateDisplay();
  connectWiFi();
}

void loop() {
  float temperature;
  float humidity;

  if (millis() - lastSendMs < SEND_INTERVAL_MS) {
    return;
  }
  lastSendMs = millis();

  temperature = dht.readTemperature();
  humidity    = dht.readHumidity();

  if (isnan(temperature) || isnan(humidity)) {
    Serial.println("Sensor read failed, check wiring");
    lastStatus = "Sensor error";
    updateDisplay();
    return;
  }

  lastTemp     = temperature;
  lastHumidity = humidity;

  if (connectWiFi()) {
    sendReading(temperature, humidity);
  }

  updateDisplay();
}