/*
 * ============================================================
 * ESP32 #1 — Greenhouse Sensor + Actuator Controller
 * Dashboard Monitoring Hidroponik
 * ============================================================
 *
 * Sensor  : DHT22 (GPIO 18), BH1750 I2C (SDA 21, SCL 22)
 * Aktuator: exhaust_fan (GPIO 25), pompa_misting (GPIO 26),
 *           led_grow_light (GPIO 27)
 * Tabel   : sensor_readings (suhu-rumah-kaca, kelembapan,
 *            intensitas-cahaya)
 *            sensor_thresholds (suhu_rumah_kaca, intensitas_cahaya)
 *            actuator_states (actual_on, last_heartbeat, triggered_by)
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
const char *SUPABASE_ANON_KEY =
    "sb_publishable_wAF-1uo9hEZeLMFZyO5NhQ_m097tdty";

const unsigned long SENSOR_INTERVAL = 60000;    // Kirim sensor tiap 60 detik
const unsigned long THRESHOLD_INTERVAL = 30000; // Fetch threshold tiap 30 detik
const unsigned long ACTUATOR_INTERVAL =
    30000; // Lapor status relay tiap 30 detik

// ── Misting Duty Cycle ──
const unsigned long MIST_ON_DURATION = 180000; // 3 menit ON
const unsigned long MIST_OFF_DURATION = 60000; // 1 menit OFF
const unsigned long MIST_CYCLE_TOTAL = 240000; // Total 1 siklus = 4 menit

// ── Fan Auto-OFF ──
const float FAN_OFF_TEMP = 20.0;
   

// ── NTP ──
const char *NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET_S = 7 * 3600;
const int DST_OFFSET_S = 0;

// ── Pin Sensor ──
#define DHTPIN 18
#define DHTTYPE DHT22
#define SDA_PIN 21
#define SCL_PIN 22

// ── Pin Relay (Active-LOW) ──
#define RELAY_FAN 25
#define RELAY_MIST 26
#define RELAY_LED 27
#define RELAY_ON LOW
#define RELAY_OFF HIGH

// ── ID & Label Aktuator di Supabase ──
const char *ID_FAN = "exhaust_fan";
const char *ID_MIST = "pompa_misting";
const char *ID_LED = "led_grow_light";
const char *LABEL_FAN = "Exhaust Fan";
const char *LABEL_MIST = "Pompa Misting";
const char *LABEL_LED = "LED Grow Light";

// ── SPIFFS ──
const char *SPIFFS_FILE = "/data_log.txt";

// ================================================================
//  OBJEK SENSOR & VARIABEL
// ================================================================
DHT dht(DHTPIN, DHTTYPE);
BH1750 lightMeter;

float suhuValue = NAN;
float humValue = NAN;
float luxValue = NAN;

bool fanState = true; // Default ON
bool mistState = false;
bool ledState = false;

// ── Misting Duty Cycle State ──
bool mistingActive = false;
unsigned long mistCycleStart = 0;

// ── Threshold Struct ──
struct SensorThreshold {
  float minVal, maxVal;
  bool loaded;
};
struct {
  SensorThreshold suhu = {35.0f, 35.0f, true};
  SensorThreshold cahaya = {3000.0f, 3000.0f, true};
} thresholds;

unsigned long lastSensorTime = 0;
unsigned long lastThresholdTime = 0;
unsigned long lastActuatorTime = 0;

// Flag untuk deteksi WiFi reconnect
bool wasOffline = false;

// ================================================================
//  FORWARD DECLARATIONS
// ================================================================
bool reportOneActuator(const char *id, bool state,
                       const char *triggeredBy = nullptr);

// ================================================================
//  TIMESTAMP ISO 8601 WIB
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

// ================================================================
//  RELAY
// ================================================================
void setRelay(int pin, bool on) {
  digitalWrite(pin, on ? RELAY_ON : RELAY_OFF);
}
bool getRelayState(int pin) { return digitalRead(pin) == RELAY_ON; }

// ================================================================
//  WIFI
// ================================================================
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED)
    return;
  Serial.print("[WiFi] Menghubungkan ke: ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 40) {
    delay(500);
    Serial.print(".");
    attempt++;
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] ✅ IP: ");
    Serial.println(WiFi.localIP());
    configTime(GMT_OFFSET_S, DST_OFFSET_S, NTP_SERVER);

    Serial.print("[NTP] Menunggu sinkronisasi");
    struct tm timeinfo;
    int retry = 0;
    while (!getLocalTime(&timeinfo) && retry < 20) {
      Serial.print(".");
      delay(500);
      retry++;
    }
    if (retry < 20) {
      char buf[30];
      strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+07:00", &timeinfo);
      Serial.println(" OK!");
      Serial.print("[NTP] Waktu WIB: ");
      Serial.println(buf);
    } else {
      Serial.println(" GAGAL! Timestamp mungkin tidak akurat.");
    }
  } else {
    Serial.println("[WiFi] ❌ Gagal terhubung.");
  }
}

// ================================================================
//  BACA SENSOR
// ================================================================
void readSensors() {
  // DHT22 — suhu + kelembapan
  suhuValue = NAN;
  humValue = NAN;
  for (int i = 0; i < 3; i++) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) {
      suhuValue = t;
      humValue = h;
      break;
    }
    Serial.print("[DHT22] Retry ");
    Serial.print(i + 1);
    Serial.println("/3 (gagal baca)...");
    delay(2000);
  }

  // BH1750 — intensitas cahaya
  luxValue = NAN;
  for (int i = 0; i < 3; i++) {
    float lux = lightMeter.readLightLevel();
    if (lux >= 0) {
      luxValue = lux;
      break;
    }
    Serial.print("[BH1750] Retry ");
    Serial.print(i + 1);
    Serial.println("/3...");
    delay(200);
  }
  if (isnan(luxValue)) {
    Serial.println("[BH1750] Semua retry gagal, re-init I2C...");
    Wire.begin(SDA_PIN, SCL_PIN);
    delay(50);
    if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
      delay(200);
      float lux = lightMeter.readLightLevel();
      if (lux >= 0)
        luxValue = lux;
    }
    if (isnan(luxValue))
      Serial.println("[BH1750] Gagal total → NAN");
  }

  Serial.println("─────────────────────────────────────");
  Serial.print("  Suhu      : ");
  if (isnan(suhuValue))
    Serial.println("ERROR");
  else {
    Serial.print(suhuValue, 1);
    Serial.println(" °C");
  }
  Serial.print("  Kelembapan: ");
  if (isnan(humValue))
    Serial.println("ERROR");
  else {
    Serial.print(humValue, 1);
    Serial.println(" %");
  }
  Serial.print("  Cahaya    : ");
  if (isnan(luxValue))
    Serial.println("ERROR");
  else {
    Serial.print(luxValue, 0);
    Serial.println(" lux");
  }
}

// ================================================================
//  LOGIKA KONTROL RELAY
// ================================================================
void controlFan() {
  if (isnan(suhuValue))
    return;
  bool newFan = (suhuValue >= FAN_OFF_TEMP);
  if (newFan != fanState) {
    setRelay(RELAY_FAN, newFan);
    fanState = newFan;
    Serial.print("[FAN] ");
    Serial.println(fanState ? "ON" : "OFF (suhu < 20°C)");
    reportOneActuator(ID_FAN, fanState, fanState ? nullptr : "suhu-rumah-kaca");
    delay(50);
  }
}

void controlMisting() {
  if (isnan(suhuValue) || !thresholds.suhu.loaded)
    return;
  float setPoint = thresholds.suhu.minVal;

  if (suhuValue > setPoint && !mistingActive) {
    mistingActive = true;
    mistCycleStart = millis();
    setRelay(RELAY_MIST, true);
    mistState = true;
    Serial.print("[MISTING] AKTIF (suhu ");
    Serial.print(suhuValue, 1);
    Serial.print(" > ");
    Serial.print(setPoint, 1);
    Serial.println(")");
    reportOneActuator(ID_MIST, true, "suhu-rumah-kaca");
    delay(50);
  } else if (suhuValue <= setPoint && mistingActive) {
    mistingActive = false;
    if (mistState) {
      setRelay(RELAY_MIST, false);
      mistState = false;
      reportOneActuator(ID_MIST, false, nullptr);
      delay(50);
    }
    Serial.println("[MISTING] MATI (suhu <= set point)");
  }
}

void controlLED() {
  if (!thresholds.cahaya.loaded || isnan(luxValue))
    return;
  float setPoint = thresholds.cahaya.minVal;
  bool newLed = (luxValue < setPoint);
  if (newLed != ledState) {
    setRelay(RELAY_LED, newLed);
    ledState = newLed;
    reportOneActuator(ID_LED, ledState,
                      ledState ? "intensitas-cahaya" : nullptr);
    delay(50);
    Serial.print("[LED] ");
    Serial.println(ledState ? "ON" : "OFF");
  }
}

// ================================================================
//  FETCH THRESHOLD DARI SUPABASE
// ================================================================
void fetchThresholds() {
  if (WiFi.status() != WL_CONNECTED)
    return;

  String url = String(SUPABASE_URL) + "/rest/v1/sensor_thresholds" +
               "?select=sensor_id,min_value,max_value" +
               "&sensor_id=in.(suhu_rumah_kaca,intensitas_cahaya)";

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5);
  HTTPClient http;
  http.begin(client, url);
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(8000);

  int code = http.GET();
  String body = http.getString();
  Serial.print("[THRESHOLD] Fetch HTTP ");
  Serial.println(code);

  if (code == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, body)) {
      for (JsonObject row : doc.as<JsonArray>()) {
        const char *id = row["sensor_id"] | "";
        float mn = row["min_value"].as<float>();
        float mx = row["max_value"].as<float>();
        if (strcmp(id, "suhu_rumah_kaca") == 0)
          thresholds.suhu = {mn, mx, true};
        else if (strcmp(id, "intensitas_cahaya") == 0)
          thresholds.cahaya = {mn, mx, true};
        Serial.print("  [THRESHOLD] ");
        Serial.print(id);
        Serial.print(" → ");
        Serial.print(mn);
        Serial.print(" – ");
        Serial.println(mx);
      }
    }
  } else {
    Serial.print("[THRESHOLD] ❌ HTTP ");
    Serial.println(code);
  }
  http.end();
  client.stop();
}

// ================================================================
//  KIRIM SENSOR KE SUPABASE
// ================================================================
bool sendData(const String &payload) {
  if (WiFi.status() != WL_CONNECTED)
    return false;

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5);
  HTTPClient http;
  http.begin(client, String(SUPABASE_URL) + "/rest/v1/sensor_readings");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Prefer", "return=minimal");
  http.setTimeout(8000);

  int code = http.POST(payload);
  String resp = http.getString();
  http.end();
  client.stop();

  if (code == 200 || code == 201 || code == 204) {
    Serial.print("[Supabase] Sensor terkirim. HTTP ");
    Serial.println(code);
    return true;
  }
  Serial.print("[Supabase] Gagal. HTTP ");
  Serial.println(code);
  Serial.print("  Response: ");
  Serial.println(resp);
  return false;
}

String buildSensorPayload() {
  JsonDocument doc;
  bool hasData = false;

  if (!isnan(suhuValue) && suhuValue >= 0.0f && suhuValue <= 60.0f) {
    doc["suhu-rumah-kaca"] = round(suhuValue * 10) / 10.0;
    hasData = true;
  }
  if (!isnan(humValue) && humValue >= 1.0f && humValue <= 100.0f) {
    doc["kelembapan"] = round(humValue * 10) / 10.0;
    hasData = true;
  }
  if (!isnan(luxValue) && luxValue >= 0.0f) {
    doc["intensitas-cahaya"] = round(luxValue);
    hasData = true;
  }

  if (hasData) {
    doc["created_at"] = getTimestamp();
  }

  String out;
  serializeJson(doc, out);
  return out;
}

// ================================================================
//  SPIFFS
// ================================================================
void saveToSPIFFS(const String &data) {
  File f = SPIFFS.open(SPIFFS_FILE, FILE_APPEND);
  if (f) {
    f.println(data);
    f.close();
    Serial.println("[SPIFFS] Data disimpan.");
  }
}

void flushSPIFFS() {
  if (!SPIFFS.exists(SPIFFS_FILE))
    return;
  File f = SPIFFS.open(SPIFFS_FILE, FILE_READ);
  if (!f)
    return;

  String remaining = "";
  bool failed = false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0)
      continue;
    if (!failed && sendData(line)) {
      Serial.println("[SPIFFS] Data tertunda terkirim.");
    } else {
      failed = true;
      remaining += line + "\n";
    }
  }
  f.close();
  SPIFFS.remove(SPIFFS_FILE);
  if (remaining.length() > 0) {
    File nf = SPIFFS.open(SPIFFS_FILE, FILE_WRITE);
    if (nf) {
      nf.print(remaining);
      nf.close();
    }
  } else {
    Serial.println("[SPIFFS] Semua data tertunda terkirim.");
  }
}

void sendOrSave(const String &payload) {
  if (payload == "{}" || payload.length() == 0) {
    Serial.println("[SENSOR] Payload kosong, skip kirim.");
    return;
  }
  Serial.print("[SENSOR] Payload: ");
  Serial.println(payload);
  if (!sendData(payload)) {
    delay(5000);
    if (!sendData(payload))
      saveToSPIFFS(payload);
  } else {
    flushSPIFFS();
  }
}

// ================================================================
//  LAPOR STATUS AKTUATOR KE SUPABASE
// ================================================================
bool reportOneActuator(const char *id, bool state, const char *triggeredBy) {
  if (WiFi.status() != WL_CONNECTED)
    return false;

  String url = String(SUPABASE_URL) + "/rest/v1/actuator_states?id=eq." + id;

  JsonDocument doc;
  doc["actual_on"] = state;
  doc["last_heartbeat"] = getTimestamp();
  if (triggeredBy != nullptr && strlen(triggeredBy) > 0) {
    doc["triggered_by"] = triggeredBy;
  } else {
    doc["triggered_by"] = nullptr;
  }
  String payload;
  serializeJson(doc, payload);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5);
  HTTPClient http;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Prefer", "return=minimal");
  http.setTimeout(8000);

  int code = http.PATCH(payload);
  Serial.print("  [");
  Serial.print(id);
  Serial.print("] actual_on=");
  Serial.print(state ? "true" : "false");
  Serial.print(" HTTP ");
  Serial.println(code);
  http.end();
  client.stop();
  return (code == 200 || code == 204);
}

// Bulk heartbeat — struktur identik ESP2
void reportActuatorStatus() {
  if (WiFi.status() != WL_CONNECTED)
    return;
  Serial.println("[ACTUATOR] Bulk heartbeat ke Supabase...");

  struct {
    const char *id;
    const char *label;
    int pin;
  } list[] = {
      {ID_FAN, LABEL_FAN, RELAY_FAN},
      {ID_MIST, LABEL_MIST, RELAY_MIST},
      {ID_LED, LABEL_LED, RELAY_LED},
  };

  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();
  String ts = getTimestamp();

  for (auto &a : list) {
    JsonObject obj = array.add<JsonObject>();
    obj["id"] = a.id;
    obj["label"] = a.label;
    obj["actual_on"] = getRelayState(a.pin);
    obj["last_heartbeat"] = ts;
  }

  String payload;
  serializeJson(doc, payload);

  String url = String(SUPABASE_URL) + "/rest/v1/actuator_states";

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5);

  HTTPClient http;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Prefer", "resolution=merge-duplicates,return=minimal");
  http.setTimeout(8000);

  int code = http.POST(payload);
  String resp = http.getString();

  Serial.print("[ACTUATOR] Bulk heartbeat HTTP ");
  Serial.println(code);
  if (code != 200 && code != 201 && code != 204) {
    Serial.print("[Supabase Error]: ");
    Serial.println(resp);
  }

  http.end();
  client.stop();
  esp_task_wdt_reset();
}

// ================================================================
//  SETUP
// ================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("==============================================");
  Serial.println("  ESP32 #1 — Greenhouse Controller");
  Serial.println("  DHT22 + BH1750 + 3 Relay + Supabase");
  Serial.println("==============================================");

  // SPIFFS
  if (!SPIFFS.begin(true))
    Serial.println("[SPIFFS] ❌ Gagal");
  else
    Serial.println("[SPIFFS] ✅ Siap");

  // Sensor
  dht.begin();
  delay(2000);
  Serial.println("[DHT22] Warm-up selesai.");
  Wire.begin(SDA_PIN, SCL_PIN);
  delay(50);
  if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println("[BH1750] Gagal init! Periksa wiring.");
  } else {
    delay(200);
    Serial.println("[BH1750] Siap.");
  }

  // Relay — Fan ON default, Misting & LED OFF
  int allPins[] = {RELAY_FAN, RELAY_MIST, RELAY_LED};
  for (int p : allPins) {
    pinMode(p, OUTPUT);
    setRelay(p, false);
  }
  setRelay(RELAY_FAN, true);
  fanState = true;
  Serial.println("[RELAY] Fan=ON (default), Mist=OFF, LED=OFF.");

  // WiFi & NTP
  connectWiFi();

  // Threshold awal
  if (WiFi.status() == WL_CONNECTED) {
    fetchThresholds();
    // Hapus data SPIFFS lama dari firmware sebelumnya (format kolom berbeda)
    if (SPIFFS.exists(SPIFFS_FILE)) {
      SPIFFS.remove(SPIFFS_FILE);
      Serial.println("[SPIFFS] Data lama dihapus (format kolom berubah).");
    }
  }

  // Hardware Watchdog Timer (5 menit)
  const esp_task_wdt_config_t wdt_config = {
      .timeout_ms = 300000, .idle_core_mask = 0, .trigger_panic = true};
  esp_err_t wdt_err = esp_task_wdt_reconfigure(&wdt_config);
  if (wdt_err == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_init(&wdt_config);
  }
  esp_task_wdt_add(NULL);
  Serial.println("[WDT] Watchdog aktif (timeout 5 menit).");

  // Stagger timer agar task tidak overlap
  unsigned long bootNow = millis();
  lastSensorTime = bootNow;
  lastThresholdTime = bootNow - 10000;
  lastActuatorTime = bootNow - 20000;

  Serial.println("[BOOT] Sistem siap!\n");
}

// ================================================================
//  MAIN LOOP — Struktur identik ESP2
// ================================================================
void loop() {
  // Watchdog reset
  esp_task_wdt_reset();

  // Heap monitoring
  static unsigned long lastHeapLog = 0;
  unsigned long now = millis();
  if (now - lastHeapLog >= 30000) {
    Serial.printf("[HEAP] Free: %u bytes | Min ever: %u bytes\n",
                  ESP.getFreeHeap(), ESP.getMinFreeHeap());
    if (ESP.getFreeHeap() < 20000) {
      Serial.println("[HEAP] ⚠ Memori kritis! Restart preventif...");
      delay(1000);
      ESP.restart();
    }
    lastHeapLog = now;
  }

  // Auto-reconnect WiFi
  if (WiFi.status() != WL_CONNECTED) {
    if (!wasOffline) {
      Serial.println("[WiFi] Koneksi putus. Backup SPIFFS aktif.");
      wasOffline = true;
    }
    Serial.println("[WiFi] Reconnecting...");
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) {
      delay(10000);
      return;
    }
  }

  // Reconnect berhasil → flush SPIFFS & re-fetch threshold
  if (wasOffline && WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] Reconnect berhasil! Flush SPIFFS...");
    flushSPIFFS();
    fetchThresholds();
    lastThresholdTime = millis();
    wasOffline = false;
  }

  // 1. Fetch threshold (tiap 30 detik)
  if (now - lastThresholdTime >= THRESHOLD_INTERVAL) {
    fetchThresholds();
    lastThresholdTime = now;
    // Re-evaluasi kontrol setelah threshold baru
    controlFan();
    controlMisting();
    controlLED();
  }

  // 2. Baca & kontrol sensor (tiap 60 detik)
  if (now - lastSensorTime >= SENSOR_INTERVAL) {
    readSensors();

    // Kontrol otomatis
    controlFan();
    controlMisting();
    controlLED();

    // Kirim sensor ke Supabase
    sendOrSave(buildSensorPayload());
    lastSensorTime = now;
  }

  // 3. Lapor status relay (tiap 30 detik)
  if (now - lastActuatorTime >= ACTUATOR_INTERVAL) {
    reportActuatorStatus();
    lastActuatorTime = now;
  }

  // 4. Misting Duty Cycle (setiap loop ~100ms)
  if (mistingActive) {
    unsigned long currentMs = millis();
    unsigned long elapsed = (currentMs - mistCycleStart) % MIST_CYCLE_TOTAL;
    bool shouldBeOn = (elapsed < MIST_ON_DURATION);
    if (shouldBeOn != mistState) {
      setRelay(RELAY_MIST, shouldBeOn);
      mistState = shouldBeOn;
      reportOneActuator(ID_MIST, mistState,
                        mistState ? "suhu-rumah-kaca" : nullptr);
      Serial.println(shouldBeOn ? "[MISTING] Siklus ON (3 menit)"
                                : "[MISTING] Siklus OFF (1 menit jeda)");
      delay(50);
    }
  }

  delay(100);
}
