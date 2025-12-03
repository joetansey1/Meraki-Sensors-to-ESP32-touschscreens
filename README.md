Meraki MT15 Air-Quality Dashboard (M5Core2)

This project displays live and 30-day historical Meraki MT15 sensor data on an M5Stack M5Core2 device.
It provides a touch-based interface with swipe navigation and renders sparklines for temperature and humidity history.

The device becomes a compact, real-time MT15 “desktop widget” that continuously shows:

Temperature (°C)
Humidity (%)
CO₂ (ppm)
Ambient noise (dB)
PM2.5 (µg/m³)
TVOC (ppb)
Indoor Air Quality Score (IAQ)

WiFi status
30-day Temp Sparkline
30-day Humidity Sparkline

All sensor data is fetched directly from the Meraki Dashboard API using your API key and MT15 serial number.

 Screens

Page 1: LIVE sensor metrics
Page 2: Temperature 30-day sparkline (with weekly ticks + MM/DD labels)
Page 3: Humidity 30-day sparkline
Swipe left/right to switch pages.

 Hardware Requirements
Component	Notes
M5Stack M5Core2	Touchscreen ESP32 unit (480×320)
USB-C cable	For flashing firmware
WiFi access	Required for Meraki API calls
Meraki MT15 Sensor	Any deployed MT15
Meraki API Key	Full or read-only org key

Optional:

3D-printed stand
LiPo battery for portable operation

Software Requirements
PlatformIO (VSCode extension recommended)
Arduino framework
Libraries (auto-installed by PlatformIO):

M5Core2
ArduinoJson
WiFiClientSecure
HTTPClient

Install & Build Instructions
1. Clone this repo
git clone https://github.com/<yourname>/mt15-m5core2-dashboard.git
cd mt15-m5core2-dashboard

2. Open in VSCode + PlatformIO

Open the folder in VSCode
PlatformIO will auto-detect platformio.ini
Dependencies auto-install at build time

3. Edit Your Configuration

Open src/main.cpp and modify the following lines:

#define WIFI_SSID       "YourSSID"
#define WIFI_PASS       "YourPassword"
#define MERAKI_API_KEY  "YourMerakiAPIKey"
#define MERAKI_ORG_ID   "123456"
#define MT15_SERIAL     "Qxxx-xxxx-xxxx"


Your MT15 serial format should be exactly as displayed in the Meraki dashboard.

4. Build
pio run
5. Flash Firmware to M5Core2
Connect via USB-C:
pio run --target upload


Monitor logs:
pio device monitor

API Endpoints Used
Latest live metrics
GET /api/v1/organizations/{orgId}/sensor/readings/latest?serials[]={serial}

30-day temperature history
GET /api/v1/organizations/{orgId}/sensor/readings/history/byInterval
    ?serials[]={serial}
    &metrics[]=temperature
    &timespan=2592000
    &interval=86400

30-day humidity history
GET /api/v1/organizations/{orgId}/sensor/readings/history/byInterval
    ?serials[]={serial}
    &metrics[]=humidity
    &timespan=2592000
    &interval=86400

All calls are made securely using WiFiClientSecure (TLS), though CA validation is disabled for demo builds.

Touch / Swipe Navigation
Swipe left → next page
Swipe right ← previous page
Pages refresh every 60 seconds
WiFi reconnects automatically if dropped

Troubleshooting
No 30-day data appears
Ensure the MT15 has been online for >24h
The Meraki API returns newest→oldest, code reverses this correctly
Check the serial number is exact
Confirm your API key has org-read permissions
“WiFi FAIL”
Verify SSID & password
Hotspots sometimes block long-running TLS; try a normal AP
Touchscreen not responding
Remove USB power for 5 seconds
The M5Core2 touch controller occasionally needs a reset
JSON parse error
Ensure the API key is valid
Ensure your org actually contains the given MT15

 Repo Structure
/src
    main.cpp
/assets
    mt15_icon.h
platformio.ini
README.md
