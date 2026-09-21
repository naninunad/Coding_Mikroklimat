/*
 * ============================================================
 * ESP32 #1 — Greenhouse Controller (REV25)
 * ============================================================
 */

#include "DHT.h"
#include "SPIFFS.h"
#include "time.h"
#include <ArduinoJson.h>
#include <BH1750.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <esp_task_wdt.h>

// ================================================================
//  KONFIGURASI
// ================================================================
const char *WIFI_SSID = "Greenhouse_Polman";
const char *WIFI_PASSWORD = "tad42026";

const char *SUPABASE_URL = "https://xkziqidvpmexhhxjxwzw.supabase.co";
const char *SUPABASE_ANON_KEY = "sb_publishable_wAF-1uo9hEZeLMFZyO5NhQ_m097tdty";

// ── INTERVAL GANJIL GANESHA (ANTI-TABRAKAN DENGAN ESP2 MIRZA) ──
const unsigned long SENSOR_INTERVAL = 67000;    // 67 detik
const unsigned long THRESHOLD_INTERVAL = 37000; // 37 detik
const unsigned long ACTUATOR_INTERVAL = 33000;  // 33 detik

// ── Misting Duty Cycle ──
const unsigned long MIST_ON_DURATION = 180000; // 3 menit ON
const unsigned long MIST_OFF_DURATION = 60000; // 1 menit OFF
const unsigned long MIST_CYCLE_TOTAL = 240000; // 4 menit total

// ── NTP ──
const char *NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_S = 7 * 3600;
const int DST_OFFSET_S = 0;

// ── Pin Hardware ──
#define DHTPIN 18
#define DHTTYPE DHT22
#define SDA_PIN 21
#define SCL_PIN 22
#define RELAY_FAN 25
#define RELAY_MIST 26
#define RELAY_LED 27

#define RELAY_ON LOW
#define RELAY_OFF HIGH

const char *ID_FAN = "exhaust_fan";
const char *ID_MIST = "pompa_misting";
const char *ID_LED = "led_grow_light";
const char *LABEL_FAN = "Exhaust Fan";
const char *LABEL_MIST = "Pompa Misting";
const char *LABEL_LED = "LED Grow Light";

const char *SPIFFS_FILE = "/data_log.txt";

// ================================================================
//  OBJEK & VARIABEL
// ================================================================
DHT dht(DHTPIN, DHTTYPE);
BH1750 lightMeter;

float suhuValue = NAN;
float humValue = NAN;
float luxValue = NAN;

bool fanState = true; 
bool mistState = false;
bool ledState = false;

// ── Misting State ──
bool mistingActive = false;
unsigned long mistCycleStart = 0;

// ── Threshold dari Supabase ──
struct SensorThreshold { float minVal, maxVal; bool loaded; };
struct {
  SensorThreshold suhu = {35.0f, 35.0f, true};
  SensorThreshold cahaya = {3000.0f, 3000.0f, true};
} thresholds;

unsigned long lastSensorTime = 0;
unsigned long lastThresholdTime = 0;
unsigned long lastActuatorTime = 0;
bool wasOffline = false;

// ================================================================
//  FUNGSI BANTUAN
// ================================================================
String getTimestamp() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    char buf[30];
    snprintf(buf, sizeof(buf), "1970-01-01T00:00:%02luZ", millis() / 1000 % 60);
    return String(buf);
  }
  char buf[30];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+07:00", &timeinfo);
  return String(buf);
}

void setRelay(int pin, bool on) {
  digitalWrite(pin, on ? RELAY_ON : RELAY_OFF);
}

bool getRelayState(int pin) { 
  return digitalRead(pin) == RELAY_ON; 
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  
  WiFi.disconnect(true);
  delay(100);
  
  Serial.print("[WiFi] Menghubungkan ke: ");
  Serial.println(WIFI_SSID);
  
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false); // Cegah mode hemat daya (Anti-DHCP Timeout)
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 40) {
    delay(500);
    Serial.print(".");
    attempt++;
  }
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] ✅ IP: "); Serial.println(WiFi.localIP());
    configTime(GMT_OFFSET_S, DST_OFFSET_S, NTP_SERVER);
  } else {
    Serial.println("[WiFi] ❌ Gagal terhubung.");
  }
}

// ================================================================
//  BACA SENSOR
// ================================================================
void readSensors() {
  suhuValue = NAN; humValue = NAN;
  for (int i = 0; i < 3; i++) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) {
      suhuValue = t + TEMP_OFFSET; 
      humValue = h; 
      break;
    }
    delay(2000); // Penundaan 2 detik wajib untuk DHT22
  }

  luxValue = NAN;
  for (int i = 0; i < 3; i++) {
    float lux = lightMeter.readLightLevel();
    if (lux >= 0) { 
      luxValue = lux; 
      break; 
    }
    delay(100);
  }
  
  if (isnan(luxValue)) {
    Wire.begin(SDA_PIN, SCL_PIN); delay(50);
    if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
      delay(200); float lux = lightMeter.readLightLevel();
      if (lux >= 0) luxValue = lux;
    }
  }
  
  Serial.println("─────────────────────────────────────");
  Serial.printf("  Suhu      : %.1f °C\n", suhuValue);
  Serial.printf("  Kelembapan: %.1f %%\n", humValue);
  Serial.printf("  Cahaya    : %.0f lux\n", luxValue);
}

// ================================================================
//  LOGIKA KONTROL RELAY (HISTERESIS DINAMIS)
// ================================================================
void controlFan() {
  if (isnan(suhuValue)) return;
  bool newFan = (suhuValue >= FAN_OFF_TEMP);
  if (newFan != fanState) {
    setRelay(RELAY_FAN, newFan); 
    fanState = newFan;
    reportOneActuator(ID_FAN, fanState, fanState ? nullptr : "suhu_rumah_kaca");
  }
}

void controlMisting() {
  if (isnan(suhuValue) || !thresholds.suhu.loaded) return;
  
  float setPointOn = thresholds.suhu.maxVal;  
  float setPointOff = thresholds.suhu.minVal; 

  if (suhuValue >= setPointOn && !mistingActive) {
    mistingActive = true;
    mistCycleStart = millis(); 
    setRelay(RELAY_MIST, true);
    mistState = true;
    reportOneActuator(ID_MIST, true, "suhu_rumah_kaca");
    
  } else if (suhuValue <= setPointOff && mistingActive) {
    mistingActive = false;
    if (mistState) {
      setRelay(RELAY_MIST, false);
      mistState = false;
      reportOneActuator(ID_MIST, false, nullptr);
    }
  }
}

void controlLED() {
  if (!thresholds.cahaya.loaded || isnan(luxValue)) return;
  
  float setPointOn = thresholds.cahaya.minVal;  
  float setPointOff = thresholds.cahaya.maxVal; 

  if (luxValue <= setPointOn && !ledState) {
    setRelay(RELAY_LED, true); 
    ledState = true;
    reportOneActuator(ID_LED, true, "intensitas_cahaya");
    
  } else if (luxValue >= setPointOff && ledState) {
    setRelay(RELAY_LED, false); 
    ledState = false;
    reportOneActuator(ID_LED, false, nullptr);
  }
}

// ================================================================
//  KOMUNIKASI SUPABASE 
// ================================================================
void fetchThresholds() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  String url = String(SUPABASE_URL) + "/rest/v1/sensor_thresholds?select=sensor_id,min_value,max_value&sensor_id=in.(suhu_rumah_kaca,intensitas_cahaya)";
  WiFiClientSecure client; client.setInsecure(); client.setTimeout(5);
  
  HTTPClient http; http.begin(client, url);
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  
  int code = http.GET();
  if (code == 200) {
    String body = http.getString(); 
    JsonDocument doc;
    if (!deserializeJson(doc, body)) {
      for (JsonObject row : doc.as<JsonArray>()) {
        const char *id = row["sensor_id"] | "";
        if (strcmp(id, "suhu_rumah_kaca") == 0) {
          thresholds.suhu = {row["min_value"], row["max_value"], true};
        } else if (strcmp(id, "intensitas_cahaya") == 0) {
          thresholds.cahaya = {row["min_value"], row["max_value"], true};
        }
      }
    }
  } else {
    http.getString(); 
  }
  http.end(); client.stop();
}

bool sendData(const String &payload) {
  if (WiFi.status() != WL_CONNECTED) return false;
  
  WiFiClientSecure client; client.setInsecure(); client.setTimeout(5);
  HTTPClient http; http.begin(client, String(SUPABASE_URL) + "/rest/v1/sensor_readings");
  
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Prefer", "return=minimal");
  
  int code = http.POST(payload);
  
  // Menguras buffer data respon untuk mencegah memory fragmentasi RAM
  http.getString();
  http.end(); client.stop();
  
  if (code == 200 || code == 201 || code == 204) {
    Serial.printf("[Supabase] Data Sensor Terkirim! HTTP %d\n", code);
    return true;
  }
  Serial.printf("[Supabase] GAGAL MENGIRIM SENSOR! HTTP %d\n", code);
  return false;
}

String buildSensorPayload() {
  JsonDocument doc; 
  bool hasData = false;
  
  // PERBAIKAN KRUSIAL MASTER: Mengganti semua format strip (-) menjadi underscore (_)
  if (!isnan(suhuValue) && suhuValue >= 0.0f && suhuValue <= 60.0f) { 
    doc["suhu_rumah_kaca"] = round(suhuValue * 10) / 10.0; hasData = true; 
  }
  if (!isnan(humValue) && humValue >= 1.0f && humValue <= 100.0f) { 
    doc["kelembapan"] = round(humValue * 10) / 10.0; hasData = true; 
  }
  if (!isnan(luxValue) && luxValue >= 0.0f) { 
    doc["intensitas_cahaya"] = round(luxValue); hasData = true; 
  }
  
  if (hasData) doc["created_at"] = getTimestamp();
  
  String out; serializeJson(doc, out); 
  return out;
}

// ================================================================
//  SPIFFS (OFFLINE QUEUE)
// ================================================================
void flushSPIFFS() {
  if (!SPIFFS.exists(SPIFFS_FILE)) return;
  File f = SPIFFS.open(SPIFFS_FILE, FILE_READ);
  if (!f) return;

  String remaining = "";
  bool failed = false;
  while (f.available()) {
    String line = f.readStringUntil('\n'); line.trim();
    if (line.length() == 0) continue;
    if (!failed && sendData(line)) {
      Serial.println("[SPIFFS] Data tertunda terkirim.");
    } else {
      failed = true; remaining += line + "\n";
    }
  }
  f.close(); SPIFFS.remove(SPIFFS_FILE);
  if (remaining.length() > 0) {
    File nf = SPIFFS.open(SPIFFS_FILE, FILE_WRITE);
    if (nf) { nf.print(remaining); nf.close(); }
  }
}

void sendOrSave(const String &payload) {
  if (payload.length() == 0 || payload == "{}") return;
  if (!sendData(payload)) {
    File f = SPIFFS.open(SPIFFS_FILE, FILE_APPEND);
    if (f) { f.println(payload); f.close(); }
  } else {
    flushSPIFFS();
  }
}

// ================================================================
//  LAPOR STATUS AKTUATOR
// ================================================================
bool reportOneActuator(const char *id, bool state, const char *triggeredBy) {
  if (WiFi.status() != WL_CONNECTED) return false;
  
  String url = String(SUPABASE_URL) + "/rest/v1/actuator_states?id=eq." + id;
  JsonDocument doc; 
  doc["actual_on"] = state; 
  doc["last_heartbeat"] = getTimestamp();
  if (triggeredBy) doc["triggered_by"] = triggeredBy; else doc["triggered_by"] = nullptr;
  
  String payload; serializeJson(doc, payload);
  
  WiFiClientSecure client; client.setInsecure(); client.setTimeout(5);
  HTTPClient http; http.begin(client, url);
  
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Prefer", "return=minimal");
  
  int code = http.PATCH(payload);
  http.getString(); 
  http.end(); client.stop();
  return (code == 200 || code == 204);
}

void reportActuatorStatus() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  struct { const char *id; const char *label; int pin; } list[] = {
      {ID_FAN, LABEL_FAN, RELAY_FAN}, 
      {ID_MIST, LABEL_MIST, RELAY_MIST}, 
      {ID_LED, LABEL_LED, RELAY_LED},
  };
  
  JsonDocument doc; JsonArray array = doc.to<JsonArray>(); String ts = getTimestamp();
  for (auto &a : list) {
    JsonObject obj = array.add<JsonObject>();
    obj["id"] = a.id; 
    obj["label"] = a.label; 
    obj["actual_on"] = getRelayState(a.pin); 
    obj["last_heartbeat"] = ts;
  }
  
  String payload; serializeJson(doc, payload);
  
  WiFiClientSecure client; client.setInsecure(); client.setTimeout(5);
  HTTPClient http; http.begin(client, String(SUPABASE_URL) + "/rest/v1/actuator_states");
  
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Prefer", "resolution=merge-duplicates,return=minimal");
  
  int code = http.POST(payload);
  http.getString(); 
  http.end(); client.stop();
}

// ================================================================
//  SETUP
// ================================================================
void setup() {
  Serial.begin(115200); delay(500);
  
  Serial.println("\n==============================================");
  Serial.println("  ESP32 #1 — Greenhouse Controller");
  Serial.println("  REV25 FINAL (Non-Stop 24/7 Staggered)");
  Serial.println("==============================================");
  
  SPIFFS.begin(true);
  dht.begin();
  Wire.begin(SDA_PIN, SCL_PIN);
  lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);

  pinMode(RELAY_FAN, OUTPUT); 
  pinMode(RELAY_MIST, OUTPUT); 
  pinMode(RELAY_LED, OUTPUT);
  
  setRelay(RELAY_FAN, true); 
  setRelay(RELAY_MIST, false); 
  setRelay(RELAY_LED, false);

  connectWiFi();

  // Watchdog Timer (5 Menit)
  const esp_task_wdt_config_t wdt_config = { .timeout_ms = 300000, .idle_core_mask = 0, .trigger_panic = true};
  esp_task_wdt_reconfigure(&wdt_config); esp_task_wdt_add(NULL);

  unsigned long bootNow = millis();
  lastSensorTime = bootNow; 
  lastThresholdTime = bootNow - 10000; 
  lastActuatorTime = bootNow - 20000;
  
  Serial.println("[BOOT] Sistem Sempurna Siap Beroperasi!\n");
}

// ================================================================
//  MAIN LOOP
// ================================================================
void loop() {
  esp_task_wdt_reset(); 
  unsigned long now = millis();

  // Proteksi Memori: Hanya restart jika sisa RAM di bawah 15KB (Sangat jarang terjadi)
  static unsigned long lastHeapLog = 0;
  if (now - lastHeapLog >= 30000) {
    if (ESP.getFreeHeap() < 15000) {
      Serial.println("[HEAP] Memori kritis! Memaksa restart...");
      delay(1000);
      ESP.restart();
    }
    lastHeapLog = now;
  }

  // Handle WiFi Reconnect
  if (WiFi.status() != WL_CONNECTED) {
    if (!wasOffline) wasOffline = true;
    connectWiFi();
  }
  if (wasOffline && WiFi.status() == WL_CONNECTED) {
    fetchThresholds(); 
    flushSPIFFS();
    wasOffline = false;
  }

  // 1. Fetch Threshold Data (Setiap 37 Detik)
  if (now - lastThresholdTime >= THRESHOLD_INTERVAL) {
    fetchThresholds(); 
    lastThresholdTime = now;
  }

  // 2. Baca Sensor & Evaluasi Aktuator (Setiap 67 Detik)
  if (now - lastSensorTime >= SENSOR_INTERVAL) {
    readSensors();
    controlFan(); 
    controlMisting(); 
    controlLED();
    sendOrSave(buildSensorPayload());
    lastSensorTime = now;
  }

  // 3. Lapor Heartbeat Aktuator (Setiap 33 Detik)
  if (now - lastActuatorTime >= ACTUATOR_INTERVAL) {
    reportActuatorStatus(); 
    lastActuatorTime = now;
  }

  // 4. LOGIKA MISTING SIKLUS AKTIF (Berjalan cepat setiap 100ms)
  if (mistingActive) {
    unsigned long currentMs = millis();
    unsigned long elapsed = (currentMs - mistCycleStart) % MIST_CYCLE_TOTAL;
    bool shouldBeOn = (elapsed < MIST_ON_DURATION);
    
    if (shouldBeOn != mistState) {
      setRelay(RELAY_MIST, shouldBeOn);
      mistState = shouldBeOn;
      reportOneActuator(ID_MIST, mistState, mistState ? "suhu_rumah_kaca" : nullptr);
    }
  }

  delay(100);
}
