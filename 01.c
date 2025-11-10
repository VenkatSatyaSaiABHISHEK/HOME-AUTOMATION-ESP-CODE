#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <SinricPro.h>
#include <SinricProSwitch.h>
#include <PubSubClient.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "esp_crt_bundle.h"   // built-in CA bundle (symbols exported by core)

// --- Built-in bundle symbols (provided by esp32 core) ---
extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[]   asm("_binary_x509_crt_bundle_end");
static inline size_t crt_bundle_size() {
  return (size_t)(x509_crt_bundle_end - x509_crt_bundle_start);
}

// ---------- USER SETTINGS ----------
#define WIFI_SSID "abhi4g"
#define WIFI_PASS "Imamssik"

#define APP_KEY "82d48252-d96f-4b1e-a7ab-042c588ea74e"
#define APP_SECRET "da236b6f-15a7-4a22-bf13-4bb90d11252a-8300eb7c-ef9f-4d33-b4ee-dc1d271bad53"

// Devices
#define SWITCH_ID_1 "68e9d693ba649e246c0af03d"
#define RELAY_PIN_1 23
#define SWITCH_ID_2 "YOUR_SECOND_DEVICE_ID"
#define RELAY_PIN_2 22

// HTTP server
#define HTTP_PORT 80

// MQTT broker (EMQX Cloud)
#define MQTT_BROKER "e2a792bf.ala.eu-central-1.emqxsl.com"
#define MQTT_PORT 8883
#define MQTT_CLIENT_ID "ESP32_MultiSwitch"

// (optional) EMQX username/password
#define MQTT_USERNAME "esp32_1"
#define MQTT_PASSWORD "321654987"

// Supabase Edge Function  ✅ FIXED URL
const char* EDGE_URL = "https://jiiopewohvvhgmiknpln.functions.supabase.co/event";
const char* EDGE_SECRET = "AbhiSecret_92jd8h2jd72hs9d2";

// 🔐 Supabase anon key (anon, NOT service_role)
const char* SUPABASE_ANON_KEY = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImppaW9wZXdvaHZ2aGdtaWtucGxuIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjIzNDI0MjcsImV4cCI6MjA3NzkxODQyN30.FtvZfA9JDHS1ypuZH73_3XdH0XypCk2LkEYh5285Wh4";

// ----------------------------------
WebServer server(HTTP_PORT);

// MQTT Clients
WiFiClient espClient;            
WiFiClientSecure tlsClient;      
PubSubClient mqttClient(espClient);

// Device state struct
struct Device {
  String deviceId;
  int relayPin;
  bool state; // true=ON
};

Device devices[] = {
  {SWITCH_ID_1, RELAY_PIN_1, false},
  {SWITCH_ID_2, RELAY_PIN_2, false}
};
const int NUM_DEVICES = sizeof(devices) / sizeof(Device);

// Forward declarations
void publishMqttStatus(String deviceId, bool state);
void postStateEvent(const String& deviceId, bool state);

// ------------------ TIME SYNC (for TLS) ------------------
void syncTime() {
  configTime(19800, 0, "pool.ntp.org", "time.google.com"); // IST
  Serial.print("⏱ Syncing time");
  time_t now = 0;
  int retries = 0;
  while (now < 1700000000 && retries < 40) {
    Serial.print(".");
    delay(500);
    now = time(nullptr);
    retries++;
  }
  Serial.printf("\n⏱ Time set: %ld\n", now);
}

// ---------- EXTRA: Preflight diagnostics ----------
bool preflightDiagnostics() {
  Serial.println("🔍 Preflight: DNS, TCP reachability, TLS verify...");
  IPAddress ip;
  if (!WiFi.hostByName(MQTT_BROKER, ip)) {
    Serial.println("❌ DNS failed to resolve broker hostname");
    return false;
  }
  Serial.printf("✅ DNS: %s -> %s\n", MQTT_BROKER, ip.toString().c_str());

  WiFiClient testTcp;
  if (!testTcp.connect(ip, MQTT_PORT)) {
    Serial.println("❌ TCP connect to 8883 failed (network/ISP may block 8883)");
    return false;
  }
  Serial.println("✅ TCP 8883 reachable");
  testTcp.stop();

  WiFiClientSecure probe;
  probe.setCACertBundle(x509_crt_bundle_start, crt_bundle_size());
  probe.setHandshakeTimeout(30);
  if (!probe.connect(MQTT_BROKER, MQTT_PORT)) {
    char err[150]; err[0] = 0;
    probe.lastError(err, sizeof(err));
    Serial.printf("❌ TLS handshake failed. %s\n", err[0] ? err : "(no detail)");
    return false;
  }
  Serial.println("✅ TLS handshake OK (chain + hostname verified by mbedTLS)");
  probe.stop();
  return true;
}

// ------------------ CORE CONTROL ------------------
void setRelayState(String deviceId, bool state, const char* source) {
  for (int i = 0; i < NUM_DEVICES; i++) {
    if (devices[i].deviceId == deviceId) {
      devices[i].state = state;
      digitalWrite(devices[i].relayPin, state ? LOW : HIGH); // Active LOW

      Serial.println("\n========================================");
      Serial.printf("📥 COMMAND from %s\n", source);
      Serial.printf("Device: %s\n", deviceId.c_str());
      Serial.printf("New State: %s\n", state ? "ON" : "OFF");
      Serial.printf("✅ Relay turned %s (GPIO %d = %s)\n",
                    state ? "ON" : "OFF", devices[i].relayPin, state ? "LOW" : "HIGH");
      Serial.println("========================================\n");

      publishMqttStatus(deviceId, state);
      postStateEvent(deviceId, state);
      return;
    }
  }
  Serial.printf("❌ Unknown device ID: %s\n", deviceId.c_str());
}

// SinricPro power handler
bool onPowerState(const String &deviceId, bool &state) {
  for (int i = 0; i < NUM_DEVICES; i++) {
    if (devices[i].deviceId == deviceId) {
      setRelayState(deviceId, state, "Sinric Pro (Alexa/Google)");
      return true;
    }
  }
  Serial.printf("❌ Unknown device ID: %s\n", deviceId.c_str());
  return false;
}

// ------------------ HTTP API ----------------------
void handleStatus() {
  StaticJsonDocument<512> doc;
  doc["success"] = true;
  JsonArray arr = doc.createNestedArray("devices");
  for (int i = 0; i < NUM_DEVICES; i++) {
    JsonObject x = arr.createNestedObject();
    x["deviceId"] = devices[i].deviceId;
    x["gpio"] = devices[i].relayPin;
    x["state"] = devices[i].state ? "ON" : "OFF";
  }
  String response; serializeJson(doc, response);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", response);
}

void handleStatusOne() {
  if (!server.hasArg("deviceId")) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"Missing deviceId\"}");
    return;
  }
  String id = server.arg("deviceId");
  for (int i = 0; i < NUM_DEVICES; i++) {
    if (devices[i].deviceId == id) {
      StaticJsonDocument<256> doc;
      doc["success"] = true;
      doc["deviceId"] = id;
      doc["gpio"] = devices[i].relayPin;
      doc["state"] = devices[i].state ? "ON" : "OFF";
      String response; serializeJson(doc, response);
      server.sendHeader("Access-Control-Allow-Origin", "*");
      server.send(200, "application/json", response);
      return;
    }
  }
  server.send(404, "application/json", "{\"success\":false,\"message\":\"Unknown deviceId\"}");
}

void handleControl() {
  if (server.method() == HTTP_OPTIONS) {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
    server.send(204);
    return;
  }
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"No body\"}");
    return;
  }
  StaticJsonDocument<200> doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
    return;
  }
  const char* id = doc["deviceId"];
  const char* stateStr = doc["state"];
  if (!id || !stateStr) {
    server.send(400, "application/json", "{\"success\":\"false\",\"message\":\"Missing deviceId or state\"}");
    return;
  }
  bool newState = (strcasecmp(stateStr, "ON") == 0);
  setRelayState(String(id), newState, "HTTP");

  StaticJsonDocument<200> res;
  res["success"] = true;
  res["deviceId"] = id;
  res["state"] = newState ? "ON" : "OFF";
  String response; serializeJson(res, response);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", response);
}

void handleInfo() {
  StaticJsonDocument<512> doc;
  doc["success"] = true;
  doc["ip"] = WiFi.localIP().toString();
  doc["mac"] = WiFi.macAddress();
  doc["ssid"] = WIFI_SSID;
  JsonArray arr = doc.createNestedArray("devices");
  for (int i = 0; i < NUM_DEVICES; i++) {
    JsonObject x = arr.createNestedObject();
    x["deviceId"] = devices[i].deviceId;
    x["gpio"] = devices[i].relayPin;
    x["state"] = devices[i].state ? "ON" : "OFF";
  }
  String response; serializeJson(doc, response);
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", response);
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>ESP32 Controller</title></head><body>";
  html += "<h1>ESP32 Relay Controller</h1>";
  html += "<p>See <a href='/status'>/status</a>, <a href='/info'>/info</a></p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

// ------------------ MQTT -------------------------
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.printf("\n📨 MQTT Message on [%s]: ", topic);
  String topicStr = String(topic);
  int firstSlash = topicStr.indexOf('/');
  int secondSlash = topicStr.indexOf('/', firstSlash + 1);
  String deviceId = topicStr.substring(firstSlash + 1, secondSlash);

  String message;
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
  Serial.println(message);

  StaticJsonDocument<200> doc;
  if (deserializeJson(doc, message)) {
    Serial.println("❌ Invalid MQTT JSON");
    return;
  }
  const char* stateStr = doc["state"];
  if (stateStr) {
    bool newState = (strcasecmp(stateStr, "ON") == 0);
    setRelayState(deviceId, newState, "MQTT (App)");
  }
}

void publishMqttStatus(String deviceId, bool state) {
  if (!mqttClient.connected()) return;
  String statusTopic = "sinric/" + deviceId + "/status";
  StaticJsonDocument<200> doc;
  doc["deviceId"] = deviceId;
  doc["state"] = state ? "ON" : "OFF";
  doc["timestamp"]= millis();
  String msg; serializeJson(doc, msg);
  mqttClient.publish(statusTopic.c_str(), msg.c_str());
  Serial.printf("📤 MQTT Status -> %s: %s\n", statusTopic.c_str(), state ? "ON" : "OFF");
}

void reconnectMqtt() {
  while (!mqttClient.connected()) {
    Serial.print("🔌 Connecting MQTT... ");

    // Reset TLS state and (re)attach CA bundle every attempt
    tlsClient.stop();
    tlsClient.setCACertBundle(x509_crt_bundle_start, crt_bundle_size());
    tlsClient.setHandshakeTimeout(30);
    tlsClient.setTimeout(15000);

    bool connected = false;
    if (strlen(MQTT_USERNAME) > 0) {
      connected = mqttClient.connect(MQTT_CLIENT_ID, MQTT_USERNAME, MQTT_PASSWORD);
    } else {
      connected = mqttClient.connect(MQTT_CLIENT_ID);
    }

    if (connected) {
      Serial.println("✅ Connected");
      for (int i = 0; i < NUM_DEVICES; i++) {
        String controlTopic = "sinric/" + devices[i].deviceId + "/control";
        mqttClient.subscribe(controlTopic.c_str());
        Serial.printf("📥 Subscribed: %s\n", controlTopic.c_str());
      }
      for (int i = 0; i < NUM_DEVICES; i++) publishMqttStatus(devices[i].deviceId, devices[i].state);
    } else {
      Serial.printf("❌ rc=%d. ", mqttClient.state());
      char err[150] = {0};
      tlsClient.lastError(err, sizeof(err));
      if (err[0]) Serial.printf("TLS error: %s. ", err);
      Serial.println("Retry in 5s...");
      delay(5000);
    }
  }
}

// ------------------ Supabase POST ----------------  ✅ FIXED: secure client + URL
void postStateEvent(const String& deviceId, bool state) {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure https;
  https.setCACertBundle(x509_crt_bundle_start, crt_bundle_size());
  https.setHandshakeTimeout(30);
  https.setTimeout(15000);

  HTTPClient http;
  if (!http.begin(https, EDGE_URL)) {
    Serial.println("❌ http.begin failed");
    return;
  }

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("x-device-secret", EDGE_SECRET);

  StaticJsonDocument<200> doc;
  doc["deviceId"] = deviceId;
  doc["state"] = state ? "ON" : "OFF";
  doc["ts"] = (uint32_t)time(nullptr);  // useful for your 'ts' column

  String body; serializeJson(doc, body);
  int code = http.POST(body);
  String resp = http.getString();
  http.end();

  Serial.printf("🌐 POST /event [%d]: %s\n", code, resp.c_str());
}

// ------------------ SETUP/LOOP -------------------
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== ESP32 + Sinric + Supabase ===");

  // Relay pins
  for (int i = 0; i < NUM_DEVICES; i++) {
    pinMode(devices[i].relayPin, OUTPUT);
    digitalWrite(devices[i].relayPin, HIGH); // start OFF
    Serial.printf("Relay %d @ GPIO %d set HIGH (OFF)\n", i+1, devices[i].relayPin);
  }

  // WiFi
  Serial.printf("WiFi -> %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.printf("\nIP: %s\n", WiFi.localIP().toString().c_str());

  // Time sync for TLS validation
  syncTime();

  // TLS for EMQX 8883
  tlsClient.setCACertBundle(x509_crt_bundle_start, crt_bundle_size());
  tlsClient.setHandshakeTimeout(30);
  mqttClient.setClient(tlsClient);          
  mqttClient.setBufferSize(1024);
  mqttClient.setKeepAlive(30);

  // Preflight
  bool tlsOK = preflightDiagnostics();
  if (!tlsOK) Serial.println("⚠  TLS preflight not OK; MQTT may show rc=-2.");

  // SinricPro devices
  for (int i = 0; i < NUM_DEVICES; i++) {
    SinricProSwitch &sw = SinricPro[devices[i].deviceId.c_str()];
    sw.onPowerState(onPowerState);
  }
  SinricPro.onConnected([](){ Serial.println("✅ SinricPro connected"); });
  SinricPro.onDisconnected([](){ Serial.println("⚠ SinricPro disconnected"); });
  SinricPro.begin(APP_KEY, APP_SECRET);

  // HTTP routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/status/one", HTTP_GET, handleStatusOne);
  server.on("/control", HTTP_POST, handleControl);
  server.on("/control", HTTP_OPTIONS, handleControl);
  server.on("/info", HTTP_GET, handleInfo);
  server.begin();
  Serial.printf("HTTP server on http://%s\n", WiFi.localIP().toString().c_str());

  // MQTT
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  Serial.printf("MQTT broker: %s:%d (TLS)\n", MQTT_BROKER, MQTT_PORT);
}

void loop() {
  SinricPro.handle();
  server.handleClient();
  if (!mqttClient.connected()) reconnectMqtt();
  mqttClient.loop();
}
