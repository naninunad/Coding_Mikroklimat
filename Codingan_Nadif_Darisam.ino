
 *
 *  Sensor yang ditangani:
 *    //- DHT22       → suhu-rumah-kaca, kelembapan
 *    //- BH1750 I2C  → intensitas-cahaya
 *
 *  Aktuator yang dikontrol:
 *    - exhaust_fan    (Relay GPIO 25)
 *    - pompa_misting  (Relay GPIO 26)
 *    - led_grow_light (Relay GPIO 27)
 *
 *  Fitur:
 *   //- INSERT sensor ke Supabase sensor_readings tiap 30 detik
 *    //- FETCH threshold dari sensor_thresholds tiap 15 detik
 *    //- Logika relay two-stage cooling (suhu) + cahaya
 *    //- PATCH actual_on + last_heartbeat ke actuator_states
 *    //- SPIFFS backup saat koneksi gagal
 *    //- NTP timestamp ISO 8601 WIB
 *
 *  Library (Arduino Library Manager):
 *    - ArduinoJson   by Benoit Blanchon (v7.x)
 *    - DHT sensor library + Adafruit Unified Sensor
 *    - BH1750        by Christopher Laws
 *    - WiFi, WiFiClientSecure, HTTPClient (built-in ESP32)
 * ============================================================
 */


#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <BH1750.h>
#include "DHT.h"
#include "SPIFFS.h"
#include "time.h"


// ================================================================
//  KONFIGURASI
// ================================================================


const char* WIFI_SSID     = "Tether";
const char* WIFI_PASSWORD = "12346578";


const char* SUPABASE_URL      = "https://xkziqidvpmexhhxjxwzw.supabase.co";
const char* SUPABASE_ANON_KEY = "sb_publishable_wAF-1uo9hEZeLMFZyO5NhQ_m097tdty";
const char* SENSOR_TABLE      = "sensor_readings";
const char* ACTUATOR_TABLE    = "actuator_states";
const char* THRESHOLD_TABLE   = "sensor_thresholds";


// ── Interval ──
const unsigned long SENSOR_INTERVAL    = 30000;  // Kirim sensor tiap 30 detik
const unsigned long THRESHOLD_INTERVAL = 15000;  // Fetch threshold tiap 15 detik
const unsigned long REPORT_INTERVAL    = 15000;  // Lapor aktuator tiap 15 detik


// ── NTP ──
const char* NTP_SERVER    = "pool.ntp.org";
const long  GMT_OFFSET_S  = 25200;   // WIB UTC+7
const int   DST_OFFSET_S  = 0;


// ── Pin Sensor ──
#define DHTPIN   19
#define DHTTYPE  DHT22
#define SDA_PIN  21
#define SCL_PIN  22


// ── Pin Relay (Active-LOW) ──
#define RELAY_FAN   25   // exhaust_fan
#define RELAY_MIST  26   // pompa_misting
#define RELAY_LED   27   // led_grow_light
const bool RELAY_ACTIVE_LOW = true;


// ── ID Aktuator di Supabase ──
const char* ID_FAN  = "exhaust_fan";
const char* ID_MIST = "pompa_misting";
const char* ID_LED  = "led_grow_light";


// ── Kalibrasi Suhu ──
const float TEMP_OFFSET = -4.0;  // DHT22 terbaca 4°C lebih tinggi


// ── SPIFFS ──
const char* SPIFFS_FILE = "/data_log.txt";


// ================================================================
//  OBJEK SENSOR & VARIABEL
// ================================================================
DHT dht(DHTPIN, DHTTYPE);
BH1750 lightMeter;


bool fanState  = false;
bool mistState = false;
bool ledState  = false;


unsigned long lastSensorTime    = 0;
unsigned long lastThresholdTime = 0;
unsigned long lastReportTime    = 0;


// ── Threshold (diisi dari Supabase) ──
struct SensorThreshold {
  float minVal;
  float maxVal;
  bool  loaded;
};


struct {
  SensorThreshold suhu;
  SensorThreshold cahaya;
} thresholds = {
  {15.0f, 25.0f, false},
  {2152.0f, 4305.0f, false}
};


// ================================================================
//  TIMESTAMP
// ================================================================
String getISOTimestamp() {
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
//  WIFI
// ================================================================
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;


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
    Serial.print("[WiFi] Terhubung! IP: ");
    Serial.println(WiFi.localIP());
    configTime(GMT_OFFSET_S, DST_OFFSET_S, NTP_SERVER);
    Serial.println("[NTP] Sinkronisasi waktu dikonfigurasi.");
  } else {
    Serial.println("[WiFi] Gagal terhubung!");
  }
}


// ================================================================
//  RELAY
// ================================================================
void setRelay(int pin, bool on) {
  digitalWrite(pin, RELAY_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW));
}


bool getRelayState(int pin) {
  return RELAY_ACTIVE_LOW ? (digitalRead(pin) == LOW) : (digitalRead(pin) == HIGH);
}


// ================================================================
//  BACA SENSOR
// ================================================================
float readSuhu() {
  float v = dht.readTemperature();
  return isnan(v) ? NAN : v + TEMP_OFFSET;
}


float readKelembapan() {
  return dht.readHumidity();
}


float readLux() {
  float lux = lightMeter.readLightLevel();
  return (lux < 0) ? 0 : lux;
}


// ================================================================
//  KIRIM SENSOR KE SUPABASE
// ================================================================
bool sendSensorToSupabase(const String& payload) {
  if (WiFi.status() != WL_CONNECTED) return false;


  String url = String(SUPABASE_URL) + "/rest/v1/" + SENSOR_TABLE;
  WiFiClientSecure client;
  client.setInsecure();


  HTTPClient http;
  http.begin(client, url);
  http.addHeader("Content-Type",  "application/json");
  http.addHeader("apikey",        SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Prefer",        "return=minimal");
  http.setTimeout(8000);


  int code = http.POST(payload);
  String resp = http.getString();
  http.end();


  if (code == 200 || code == 201 || code == 204) {
    Serial.print("[Supabase] Sensor terkirim. HTTP "); Serial.println(code);
    return true;
  }


  Serial.print("[Supabase] Gagal kirim sensor. HTTP "); Serial.println(code);
  Serial.print("[Supabase] Response: "); Serial.println(resp);
  return false;
}


// ================================================================
//  SPIFFS BACKUP & FLUSH
// ================================================================
void saveToSPIFFS(const String& data) {
  File f = SPIFFS.open(SPIFFS_FILE, FILE_APPEND);
  if (f) { f.println(data); f.close(); Serial.println("[SPIFFS] Data disimpan."); }
  else    { Serial.println("[SPIFFS] Gagal membuka file!"); }
}


void flushSPIFFS() {
  if (!SPIFFS.exists(SPIFFS_FILE)) return;
  File f = SPIFFS.open(SPIFFS_FILE, FILE_READ);
  if (!f) return;


  String remaining = "";
  bool failed = false;


  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;


    if (!failed && sendSensorToSupabase(line)) {
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
    if (nf) { nf.print(remaining); nf.close(); }
  } else {
    Serial.println("[SPIFFS] Semua data tertunda terkirim.");
  }
}


// ================================================================
//  FETCH THRESHOLD DARI SUPABASE
//  Membaca sensor_id: suhu_rumah_kaca dan intensitas_cahaya
// ================================================================
void fetchThresholds() {
  if (WiFi.status() != WL_CONNECTED) return;


  String url = String(SUPABASE_URL) + "/rest/v1/" + THRESHOLD_TABLE
             + "?select=sensor_id,min_value,max_value"
             + "&sensor_id=in.(suhu_rumah_kaca,intensitas_cahaya)";


  WiFiClientSecure client;
  client.setInsecure();


  HTTPClient http;
  http.begin(client, url);
  http.addHeader("apikey",        SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Content-Type",  "application/json");
  http.setTimeout(8000);


  int code = http.GET();
  Serial.print("[THRESHOLD] Fetch HTTP "); Serial.println(code);


  if (code == 200) {
    String body = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);


    if (!err) {
      for (JsonObject row : doc.as<JsonArray>()) {
        const char* id  = row["sensor_id"] | "";
        float minVal    = row["min_value"].as<float>();
        float maxVal    = row["max_value"].as<float>();


        if (strcmp(id, "suhu_rumah_kaca") == 0) {
          thresholds.suhu = {minVal, maxVal, true};
          Serial.print("  [suhu_rumah_kaca] min="); Serial.print(minVal);
          Serial.print(" max="); Serial.println(maxVal);
        } else if (strcmp(id, "intensitas_cahaya") == 0) {
          thresholds.cahaya = {minVal, maxVal, true};
          Serial.print("  [intensitas_cahaya] min="); Serial.print(minVal);
          Serial.print(" max="); Serial.println(maxVal);
        }
      }
    } else {
      Serial.print("[THRESHOLD] JSON error: "); Serial.println(err.c_str());
    }
  } else {
    Serial.println("[THRESHOLD] Gagal fetch, pakai nilai terakhir.");
  }
  http.end();
}


// ================================================================
//  LOGIKA RELAY BERDASARKAN THRESHOLD
//  Suhu → Two-Stage Cooling (Exhaust Fan + Misting)
//  Cahaya → LED Grow Light
// ================================================================
void applyRelayLogic(float suhu, float lux) {
  Serial.println("[RELAY] Evaluasi threshold...");


  // ── Suhu: Two-Stage Cooling ──
  if (thresholds.suhu.loaded) {
    float minT = thresholds.suhu.minVal;
    float maxT = thresholds.suhu.maxVal;
    float midT = (minT + maxT) / 2.0f;


    bool newFan  = false;
    bool newMist = false;


    if (suhu > maxT) {
      newFan = true; newMist = true;
      Serial.println("  [Suhu] EMERGENCY → Fan ON, Mist ON");
    } else if (suhu > midT) {
      newFan = false; newMist = true;
      Serial.println("  [Suhu] Stage 2    → Fan OFF, Mist ON");
    } else if (suhu > minT) {
      newFan = true; newMist = false;
      Serial.println("  [Suhu] Stage 1    → Fan ON, Mist OFF");
    } else {
      newFan = false; newMist = false;
      Serial.println("  [Suhu] Normal     → Fan OFF, Mist OFF");
    }


    if (newFan != fanState)   { setRelay(RELAY_FAN,  newFan);  fanState  = newFan; }
    if (newMist != mistState) { setRelay(RELAY_MIST, newMist); mistState = newMist; }


    Serial.print("  Suhu="); Serial.print(suhu, 1);
    Serial.print(" | range=["); Serial.print(minT, 1);
    Serial.print(", "); Serial.print(midT, 1);
    Serial.print(", "); Serial.print(maxT, 1); Serial.println("]");
  }


  // ── Cahaya: LED Grow Light ──
  if (thresholds.cahaya.loaded && !isnan(lux)) {
    float minL = thresholds.cahaya.minVal;
    float maxL = thresholds.cahaya.maxVal;
    bool newLed = ledState;


    // Hysteresis: ON jika di bawah min, OFF jika di atas max
    if (lux < minL)       newLed = true;
    else if (lux >= maxL) newLed = false;


    if (newLed != ledState) { setRelay(RELAY_LED, newLed); ledState = newLed; }


    Serial.print("  Cahaya="); Serial.print(lux, 0);
    Serial.print(" lux | LED="); Serial.print(ledState ? "ON" : "OFF");
    Serial.print(" | range=["); Serial.print(minL, 0);
    Serial.print(", "); Serial.print(maxL, 0); Serial.println("]");
  }
}


// ================================================================
//  LAPOR STATUS AKTUATOR KE SUPABASE
// ================================================================
bool reportActuator(const char* id, bool state) {
  if (WiFi.status() != WL_CONNECTED) return false;


  String url = String(SUPABASE_URL) + "/rest/v1/" + ACTUATOR_TABLE
             + "?id=eq." + id;


  JsonDocument doc;
  doc["actual_on"]      = state;
  doc["last_heartbeat"] = getISOTimestamp();
  String payload;
  serializeJson(doc, payload);


  WiFiClientSecure client;
  client.setInsecure();


  HTTPClient http;
  http.begin(client, url);
  http.addHeader("Content-Type",  "application/json");
  http.addHeader("apikey",        SUPABASE_ANON_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_ANON_KEY);
  http.addHeader("Prefer",        "return=minimal");
  http.setTimeout(8000);


  int code = http.PATCH(payload);
  http.end();


  Serial.print("[ACTUATOR] "); Serial.print(id);
  Serial.print(" actual_on="); Serial.print(state ? "true" : "false");
  Serial.print(" HTTP "); Serial.println(code);


  return (code == 200 || code == 204);
}


void reportAllActuators() {
  Serial.println("[ACTUATOR] Lapor status ke Supabase...");
  reportActuator(ID_FAN,  fanState);  delay(50);
  reportActuator(ID_MIST, mistState); delay(50);
  reportActuator(ID_LED,  ledState);  delay(50);
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
  Serial.println();


  // ── SPIFFS ──
  if (!SPIFFS.begin(true)) {
    Serial.println("[SPIFFS] Gagal dimount!");
  } else {
    Serial.println("[SPIFFS] Siap.");
  }


  // ── Sensor ──
  dht.begin();
  Wire.begin(SDA_PIN, SCL_PIN);
  if (!lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    Serial.println("[BH1750] Gagal init! Periksa wiring I2C.");
  } else {
    Serial.println("[BH1750] Siap.");
  }


  // ── Relay ── semua OFF saat boot
  pinMode(RELAY_FAN,  OUTPUT); setRelay(RELAY_FAN,  false);
  pinMode(RELAY_MIST, OUTPUT); setRelay(RELAY_MIST, false);
  pinMode(RELAY_LED,  OUTPUT); setRelay(RELAY_LED,  false);
  Serial.println("[RELAY] Semua relay OFF.");


  // ── WiFi & NTP ──
  connectWiFi();


  // ── Fetch threshold awal ──
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[BOOT] Mengambil threshold awal dari Supabase...");
    fetchThresholds();


    // Flush SPIFFS backup jika ada
    flushSPIFFS();
  }


  Serial.println("[BOOT] Sistem siap!\n");
}


// ================================================================
//  MAIN LOOP
// ================================================================
void loop() {
  // Auto-reconnect
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Koneksi putus. Reconnecting...");
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) {
      delay(10000);
      return;
    }
  }


  unsigned long now = millis();


  // ── 1. Baca & Kirim Sensor (tiap 30 detik) ──
  if (now - lastSensorTime >= SENSOR_INTERVAL || lastSensorTime == 0) {
    float suhu = readSuhu();
    float hum  = readKelembapan();
    float lux  = readLux();


    Serial.println("─────────────────────────────────────────────────");
    Serial.println("[SENSOR] Pembacaan:");
    Serial.print("  Suhu      : "); Serial.print(isnan(suhu) ? 0 : suhu, 1); Serial.println(" °C");
    Serial.print("  Kelembapan: "); Serial.print(isnan(hum)  ? 0 : hum,  1); Serial.println(" %");
    Serial.print("  Cahaya    : "); Serial.print(lux, 0); Serial.println(" lux");


    // Bangun payload JSON dengan nama kolom hyphen
    JsonDocument doc;
    if (!isnan(suhu)) doc["suhu-rumah-kaca"]  = round(suhu * 10) / 10.0;
    if (!isnan(hum))  doc["kelembapan"]        = round(hum  * 10) / 10.0;
    if (!isnan(lux))  doc["intensitas-cahaya"] = round(lux);
    String payload;
    serializeJson(doc, payload);
    Serial.print("[SENSOR] Payload: "); Serial.println(payload);


    // Kirim ke Supabase, backup ke SPIFFS jika gagal
    bool ok = sendSensorToSupabase(payload);
    if (!ok) saveToSPIFFS(payload);
    else     flushSPIFFS();


    // Evaluasi relay berdasarkan threshold saat ini
    if (!isnan(suhu)) applyRelayLogic(suhu, lux);


    lastSensorTime = now;
  }


  // ── 2. Fetch Threshold dari Supabase (tiap 15 detik) ──
  if (now - lastThresholdTime >= THRESHOLD_INTERVAL || lastThresholdTime == 0) {
    fetchThresholds();
    lastThresholdTime = now;
  }


  // ── 3. Lapor Status Relay ke Supabase (tiap 15 detik) ──
  if (now - lastReportTime >= REPORT_INTERVAL || lastReportTime == 0) {
    reportAllActuators();
    lastReportTime = now;
  }


  delay(100);
}
