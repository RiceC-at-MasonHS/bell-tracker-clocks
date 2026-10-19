#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <time.h>
#include <math.h>

// ================= USER CONFIGURATION =================
const char* WIFI_SSID     = "MCS-Guest";
const char* WIFI_PASSWORD = "";

const char* API_URL       = "https://storage.googleapis.com/mcs-bell-api/schedule.json";
const char* TARGET_BLDG   = "MHS"; // "MHS" or "MMS"
const char* TIMEZONE_TZ   = "EST5EDT,M3.2.0,M11.1.0"; // America/New_York

#define NEOPIXEL_PIN      32    // GPIO32 / D32 on ESP32
#define NUM_LEDS          8     // 8 physical LEDs total
// ======================================================

Adafruit_NeoPixel strip(NUM_LEDS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

const int STATUS_LED_INDEX = 0; // Index 0 (LED 1) = Connect / Status

// ================= COLOR PALETTE ======================
// Bell States
const uint32_t COLOR_OFF          = strip.Color(0, 0, 0);
const uint32_t COLOR_BELL_PAST     = strip.Color(10, 18, 26);  // Dim Slate (#2A3D4C scaled down)
const uint32_t COLOR_BELL_ACTIVE   = strip.Color(76, 187, 23);  // Kelly Green (#4CBB17)
const uint32_t COLOR_BELL_FUTURE   = strip.Color(31, 183, 229); // Electric Cyan (#1FB7E5)
const uint32_t COLOR_BREATH_GREEN  = strip.Color(0, 138, 0);   // Deep Green (#008A00)

// System Error Overlays (Applied to LED 1)
const uint32_t COLOR_ERR_WIFI     = strip.Color(255, 0, 0);    // Red (WiFi disconnect)
const uint32_t COLOR_ERR_NTP      = strip.Color(0, 255, 255);  // Electric Cyan (NTP Fail)
const uint32_t COLOR_ERR_API_NET  = strip.Color(255, 0, 255);  // Magenta (API Unreachable / Empty)
const uint32_t COLOR_ERR_API_DATE = strip.Color(255, 80, 0);   // Amber (API Wrong Date / Stale Data)
// ======================================================

struct BellInfo {
  bool is_scheduled = false;
  long start_epoch  = 0;
  long end_epoch    = 0;
};

BellInfo bells[8]; 

bool is_school_day  = false;
bool ntp_synced     = false;
bool api_net_ok     = false;
bool api_date_ok    = false;

unsigned long last_fetch_time = 0;
const unsigned long FETCH_INTERVAL_MS = 15 * 60 * 1000;
int last_scheduled_fetch_minute = -1;

bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.print(F("Connecting to WiFi..."));
  WiFi.persistent(false);
  WiFi.disconnect(true); 
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(F("."));
    strip.setPixelColor(STATUS_LED_INDEX, (attempts % 2 == 0) ? COLOR_ERR_WIFI : COLOR_OFF);
    strip.show();
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(F("\nWiFi Connected!"));
    return true;
  }
  
  Serial.println(F("\nWiFi Connection Failed!"));
  return false;
}

bool syncNTP() {
  configTzTime(TIMEZONE_TZ, "pool.ntp.org", "time.nist.gov");
  Serial.print(F("Syncing NTP time..."));
  time_t now = time(nullptr);
  int attempts = 0;
  
  while (now < 1700000000 && attempts < 20) {
    delay(500);
    Serial.print(F("."));
    strip.setPixelColor(STATUS_LED_INDEX, (attempts % 2 == 0) ? COLOR_ERR_NTP : COLOR_OFF);
    strip.show();
    time(&now);
    attempts++;
  }

  if (now >= 1700000000) {
    Serial.println(F("\nNTP Synced!"));
    ntp_synced = true;
    return true;
  }
  Serial.println(F("\nNTP Sync Failed!"));
  ntp_synced = false;
  return false;
}

bool fetchSchedule() {
  if (WiFi.status() != WL_CONNECTED) {
    if (!connectWiFi()) return false;
  }

  Serial.println(F("Fetching schedule from API..."));

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  
  api_net_ok  = false;
  api_date_ok = false;

  if (http.begin(client, API_URL)) {
    http.useHTTP10(true);
    http.setTimeout(10000);

    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();

      // Sanitize Payload (Strip leading BOM / trailing whitespace)
      int firstBrace = payload.indexOf('{');
      int lastBrace  = payload.lastIndexOf('}');
      if (firstBrace != -1 && lastBrace > firstBrace) {
        payload = payload.substring(firstBrace, lastBrace + 1);
      }

      // Reset Period Cache
      is_school_day = false;
      for (int i = 0; i < 8; i++) {
        bells[i].is_scheduled = false;
        bells[i].start_epoch  = 0;
        bells[i].end_epoch    = 0;
      }

      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload);

      if (!error) {
        api_net_ok = true; // Network & JSON structure valid

        // --- DATE VALIDATION CHECK ---
        time_t now_epoch;
        time(&now_epoch);
        struct tm timeinfo;
        localtime_r(&now_epoch, &timeinfo);

        char today_str[11];
        snprintf(today_str, sizeof(today_str), "%04d-%02d-%02d", 
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);

        const char* api_date = doc["date"] | "";
        
        if (ntp_synced && strlen(api_date) > 0 && strcmp(api_date, today_str) != 0) {
          Serial.printf("API DATE MISMATCH! API Date: '%s' | Clock Date: '%s'\n", api_date, today_str);
          api_date_ok = false;
        } else {
          api_date_ok = true;
        }

        is_school_day = doc["is_school_day"] | false;
        JsonArray buildings = doc["buildings"].as<JsonArray>();

        for (JsonObject bldg : buildings) {
          const char* bldg_id = bldg["building_id"];
          if (bldg_id && strcmp(bldg_id, TARGET_BLDG) == 0) {
            JsonArray periods = bldg["periods"].as<JsonArray>();
            for (JsonObject period : periods) {
              const char* p_id   = period["period_id"] | "";
              const char* p_name = period["name"] | "";

              int target_index = -1;

              if (strcasecmp(p_id, "Con") == 0 || strcasecmp(p_id, "CRW") == 0 || 
                  strcasecmp(p_id, "CONN") == 0 || strcasecmp(p_name, "Connect") == 0 ||
                  strcasecmp(p_name, "CREW") == 0) {
                target_index = 0;
              }
              else if (p_id[0] == 'B' || p_id[0] == 'P') {
                int bell_num = atoi(&p_id[1]);
                if (bell_num >= 1 && bell_num <= 7) {
                  target_index = bell_num;
                }
              }

              if (target_index != -1) {
                bells[target_index].is_scheduled = true;
                bells[target_index].start_epoch  = period["start_epoch"] | 0;
                bells[target_index].end_epoch    = period["end_epoch"] | 0;
              }
            }
            break;
          }
        }
        Serial.println(F("Schedule successfully parsed!"));
      } else {
        Serial.printf("JSON parse error: %s\n", error.c_str());
      }
    } else {
      Serial.printf("HTTP GET failed, code: %d\n", httpCode);
    }
    http.end();
  }

  return (api_net_ok && api_date_ok);
}

void checkScheduleRefresh(time_t now_epoch) {
  struct tm timeinfo;
  localtime_r(&now_epoch, &timeinfo);

  int current_minute_id = timeinfo.tm_yday * 1440 + timeinfo.tm_hour * 60 + timeinfo.tm_min;

  bool is_scheduled_time = (timeinfo.tm_hour == 1 && timeinfo.tm_min == 30) ||
                           (timeinfo.tm_hour == 7 && timeinfo.tm_min == 30);

  if (is_scheduled_time && (current_minute_id != last_scheduled_fetch_minute)) {
    Serial.println(F("Triggering scheduled API refresh..."));
    fetchSchedule();
    last_fetch_time = millis();
    last_scheduled_fetch_minute = current_minute_id;
  } 
  else if (!api_net_ok || !api_date_ok || !ntp_synced || (millis() - last_fetch_time > FETCH_INTERVAL_MS)) {
    if (!ntp_synced) syncNTP();
    fetchSchedule();
    last_fetch_time = millis();
  }
}

void setup() {
  Serial.begin(115200);

  strip.begin();
  strip.setBrightness(40);
  strip.show();

  connectWiFi();
  syncNTP();
  fetchSchedule();
  last_fetch_time = millis();
}

void loop() {
  time_t now_epoch;
  time(&now_epoch);

  checkScheduleRefresh(now_epoch);

  // 1. Collect all scheduled period indices
  int sched_count = 0;
  int sorted_indices[8]; 

  for (int i = 0; i < 8; i++) {
    if (bells[i].is_scheduled) {
      sorted_indices[sched_count++] = i;
    }
  }

  // 2. Sort active indices chronologically
  for (int i = 0; i < sched_count - 1; i++) {
    for (int j = i + 1; j < sched_count; j++) {
      if (bells[sorted_indices[i]].start_epoch > bells[sorted_indices[j]].start_epoch) {
        int temp = sorted_indices[i];
        sorted_indices[i] = sorted_indices[j];
        sorted_indices[j] = temp;
      }
    }
  }

  int active_bell   = -1;
  int passing_prev  = -1;
  int passing_next  = -1;

  // 3. Evaluate bell timeline
  for (int k = 0; k < sched_count; k++) {
    int curr_idx = sorted_indices[k];

    if (now_epoch >= bells[curr_idx].start_epoch && now_epoch < bells[curr_idx].end_epoch) {
      active_bell = curr_idx;
      break;
    }

    if (k < sched_count - 1) {
      int next_idx = sorted_indices[k + 1];
      if (now_epoch >= bells[curr_idx].end_epoch && now_epoch < bells[next_idx].start_epoch) {
        passing_prev = curr_idx;
        passing_next = next_idx;
        break;
      }
    }
  }

  // 4. Check off-hours status
  bool is_outside_school_day = !is_school_day;
  if (is_school_day && sched_count > 0) {
    long first_start_time = bells[sorted_indices[0]].start_epoch;
    long last_end_time    = bells[sorted_indices[sched_count - 1]].end_epoch;

    if (now_epoch < first_start_time || now_epoch >= last_end_time) {
      is_outside_school_day = true;
    }
  }

  // 5. Render LED States
  bool blink_state = (millis() / 500) % 2 == 0; // Toggles every 500ms

  // --- RENDER PERIOD LEDs (Index 0 = LED 1, Index 1..7 = LEDs 2..8) ---
  for (int i = 0; i < 8; i++) {
    if (!bells[i].is_scheduled) {
      strip.setPixelColor(i, COLOR_OFF);
    } 
    else if (i == active_bell) {
      strip.setPixelColor(i, COLOR_BELL_ACTIVE); // Kelly Green
    } 
    else if (passing_prev != -1 && i == passing_prev) {
      strip.setPixelColor(i, blink_state ? COLOR_BELL_PAST : COLOR_OFF);
    }
    else if (passing_next != -1 && i == passing_next) {
      strip.setPixelColor(i, blink_state ? COLOR_BELL_ACTIVE : COLOR_OFF);
    }
    else if (now_epoch >= bells[i].end_epoch) {
      strip.setPixelColor(i, COLOR_BELL_PAST);   // Dim Slate Accent
    }
    else {
      strip.setPixelColor(i, COLOR_BELL_FUTURE); // Electric Cyan
    }
  }

  // --- PRIORITY ERROR OVERLAYS FOR LED 1 ---
  if (WiFi.status() != WL_CONNECTED) {
    strip.setPixelColor(STATUS_LED_INDEX, blink_state ? COLOR_ERR_WIFI : COLOR_OFF);
  } 
  else if (!ntp_synced) {
    strip.setPixelColor(STATUS_LED_INDEX, blink_state ? COLOR_ERR_NTP : COLOR_OFF);
  } 
  else if (!api_net_ok) {
    strip.setPixelColor(STATUS_LED_INDEX, blink_state ? COLOR_ERR_API_NET : COLOR_OFF);
  } 
  else if (!api_date_ok) {
    strip.setPixelColor(STATUS_LED_INDEX, blink_state ? COLOR_ERR_API_DATE : COLOR_OFF);
  } 
  else if (is_outside_school_day) {
    float sine_val = (sin(millis() * 0.002) + 1.0) / 2.0;
    uint8_t r = (uint8_t)(sine_val * 0.0);
    uint8_t g = (uint8_t)(sine_val * 138.0); // Modulated #008A00
    uint8_t b = (uint8_t)(sine_val * 0.0);
    strip.setPixelColor(STATUS_LED_INDEX, strip.Color(r, g, b));
  }

  strip.show();
  delay(100);
}
