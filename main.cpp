#include <M5Core2.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "mt15_icon.h"   // provides mt15_icon_map[] (RGB565, 160x100)

// ==== WIFI / MERAKI CONFIG ====

#define WIFI_SSID       "WiFi_SSID"
#define WIFI_PASS       "password"
#define MERAKI_API_KEY  "API_KEY"
#define MERAKI_ORG_ID   "Org_ID"
#define MT15_SERIAL     "Serial_Number"

// Latest metrics endpoint (all metrics)
const char *MERAKI_URL =
    "https://api.meraki.com/api/v1/organizations/" MERAKI_ORG_ID
    "/sensor/readings/latest?serials[]=" MT15_SERIAL;

// 30-day temp history endpoint (daily buckets)
const char *MERAKI_URL_TEMP_30D =
    "https://api.meraki.com/api/v1/organizations/" MERAKI_ORG_ID
    "/sensor/readings/history/byInterval"
    "?serials[]=" MT15_SERIAL
    "&metrics[]=temperature"
    "&timespan=2592000"   // 30 days in seconds
    "&interval=86400";    // 1 day buckets

// 30-day humidity history endpoint (daily buckets)
const char *MERAKI_URL_HUM_30D =
    "https://api.meraki.com/api/v1/organizations/" MERAKI_ORG_ID
    "/sensor/readings/history/byInterval"
    "?serials[]=" MT15_SERIAL
    "&metrics[]=humidity"
    "&timespan=2592000"
    "&interval=86400";

// Refresh interval in ms
const unsigned long REFRESH_INTERVAL_MS = 60000;

// ==== STATE ====

WiFiClientSecure secureClient;
unsigned long lastFetch = 0;

// Latest sensor values
double g_tempC       = NAN;
double g_humidityPct = NAN;
double g_co2Ppm      = NAN;
double g_noiseDb     = NAN;   // ambient noise level (dB)
double g_pm25        = NAN;   // pm2.5 ug/m3
double g_tvoc        = NAN;   // TVOC ppb
double g_iaqScore    = NAN;   // indoor air quality score (0–100)

// 30-day histories
const int MAX_TEMP_POINTS = 32;        // we only need ~30
float g_tempHistory[MAX_TEMP_POINTS];  // daily avg temps in C
float g_humHistory[MAX_TEMP_POINTS];   // daily avg humidity %
int   g_tempHistoryCount = 0;
char  g_tempDateLabel[MAX_TEMP_POINTS][6];  // "MM/DD" + '\0'

// Layout constants
const int ICON_X      = 10;
const int ICON_Y      = 10;
const int ICON_W      = 160;
const int ICON_H      = 100;

const int TITLE_X       = 190;
const int TITLE_Y       = 10;

const int WIFI_STATUS_X = 190;
const int WIFI_STATUS_Y = 55;   // below title

const int METRIC_LABEL_X = 10;
const int METRIC_VALUE_X = 120;
const int METRIC_BASE_Y  = ICON_Y + ICON_H + 8;  // below icon
const int METRIC_LINE_H  = 12;                   // tight spacing

// ==== PAGE / SWIPE STATE ====

enum PageId {
    PAGE_LIVE         = 0,
    PAGE_TEMP_HISTORY = 1,
    PAGE_HUM_HISTORY  = 2,
    PAGE_MAX          = 3
};

int g_currentPage = PAGE_LIVE;

// Forward declaration
void drawHumHistoryPage();

// ==== UI HELPERS: LIVE PAGE ====

void drawStaticLayout()
{
    M5.Lcd.fillScreen(TFT_BLACK);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);

    // Icon
    M5.Lcd.pushImage(ICON_X, ICON_Y, ICON_W, ICON_H, (uint16_t *)mt15_icon_map);

    // Title
    M5.Lcd.setTextDatum(TL_DATUM);
    M5.Lcd.setTextSize(2);
    M5.Lcd.drawString("Meraki MT15", TITLE_X, TITLE_Y);

    // WiFi status placeholder
    M5.Lcd.fillRect(WIFI_STATUS_X, WIFI_STATUS_Y,
                    120, METRIC_LINE_H * 2, TFT_BLACK);
    M5.Lcd.setTextColor(TFT_YELLOW, TFT_BLACK);
    M5.Lcd.drawString("WiFi ...", WIFI_STATUS_X, WIFI_STATUS_Y);

    // Metric labels under the icon (small font)
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Lcd.setTextSize(1);
    M5.Lcd.setTextDatum(TL_DATUM);

    int y = METRIC_BASE_Y;

    M5.Lcd.drawString("Temp:",  METRIC_LABEL_X, y); y += METRIC_LINE_H;
    M5.Lcd.drawString("Hum :",  METRIC_LABEL_X, y); y += METRIC_LINE_H;
    M5.Lcd.drawString("CO2 :",  METRIC_LABEL_X, y); y += METRIC_LINE_H;
    M5.Lcd.drawString("dB  :",  METRIC_LABEL_X, y); y += METRIC_LINE_H;
    M5.Lcd.drawString("PM2.5:", METRIC_LABEL_X, y); y += METRIC_LINE_H;
    M5.Lcd.drawString("TVOC:",  METRIC_LABEL_X, y); y += METRIC_LINE_H;
    M5.Lcd.drawString("IAQ :",  METRIC_LABEL_X, y);
}

void updateSensorText()
{
    char buf[32];

    M5.Lcd.setTextDatum(TL_DATUM);
    M5.Lcd.setTextSize(1);                 // match labels
    M5.Lcd.setTextColor(TFT_GREEN, TFT_BLACK);

    int y = METRIC_BASE_Y;

    // Temperature
    M5.Lcd.fillRect(METRIC_VALUE_X, y, 200, METRIC_LINE_H, TFT_BLACK);
    if (!isnan(g_tempC)) {
        snprintf(buf, sizeof(buf), "%.2f C", g_tempC);
        M5.Lcd.drawString(buf, METRIC_VALUE_X, y);
    } else {
        M5.Lcd.drawString("--.- C", METRIC_VALUE_X, y);
    }
    y += METRIC_LINE_H;

    // Humidity
    M5.Lcd.fillRect(METRIC_VALUE_X, y, 200, METRIC_LINE_H, TFT_BLACK);
    if (!isnan(g_humidityPct)) {
        snprintf(buf, sizeof(buf), "%.0f %%", g_humidityPct);
        M5.Lcd.drawString(buf, METRIC_VALUE_X, y);
    } else {
        M5.Lcd.drawString("-- %", METRIC_VALUE_X, y);
    }
    y += METRIC_LINE_H;

    // CO2
    M5.Lcd.fillRect(METRIC_VALUE_X, y, 200, METRIC_LINE_H, TFT_BLACK);
    if (!isnan(g_co2Ppm)) {
        snprintf(buf, sizeof(buf), "%.0f ppm", g_co2Ppm);
        M5.Lcd.drawString(buf, METRIC_VALUE_X, y);
    } else {
        M5.Lcd.drawString("--- ppm", METRIC_VALUE_X, y);
    }
    y += METRIC_LINE_H;

    // Noise
    M5.Lcd.fillRect(METRIC_VALUE_X, y, 200, METRIC_LINE_H, TFT_BLACK);
    if (!isnan(g_noiseDb)) {
        snprintf(buf, sizeof(buf), "%.0f dB", g_noiseDb);
        M5.Lcd.drawString(buf, METRIC_VALUE_X, y);
    } else {
        M5.Lcd.drawString("-- dB", METRIC_VALUE_X, y);
    }
    y += METRIC_LINE_H;

    // PM2.5
    M5.Lcd.fillRect(METRIC_VALUE_X, y, 200, METRIC_LINE_H, TFT_BLACK);
    if (!isnan(g_pm25)) {
        snprintf(buf, sizeof(buf), "%.0f ug/m3", g_pm25);
        M5.Lcd.drawString(buf, METRIC_VALUE_X, y);
    } else {
        M5.Lcd.drawString("-- ug/m3", METRIC_VALUE_X, y);
    }
    y += METRIC_LINE_H;

    // TVOC
    M5.Lcd.fillRect(METRIC_VALUE_X, y, 200, METRIC_LINE_H, TFT_BLACK);
    if (!isnan(g_tvoc)) {
        snprintf(buf, sizeof(buf), "%.0f ppb", g_tvoc);
        M5.Lcd.drawString(buf, METRIC_VALUE_X, y);
    } else {
        M5.Lcd.drawString("-- ppb", METRIC_VALUE_X, y);
    }
    y += METRIC_LINE_H;

    // IAQ
    M5.Lcd.fillRect(METRIC_VALUE_X, y, 200, METRIC_LINE_H, TFT_BLACK);
    if (!isnan(g_iaqScore)) {
        snprintf(buf, sizeof(buf), "%.0f /100", g_iaqScore);
        M5.Lcd.drawString(buf, METRIC_VALUE_X, y);
    } else {
        M5.Lcd.drawString("-- /100", METRIC_VALUE_X, y);
    }
}

// ==== TEMP HISTORY PAGE ====

void drawTempHistoryPage()
{
    M5.Lcd.fillScreen(TFT_BLACK);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Lcd.setTextDatum(TL_DATUM);

    // Title
    M5.Lcd.setTextSize(2);
    M5.Lcd.drawString("Temp last 30 days (C)", 10, 10);

    if (g_tempHistoryCount <= 1) {
        M5.Lcd.setTextSize(1);
        M5.Lcd.drawString("Not enough data", 10, 40);
        return;
    }

    // Plot area
    int x0 = 10;
    int y0 = 40;
    int w  = 280;   // narrower so labels fit on right
    int h  = 180;

    // Find min/max (skip NaN)
    float tMin = 1e9, tMax = -1e9;
    for (int i = 0; i < g_tempHistoryCount; ++i) {
        float v = g_tempHistory[i];
        if (isnan(v)) continue;
        if (v < tMin) tMin = v;
        if (v > tMax) tMax = v;
    }

    if (tMin > tMax) {
        M5.Lcd.setTextSize(1);
        M5.Lcd.drawString("No valid temps", 10, 40);
        return;
    }

    // Pad the y range slightly
    float margin = 0.5f;
    tMin -= margin;
    tMax += margin;

    // Draw border
    M5.Lcd.drawRect(x0, y0, w, h, TFT_DARKGREY);

    // Sparkline
    int prevX = -1, prevY = -1;
    for (int i = 0; i < g_tempHistoryCount; ++i) {
        float v = g_tempHistory[i];
        if (isnan(v)) continue;

        float frac = (v - tMin) / (tMax - tMin + 1e-6f); // 0..1
        int x = x0 + (int)((float)i * (w - 1) / (g_tempHistoryCount - 1));
        int y = y0 + h - 1 - (int)(frac * (h - 2));

        if (prevX >= 0) {
            M5.Lcd.drawLine(prevX, prevY, x, y, TFT_CYAN);
        }
        prevX = x;
        prevY = y;
    }

    // Y-axis labels (pulled in a bit so they don’t clip)
    M5.Lcd.setTextSize(1);
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1fC", tMax);
    M5.Lcd.drawString(buf, x0 + w + 4, y0);
    snprintf(buf, sizeof(buf), "%.1fC", tMin);
    M5.Lcd.drawString(buf, x0 + w + 4, y0 + h - 8);

    // --- Weekly vertical dashes + MM/DD labels ---

    int lastIdx = g_tempHistoryCount - 1;
    int baseY   = y0 + h;        // axis baseline for ticks
    int labelY  = baseY + 4;     // text just below ticks

    // indices we want: today, -7, -14, -21, -28 (if they exist)
    int weekOffsets[] = {0, 7, 14, 21, 28};
    int numOffsets    = sizeof(weekOffsets) / sizeof(weekOffsets[0]);

    for (int k = 0; k < numOffsets; ++k) {
        int idx = lastIdx - weekOffsets[k];
        if (idx < 0 || idx >= g_tempHistoryCount) continue;

        int x = x0 + (int)((float)idx * (w - 1) / (g_tempHistoryCount - 1));

        // vertical hashmark across the plot
        M5.Lcd.drawLine(x, y0, x, y0 + h, TFT_DARKGREY);

        // small tick at the bottom
        M5.Lcd.drawLine(x, baseY, x, baseY + 2, TFT_DARKGREY);

        // MM/DD label under the tick
        M5.Lcd.setTextSize(1);
        int textX = x - 10;  // roughly center under tick
        if (textX < 0) textX = 0;
        if (textX > 320 - 24) textX = 320 - 24;

        M5.Lcd.drawString(g_tempDateLabel[idx], textX, labelY);
    }
}

// ==== HUMIDITY HISTORY PAGE ====

void drawHumHistoryPage()
{
    M5.Lcd.fillScreen(TFT_BLACK);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Lcd.setTextDatum(TL_DATUM);

    // Title
    M5.Lcd.setTextSize(2);
    M5.Lcd.drawString("Humidity last 30 days", 10, 10);

    if (g_tempHistoryCount <= 1) {
        M5.Lcd.setTextSize(1);
        M5.Lcd.drawString("Not enough data", 10, 40);
        return;
    }

    // Plot area
    int x0 = 10;
    int y0 = 40;
    int w  = 280;
    int h  = 180;

    // Find min/max humidity (skip NaN)
    float hMin =  1e9, hMax = -1e9;
    for (int i = 0; i < g_tempHistoryCount; ++i) {
        float v = g_humHistory[i];
        if (isnan(v)) continue;
        if (v < hMin) hMin = v;
        if (v > hMax) hMax = v;
    }

    if (hMin > hMax) {
        M5.Lcd.setTextSize(1);
        M5.Lcd.drawString("No valid humidity", 10, 40);
        return;
    }

    // Pad the y range a bit
    float margin = 3.0f;
    hMin -= margin;
    hMax += margin;

    // Draw border
    M5.Lcd.drawRect(x0, y0, w, h, TFT_DARKGREY);

    // Draw sparkline
    int prevX = -1, prevY = -1;
    for (int i = 0; i < g_tempHistoryCount; ++i) {
        float v = g_humHistory[i];
        if (isnan(v)) continue;

        float frac = (v - hMin) / (hMax - hMin + 1e-6f); // 0..1
        int x = x0 + (int)((float)i * (w - 1) / (g_tempHistoryCount - 1));
        int y = y0 + h - 1 - (int)(frac * (h - 2));

        if (prevX >= 0) {
            M5.Lcd.drawLine(prevX, prevY, x, y, TFT_CYAN);
        }
        prevX = x;
        prevY = y;
    }

    // Y-axis labels
    M5.Lcd.setTextSize(1);
    char buf[16];
    snprintf(buf, sizeof(buf), "%.0f%%", hMax);
    M5.Lcd.drawString(buf, x0 + w + 4, y0);
    snprintf(buf, sizeof(buf), "%.0f%%", hMin);
    M5.Lcd.drawString(buf, x0 + w + 4, y0 + h - 8);

    // Weekly ticks based on g_tempDateLabel[]
    int n = g_tempHistoryCount;
    int baseY = y0 + h;
    int labelY = baseY + 4;

    int weekOffsets[] = {0, 7, 14, 21, 28};
    int numOffsets    = sizeof(weekOffsets) / sizeof(weekOffsets[0]);
    int lastIdx       = n - 1;

    for (int k = 0; k < numOffsets; ++k) {
        int idx = lastIdx - weekOffsets[k];
        if (idx < 0 || idx >= n) continue;

        int x = x0 + (int)((float)idx * (w - 1) / (n - 1));

        // tiny vertical tick at bottom
        M5.Lcd.drawLine(x, baseY, x, baseY + 2, TFT_DARKGREY);

        int textX = x - 10;
        if (textX < 0) textX = 0;
        if (textX > 320 - 24) textX = 320 - 24;

        M5.Lcd.drawString(g_tempDateLabel[idx], textX, labelY);
    }
}

// ==== PAGE DISPATCH ====

void drawCurrentPage()
{
    if (g_currentPage == PAGE_LIVE) {
        drawStaticLayout();
        updateSensorText();
    } else if (g_currentPage == PAGE_TEMP_HISTORY) {
        drawTempHistoryPage();
    } else if (g_currentPage == PAGE_HUM_HISTORY) {
        drawHumHistoryPage();
    }
}

// ==== MERAKI FETCH + PARSE (LATEST) ====

bool fetchMT15Once()
{
    HTTPClient http;
    secureClient.setInsecure();  // demo: no CA pinning

    if (!http.begin(secureClient, MERAKI_URL)) {
        Serial.println("[HTTP] begin() failed");
        return false;
    }

    http.addHeader("X-Cisco-Meraki-API-Key", MERAKI_API_KEY);
    http.addHeader("Accept", "application/json");

    int httpCode = http.GET();
    if (httpCode <= 0) {
        Serial.printf("[HTTP] GET failed: %s\n",
                      http.errorToString(httpCode).c_str());
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    Serial.printf("[HTTP] Status: %d, len=%d\n", httpCode, payload.length());
    // Serial.println(payload); // uncomment for raw JSON

    if (httpCode != 200) {
        Serial.println("[HTTP] Non-200 status");
        return false;
    }

    StaticJsonDocument<2048> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    if (!doc.is<JsonArray>()) {
        Serial.println("Root is not array");
        return false;
    }

    JsonArray rootArr = doc.as<JsonArray>();
    if (rootArr.size() == 0) {
        Serial.println("Empty readings array");
        return false;
    }

    JsonObject sensor  = rootArr[0];
    JsonArray  readings = sensor["readings"].as<JsonArray>();
    if (readings.isNull()) {
        Serial.println("No 'readings' array");
        return false;
    }

    // reset
    g_tempC       = NAN;
    g_humidityPct = NAN;
    g_co2Ppm      = NAN;
    g_noiseDb     = NAN;
    g_pm25        = NAN;
    g_tvoc        = NAN;
    g_iaqScore    = NAN;

    for (JsonObject r : readings) {
        const char *metric = r["metric"] | "";

        if (strcmp(metric, "temperature") == 0) {
            JsonObject temp = r["temperature"];
            if (!temp.isNull() && temp["celsius"].is<double>()) {
                g_tempC = temp["celsius"].as<double>();
            }

        } else if (strcmp(metric, "humidity") == 0) {
            JsonObject hum = r["humidity"];
            if (!hum.isNull() && hum["relativePercentage"].is<double>()) {
                g_humidityPct = hum["relativePercentage"].as<double>();
            }

        } else if (strcmp(metric, "co2") == 0) {
            JsonObject co2 = r["co2"];
            if (!co2.isNull() && co2["concentration"].is<double>()) {
                g_co2Ppm = co2["concentration"].as<double>();
            }

        } else if (strcmp(metric, "noise") == 0) {
            JsonObject amb = r["noise"]["ambient"];
            if (!amb.isNull() && amb["level"].is<double>()) {
                g_noiseDb = amb["level"].as<double>();
            }

        } else if (strcmp(metric, "pm25") == 0) {
            JsonObject pm = r["pm25"];
            if (!pm.isNull() && pm["concentration"].is<double>()) {
                g_pm25 = pm["concentration"].as<double>();
            }

        } else if (strcmp(metric, "tvoc") == 0) {
            JsonObject tv = r["tvoc"];
            if (!tv.isNull() && tv["concentration"].is<double>()) {
                g_tvoc = tv["concentration"].as<double>();
            }

        } else if (strcmp(metric, "indoorAirQuality") == 0) {
            JsonObject iaq = r["indoorAirQuality"];
            if (!iaq.isNull() && iaq["score"].is<double>()) {
                g_iaqScore = iaq["score"].as<double>();
            }
        }
    }

    Serial.printf("MT15 latest: T=%.2fC H=%.1f%% CO2=%.0fppm\n",
                  g_tempC, g_humidityPct, g_co2Ppm);

    return true;
}

// ==== MERAKI FETCH + PARSE (30D TEMP HISTORY) ====

bool fetchMT15TempHistory30d()
{
    HTTPClient http;
    secureClient.setInsecure();  // demo: no CA pinning

    if (!http.begin(secureClient, MERAKI_URL_TEMP_30D)) {
        Serial.println("[HTTP-30d] begin() failed");
        return false;
    }

    http.addHeader("X-Cisco-Meraki-API-Key", MERAKI_API_KEY);
    http.addHeader("Accept", "application/json");

    int httpCode = http.GET();
    if (httpCode <= 0) {
        Serial.printf("[HTTP-30d] GET failed: %s\n",
                      http.errorToString(httpCode).c_str());
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    Serial.printf("[HTTP-30d] Status: %d, len=%d\n", httpCode, payload.length());
    // Serial.println(payload); // debug if needed

    if (httpCode != 200) {
        Serial.println("[HTTP-30d] Non-200 status");
        return false;
    }

    StaticJsonDocument<8192> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("[HTTP-30d] JSON parse error: %s\n", err.c_str());
        return false;
    }

    if (!doc.is<JsonArray>()) {
        Serial.println("[HTTP-30d] Root is not array");
        return false;
    }

    JsonArray arr = doc.as<JsonArray>();
    int n = arr.size();
    if (n <= 0) {
        Serial.println("[HTTP-30d] Empty array");
        return false;
    }

    if (n > MAX_TEMP_POINTS) n = MAX_TEMP_POINTS;

    // API returns newest first; we want oldest at index 0
    g_tempHistoryCount = 0;
    for (int i = 0; i < n; ++i) {
        JsonObject item = arr[i];

        // temperature.celsius.average
        double tempAvg = NAN;
        JsonObject tempObj = item["temperature"]["celsius"];
        if (!tempObj.isNull() && tempObj["average"].is<double>()) {
            tempAvg = tempObj["average"].as<double>();
        }

        // date label from startTs "YYYY-MM-DDTHH:MM:SSZ"
        const char *startTs = item["startTs"] | "";
        char mmdd[6] = "??/??";
        if (startTs && strlen(startTs) >= 10) {
            mmdd[0] = startTs[5];
            mmdd[1] = startTs[6];
            mmdd[2] = '/';
            mmdd[3] = startTs[8];
            mmdd[4] = startTs[9];
            mmdd[5] = '\0';
        }

        int targetIndex = n - 1 - i; // reverse order (oldest at 0)

        // store temp
        if (!isnan(tempAvg)) {
            g_tempHistory[targetIndex] = (float)tempAvg;
        } else {
            g_tempHistory[targetIndex] = NAN;
        }

        // store label even if value is NAN
        strncpy(g_tempDateLabel[targetIndex],
                mmdd,
                sizeof(g_tempDateLabel[targetIndex]));
        g_tempDateLabel[targetIndex][5] = '\0';
    }

    g_tempHistoryCount = n;

    Serial.printf("[HTTP-30d] Parsed %d daily temp points\n", g_tempHistoryCount);
    for (int i = 0; i < g_tempHistoryCount; ++i) {
        Serial.printf("  day[%02d] = %.2f C (%s)\n",
                      i, g_tempHistory[i], g_tempDateLabel[i]);
    }

    return true;
}

// ==== MERAKI FETCH + PARSE (30D HUMIDITY HISTORY) ====

bool fetchMT15HumHistory30d()
{
    HTTPClient http;
    secureClient.setInsecure();  // demo: no CA pinning

    if (!http.begin(secureClient, MERAKI_URL_HUM_30D)) {
        Serial.println("[HTTP-30d-HUM] begin() failed");
        return false;
    }

    http.addHeader("X-Cisco-Meraki-API-Key", MERAKI_API_KEY);
    http.addHeader("Accept", "application/json");

    int httpCode = http.GET();
    if (httpCode <= 0) {
        Serial.printf("[HTTP-30d-HUM] GET failed: %s\n",
                      http.errorToString(httpCode).c_str());
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    Serial.printf("[HTTP-30d-HUM] Status: %d, len=%d\n", httpCode, payload.length());
    // Serial.println(payload); // debug if needed

    if (httpCode != 200) {
        Serial.println("[HTTP-30d-HUM] Non-200 status");
        return false;
    }

    StaticJsonDocument<8192> doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        Serial.printf("[HTTP-30d-HUM] JSON parse error: %s\n", err.c_str());
        return false;
    }

    if (!doc.is<JsonArray>()) {
        Serial.println("[HTTP-30d-HUM] Root is not array");
        return false;
    }

    JsonArray arr = doc.as<JsonArray>();
    int n = arr.size();
    if (n <= 0) {
        Serial.println("[HTTP-30d-HUM] Empty array");
        return false;
    }

    if (n > MAX_TEMP_POINTS) n = MAX_TEMP_POINTS;

    // We assume same intervals as temp; oldest at index 0
    for (int i = 0; i < n; ++i) {
        JsonObject item = arr[i];

        double humAvg = NAN;
        JsonObject humRoot = item["humidity"];
        if (!humRoot.isNull()) {
            JsonObject rel = humRoot["relativePercentage"];
            if (!rel.isNull()) {
                if (rel["average"].is<double>()) {
                    humAvg = rel["average"].as<double>();
                }
            } else if (humRoot["relativePercentage"].is<double>()) {
                humAvg = humRoot["relativePercentage"].as<double>();
            }
        }

        int targetIndex = n - 1 - i; // reverse order (oldest at 0)
        if (!isnan(humAvg)) {
            g_humHistory[targetIndex] = (float)humAvg;
        } else {
            g_humHistory[targetIndex] = NAN;
        }
    }

    Serial.println("[HTTP-30d-HUM] Filled humidity history");
    for (int i = 0; i < g_tempHistoryCount && i < n; ++i) {
        Serial.printf("  hum[%02d] = %.2f %% (%s)\n",
                      i, g_humHistory[i], g_tempDateLabel[i]);
    }

    return true;
}

// ==== WIFI STATUS LINE ====

void connectWiFi()
{
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 40) { // ~20 seconds
        delay(500);
        retries++;
    }

    // Update WiFi status under the title
    M5.Lcd.fillRect(WIFI_STATUS_X, WIFI_STATUS_Y,
                    120, METRIC_LINE_H * 2, TFT_BLACK);
    M5.Lcd.setTextDatum(TL_DATUM);
    M5.Lcd.setTextSize(2);

    if (WiFi.status() == WL_CONNECTED) {
        M5.Lcd.setTextColor(TFT_GREEN, TFT_BLACK);
        M5.Lcd.drawString("WiFi OK", WIFI_STATUS_X, WIFI_STATUS_Y);
        Serial.print("WiFi connected, IP: ");
        Serial.println(WiFi.localIP());
    } else {
        M5.Lcd.setTextColor(TFT_RED, TFT_BLACK);
        M5.Lcd.drawString("WiFi FAIL", WIFI_STATUS_X, WIFI_STATUS_Y);
        Serial.println("WiFi connect failed");
    }
}

// ==== SWIPE HANDLING ====

void handleSwipe()
{
    static bool   touchActive = false;
    static int16_t startX = 0, startY = 0;
    static int16_t lastX  = 0, lastY  = 0;

    if (M5.Touch.ispressed()) {
        Point p = M5.Touch.getPressPoint();

        if (!touchActive) {
            touchActive = true;
            startX = lastX = p.x;
            startY = lastY = p.y;
        } else {
            lastX = p.x;
            lastY = p.y;
        }
    } else if (touchActive) {
        int dx = lastX - startX;
        int dy = lastY - startY;

        const int SWIPE_THRESHOLD = 50;

        if (abs(dx) > SWIPE_THRESHOLD && abs(dx) > abs(dy)) {
            if (dx < 0 && g_currentPage < PAGE_MAX - 1) {
                g_currentPage++;
                drawCurrentPage();
            } else if (dx > 0 && g_currentPage > 0) {
                g_currentPage--;
                drawCurrentPage();
            }
        }

        touchActive = false;
    }
}

// ==== ARDUINO SETUP / LOOP ====

void setup()
{
    M5.begin(true, true, true, true);

    Serial.begin(115200);
    delay(200);

    drawStaticLayout();
    connectWiFi();

    // Initial data fetch so all pages have something to draw
    if (WiFi.status() == WL_CONNECTED) {
        fetchMT15Once();
        fetchMT15TempHistory30d();
        fetchMT15HumHistory30d();
    }

    drawCurrentPage();
    lastFetch = millis();
}

void loop()
{
    M5.update();
    handleSwipe();

    unsigned long now = millis();
    if (now - lastFetch >= REFRESH_INTERVAL_MS || lastFetch == 0) {
        if (WiFi.status() == WL_CONNECTED) {
            // Always refresh live + both histories
            fetchMT15Once();
            fetchMT15TempHistory30d();
            fetchMT15HumHistory30d();

            // Redraw current page
            if (g_currentPage == PAGE_LIVE) {
                updateSensorText();
            } else if (g_currentPage == PAGE_TEMP_HISTORY) {
                drawTempHistoryPage();
            } else if (g_currentPage == PAGE_HUM_HISTORY) {
                drawHumHistoryPage();
            }
        } else {
            connectWiFi();
        }
        lastFetch = now;
    }

    delay(100);
}
