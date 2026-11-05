// SPDX-License-Identifier: MIT
// Based on Deskbuddy by LextZip; Plane Radar portions adapted from MatixYo.
// Deskbuddy V.8
// Nav: Weather / Stocks / Flight Radar / Weather Radar; Status via top shortcut
// Full version + integrated Plane Radar 1.1.5 ADS-B page
// - KP dots replaced with Low / Medium / High / Extreme text
// - KP level text uses same small font as wind direction and stays inside the box
// - Stocks page shows Dow/Nasdaq intraday changes from the market opening value
// - Wind direction uses Accent color
// - Weather data retained for configurable Home widgets
// - Uptime added to Status page

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <TFT_eSPI.h>
#include <time.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <math.h>
#include <esp_system.h>
#include "PlaneRadarMap.h"
#include "MarketCalendar.h"
struct IndexChart;

// =========================================================
// WIFI
// =========================================================
const char* WIFI_SSID = ""; // Configure Wi-Fi on the touchscreen after first boot.
const char* WIFI_PASS = ""; // Configure Wi-Fi on the touchscreen after first boot.

// =========================================================
// DISPLAY / TOUCH
// =========================================================
TFT_eSPI tft;

const int ROT = 0;
const bool INV = false;

#define TOUCH_CS  33
#define TOUCH_IRQ 36

static const int T_SCK  = 25;
static const int T_MISO = 39;
static const int T_MOSI = 32;

SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(TOUCH_CS);

static const int TOUCH_X_MIN = 562;
static const int TOUCH_X_MAX = 3604;
static const int TOUCH_Y_MIN = 544;
static const int TOUCH_Y_MAX = 3720;

static const bool TOUCH_SWAP_XY = false;
static const bool TOUCH_FLIP_X  = false;
static const bool TOUCH_FLIP_Y  = false;

// Affine transform: screen X/Y each use both raw touch axes.
float touchCal[6] = {0, 0, 0, 0, 0, 0};
bool touchCalValid = false;
void runTouchCalibration();

// =========================================================
// WEB / STORAGE
// =========================================================
WebServer server(80);
Preferences prefs;

// Keep TLS connections from competing for the ESP32's limited heap.
SemaphoreHandle_t networkFetchMutex = nullptr;
class NetworkFetchGuard {
 public:
  bool locked = false;
  NetworkFetchGuard(TickType_t wait = portMAX_DELAY) {
    if (networkFetchMutex) locked = xSemaphoreTake(networkFetchMutex, wait) == pdTRUE;
  }
  void release() {
    if (locked) { xSemaphoreGive(networkFetchMutex); locked = false; }
  }
  ~NetworkFetchGuard() { release(); }
};



// =========================================================
// SPRITES
// =========================================================
TFT_eSprite sprClock = TFT_eSprite(&tft);
TFT_eSprite sprSmall = TFT_eSprite(&tft);

// =========================================================
// LOCATION
// =========================================================
float LAT = 33.4706f;
float LNG = -82.5009f;
String locationName = "Thomson";

// =========================================================
// THEME
// =========================================================
uint16_t COL_BG        = 0x08A3;
uint16_t COL_PANEL     = 0x1106;
uint16_t COL_PANEL_ALT = 0x18C7;
uint16_t COL_STROKE    = 0x31EC;
uint16_t COL_TEXT      = 0xEF7D;
uint16_t COL_DIM       = 0x94B2;
uint16_t COL_ACCENT    = 0x5EFA;

const uint16_t COL_GREEN  = TFT_GREEN;
const uint16_t COL_YELLOW = 0xFFE0;
const uint16_t COL_RED    = TFT_RED;
const uint16_t COL_BLUE   = 0x041F;

String textColorKey = "standard";
String unitKey = "imperial"; // metric = C/mm, imperial = F/in
String regionFormatKey = "europe"; // europe = 24h + dd.mm.yyyy, us = 12h + mm/dd/yyyy
String timezoneKey = "us_eastern";

// =========================================================
// LAYOUT
// =========================================================
const int SCREEN_W = 240;
const int SCREEN_H = 320;
const int TOPBAR_H = 34;
const int NAV_H    = 44;

const int HOME_GRID_Y1 = 120;
const int HOME_GRID_Y2 = 198;
const int HOME_WIDGET_H = 70;

const int HOME_TIMER_X = 124;
const int HOME_TIMER_Y = HOME_GRID_Y1;
const int HOME_TIMER_W = 108;
const int HOME_TIMER_H = HOME_WIDGET_H;
const int TIMER_MENU_X = 20;
const int TIMER_MENU_Y = 68;
const int TIMER_MENU_W = 200;
const int TIMER_MENU_H = 194;
const int TIMER_DONE_X = 26;
const int TIMER_DONE_Y = 92;
const int TIMER_DONE_W = 188;
const int TIMER_DONE_H = 108;
const int PAGE_ROW1_Y = 42;
const int PAGE_ROW2_Y = 120;
const int PAGE_ROW3_Y = 198;
const int PAGE_WIDGET_H = HOME_WIDGET_H;

// =========================================================
// NOTES
// =========================================================
String notesText = "No notes yet.";
bool notesDirty = true;
String buddyNickname = "";

enum HomeWidgetType {
  HOME_WIDGET_WEEK = 0,
  HOME_WIDGET_TIMER,
  HOME_WIDGET_RAIN,
  HOME_WIDGET_OUTDOOR,
  HOME_WIDGET_KP,
  HOME_WIDGET_PRESSURE,
  HOME_WIDGET_WIND,
  HOME_WIDGET_SUN
};

const int HOME_SLOT_COUNT = 4;
HomeWidgetType homeWidgetSlots[HOME_SLOT_COUNT] = {
  HOME_WIDGET_WEEK,
  HOME_WIDGET_TIMER,
  HOME_WIDGET_RAIN,
  HOME_WIDGET_OUTDOOR
};

String cacheHomeSlots[HOME_SLOT_COUNT];

// =========================================================
// STATE
// =========================================================
enum Page {
  PAGE_HOME = 0,
  PAGE_STOCKS = 1,
  PAGE_RADAR = 2,
  PAGE_STATUS = 3,
  PAGE_WEATHER_RADAR = 4
};

Page currentPage = PAGE_HOME;
Page lastDrawnPage = (Page)-1;

unsigned long lastClockTick = 0;
unsigned long lastDataTick  = 0;

const unsigned long CLOCK_TICK_MS = 1000;
const unsigned long DATA_TICK_MS  = 30UL * 1000UL;

bool pageDirty = true;
bool dataDirty = true;

// cache
String cacheClock = "";
String cacheTemp = "";
String cacheRain = "";
String cacheWeek = "";
String cacheHomeEmpty1 = "";
String cacheHomeEmpty2 = "";
String cacheFocusTimer = "";
String cacheTimerMenu = "";
String cacheTimerDone = "";
String cacheTimerDoneCountdown = "";
String cacheTimerDoneFlash = "";

String lastWifiText = "";
String lastSignalText = "";
String lastIpText = "";
String lastUptimeText = "";
String lastTempText = "";
String lastRainText = "";
String lastKpText = "";
String lastKpLevelText = "";
String lastWindText = "";
String lastWindDirText = "";
String lastNextSunLabel = "";
String lastNextSunTime = "";
String lastNotesText = "";
String lastNetworkToggleText = "";

const char* homeWidgetKey(HomeWidgetType type) {
  switch (type) {
    case HOME_WIDGET_WEEK:    return "week";
    case HOME_WIDGET_TIMER:   return "timer";
    case HOME_WIDGET_RAIN:    return "rain";
    case HOME_WIDGET_OUTDOOR: return "outdoor";
    case HOME_WIDGET_KP:      return "kp";
    case HOME_WIDGET_PRESSURE:      return "pressure";
    case HOME_WIDGET_WIND:    return "wind";
    case HOME_WIDGET_SUN:     return "sun";
    default:                  return "week";
  }
}

const char* homeWidgetLabel(HomeWidgetType type) {
  switch (type) {
    case HOME_WIDGET_WEEK:    return "Week";
    case HOME_WIDGET_TIMER:   return "Timer";
    case HOME_WIDGET_RAIN:    return "Rain";
    case HOME_WIDGET_OUTDOOR: return "Outdoor";
    case HOME_WIDGET_KP:      return "KP index";
    case HOME_WIDGET_PRESSURE:      return "Pressure";
    case HOME_WIDGET_WIND:    return "Wind";
    case HOME_WIDGET_SUN:     return "Sun event";
    default:                  return "Week";
  }
}

HomeWidgetType homeWidgetFromKey(const String& key) {
  if (key == "week") return HOME_WIDGET_WEEK;
  if (key == "timer") return HOME_WIDGET_TIMER;
  if (key == "rain") return HOME_WIDGET_RAIN;
  if (key == "outdoor") return HOME_WIDGET_OUTDOOR;
  if (key == "kp") return HOME_WIDGET_KP;
  if (key == "pressure" || key == "uv") return HOME_WIDGET_PRESSURE; // Legacy UV slots migrate in RAM.
  if (key == "wind") return HOME_WIDGET_WIND;
  if (key == "sun") return HOME_WIDGET_SUN;
  return HOME_WIDGET_WEEK;
}

const char* homeSlotLabel(int slot) {
  switch (slot) {
    case 0: return "Top left";
    case 1: return "Top right";
    case 2: return "Bottom left";
    case 3: return "Bottom right";
    default: return "Slot";
  }
}

const char* timezonePosixByKey(const String& key) {
  if (key == "utc") return "UTC0";
  if (key == "atlantic_azores") return "AZOT1AZOST,M3.5.0/0,M10.5.0/1";
  if (key == "europe_west") return "WET0WEST,M3.5.0/1,M10.5.0";
  if (key == "uk") return "GMT0BST,M3.5.0/1,M10.5.0";
  if (key == "europe_central") return "CET-1CEST,M3.5.0/2,M10.5.0/3";
  if (key == "europe_east") return "EET-2EEST,M3.5.0/3,M10.5.0/4";
  if (key == "africa_south") return "SAST-2";
  if (key == "middle_east_gulf") return "GST-4";
  if (key == "india") return "IST-5:30";
  if (key == "thailand") return "ICT-7";
  if (key == "china") return "CST-8";
  if (key == "us_eastern") return "EST5EDT,M3.2.0/2,M11.1.0/2";
  if (key == "us_central") return "CST6CDT,M3.2.0/2,M11.1.0/2";
  if (key == "us_mountain") return "MST7MDT,M3.2.0/2,M11.1.0/2";
  if (key == "us_arizona") return "MST7";
  if (key == "us_pacific") return "PST8PDT,M3.2.0/2,M11.1.0/2";
  if (key == "alaska") return "AKST9AKDT,M3.2.0/2,M11.1.0/2";
  if (key == "hawaii") return "HST10";
  if (key == "canada_atlantic") return "AST4ADT,M3.2.0/2,M11.1.0/2";
  if (key == "brazil_east") return "BRT3";
  if (key == "argentina") return "ART3";
  if (key == "asia_tokyo") return "JST-9";
  if (key == "korea") return "KST-9";
  if (key == "australia_perth") return "AWST-8";
  if (key == "australia_darwin") return "ACST-9:30";
  if (key == "australia_sydney") return "AEST-10AEDT,M10.1.0,M4.1.0/3";
  if (key == "new_zealand") return "NZST-12NZDT,M9.5.0/2,M4.1.0/3";
  return "CET-1CEST,M3.5.0/2,M10.5.0/3";
}

const char* timezoneLabelByKey(const String& key) {
  if (key == "utc") return "UTC";
  if (key == "atlantic_azores") return "Azores";
  if (key == "europe_west") return "Western Europe";
  if (key == "uk") return "United Kingdom";
  if (key == "europe_central") return "Central Europe";
  if (key == "europe_east") return "Eastern Europe";
  if (key == "africa_south") return "South Africa";
  if (key == "middle_east_gulf") return "Gulf / UAE";
  if (key == "india") return "India";
  if (key == "thailand") return "Thailand / ICT";
  if (key == "china") return "China";
  if (key == "us_eastern") return "US Eastern";
  if (key == "us_central") return "US Central";
  if (key == "us_mountain") return "US Mountain";
  if (key == "us_arizona") return "US Arizona";
  if (key == "us_pacific") return "US Pacific";
  if (key == "alaska") return "Alaska";
  if (key == "hawaii") return "Hawaii";
  if (key == "canada_atlantic") return "Canada Atlantic";
  if (key == "brazil_east") return "Brazil East";
  if (key == "argentina") return "Argentina";
  if (key == "asia_tokyo") return "Japan";
  if (key == "korea") return "South Korea";
  if (key == "australia_perth") return "Australia West";
  if (key == "australia_darwin") return "Australia Central";
  if (key == "australia_sydney") return "Australia East";
  if (key == "new_zealand") return "New Zealand";
  return "Central Europe";
}

String sanitizeTimezoneKey(const String& key) {
  const char* supported[] = {
    "utc", "atlantic_azores", "europe_west", "uk", "europe_central", "europe_east",
    "africa_south", "middle_east_gulf", "india", "thailand", "china",
    "us_eastern", "us_central", "us_mountain", "us_arizona", "us_pacific",
    "alaska", "hawaii", "canada_atlantic", "brazil_east", "argentina",
    "asia_tokyo", "korea", "australia_perth", "australia_darwin",
    "australia_sydney", "new_zealand"
  };
  for (const char* supportedKey : supported) {
    if (key == supportedKey) return key;
  }
  return "europe_central";
}

void applyDeviceTimezoneByKey(const String& key) {
  timezoneKey = sanitizeTimezoneKey(key);
  setenv("TZ", timezonePosixByKey(timezoneKey), 1);
  tzset();
}

void appendTimezoneOptions(String& page, const String& selectedKey) {
  struct TimezoneGroup {
    const char* label;
    const char* keys[8];
    int count;
  };

  const TimezoneGroup groups[] = {
    {"Global", {"utc"}, 1},
    {"Europe", {"atlantic_azores", "europe_west", "uk", "europe_central", "europe_east"}, 5},
    {"Africa & Middle East", {"africa_south", "middle_east_gulf"}, 2},
    {"Asia", {"india", "thailand", "china", "asia_tokyo", "korea"}, 5},
    {"North America", {"us_eastern", "us_central", "us_mountain", "us_arizona", "us_pacific", "alaska", "hawaii", "canada_atlantic"}, 8},
    {"South America", {"brazil_east", "argentina"}, 2},
    {"Australia & Oceania", {"australia_perth", "australia_darwin", "australia_sydney", "new_zealand"}, 4}
  };

  for (const TimezoneGroup& group : groups) {
    page += "<optgroup label='";
    page += group.label;
    page += "'>";
    for (int i = 0; i < group.count; i++) {
      const char* key = group.keys[i];
      page += "<option value='";
      page += key;
      page += "'";
      if (selectedKey == key) page += " selected";
      page += ">";
      page += timezoneLabelByKey(key);
      page += "</option>";
    }
    page += "</optgroup>";
  }
}

void getHomeSlotRect(int slot, int& x, int& y, int& w, int& h) {
  const int xs[HOME_SLOT_COUNT] = {8, 124, 8, 124};
  const int ys[HOME_SLOT_COUNT] = {HOME_GRID_Y1, HOME_GRID_Y1, HOME_GRID_Y2, HOME_GRID_Y2};
  x = xs[slot];
  y = ys[slot];
  w = 108;
  h = HOME_WIDGET_H;
}

void appendHomeWidgetOptions(String& page, const String& selectedKey) {
  const HomeWidgetType types[] = {
    HOME_WIDGET_WEEK,
    HOME_WIDGET_TIMER,
    HOME_WIDGET_RAIN,
    HOME_WIDGET_OUTDOOR,
    HOME_WIDGET_KP,
    HOME_WIDGET_PRESSURE,
    HOME_WIDGET_WIND,
    HOME_WIDGET_SUN
  };

  for (HomeWidgetType type : types) {
    const char* key = homeWidgetKey(type);
    page += "<option value='";
    page += key;
    page += "'";
    if (selectedKey == key) page += " selected";
    page += ">";
    page += homeWidgetLabel(type);
    page += "</option>";
  }
}

void clearHomeSlotCaches() {
  for (int i = 0; i < HOME_SLOT_COUNT; i++) {
    cacheHomeSlots[i] = "";
  }
}

// Focus timer
bool focusMenuOpen = false;
bool focusTimerRunning = false;
bool focusTimerFinished = false;
unsigned long focusEndMs = 0;
unsigned long focusDurationSec = 0;
unsigned long focusRemainingSec = 0;
bool timerDoneDialogOpen = false;
unsigned long timerDoneDialogStartedMs = 0;
const unsigned long TIMER_DONE_DIALOG_MS = 60UL * 1000UL;
bool flashModeEnabled = false;
int timerPresetMin[6] = {1, 5, 10, 15, 25, 30};
String savedWifiSsid, savedWifiPass;
String pendingWifiSsid, pendingWifiPass;
bool wifiSetupOpen = false;
bool wifiScanning = false;
bool wifiSetupConnecting = false;
bool wifiPasswordEntry = false;
bool wifiKeyboardShift = false;
bool wifiKeyboardSymbols = false;
bool wifiSetupDirty = true;
int wifiNetworkOffset = 0;
String wifiSetupMessage;
void openWifiSetup();
void drawWifiSetup();
void handleWifiSetupTouch(int x, int y);
bool wifiEnabled = true;
bool wifiConnectInProgress = false;
unsigned long wifiConnectStartedMs = 0;
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000UL;

// Weather
static float tempC = NAN;
static float tempMinC = NAN;
static float tempMaxC = NAN;
static float precipMm = NAN;
static float windSpeedMs = NAN;
static float windDirectionDeg = NAN;
static float pressureHpa = NAN;
static float pressureDeltaHpa = NAN;
static time_t lastWeatherFetch = 0;
static const uint32_t WEATHER_INTERVAL_SEC = 10 * 60;

// KP-index
static float kpIndex = NAN;
static time_t lastKpFetch = 0;
static const uint32_t KP_INTERVAL_SEC = 10 * 60;

// Sunrise / Sunset
static int sunriseMin = -1;
static int sunsetMin  = -1;
static int lastSunYmd = -1;
static time_t lastSyncTime = 0;

// =========================================================
// SLEEP / BACKLIGHT
// =========================================================
const int BACKLIGHT_PIN = 21;

bool sleepDimmed = false;
bool sleepOff = false;
bool manualDimMode = false;

unsigned long lastInteractionMs = 0;

int sleepIntervalMin = 10;
int sleepOffDelaySec = 60;

const int BL_FULL = 255;
const int BL_DIM  = 18;
const int BL_OFF  = 0;
const int FLASH_BL_LOW = 20;
const int FLASH_BL_HIGH = 255;

void wakeDisplay(bool clearManualMode = true);

int sanitizeTimerMinutes(int value);

// =========================================================
// HELPERS
// =========================================================
static int ymdFromLocal(time_t t) {
  struct tm tmLocal;
  localtime_r(&t, &tmLocal);
  return (tmLocal.tm_year + 1900) * 10000 + (tmLocal.tm_mon + 1) * 100 + tmLocal.tm_mday;
}

static int minutesFromLocalEpoch(time_t t) {
  struct tm tmLocal;
  localtime_r(&t, &tmLocal);
  return tmLocal.tm_hour * 60 + tmLocal.tm_min;
}

static int minutesNowLocal() {
  time_t now = time(nullptr);
  struct tm tmNow;
  localtime_r(&now, &tmNow);
  return tmNow.tm_hour * 60 + tmNow.tm_min;
}

static String wifiStatusText() {
  if (!wifiEnabled) return "Disabled";
  return WiFi.status() == WL_CONNECTED ? "Online" : "Offline";
}

static String signalText() {
  if (!wifiEnabled || WiFi.status() != WL_CONNECTED) return "-- dBm";
  return String(WiFi.RSSI()) + " dBm";
}

static String ipText() {
  if (!wifiEnabled || WiFi.status() != WL_CONNECTED) return "-";
  return WiFi.localIP().toString();
}

static bool useUsRegionFormat() {
  return regionFormatKey == "us";
}

static String formatClockParts(const struct tm& tmValue, bool withSeconds) {
  char buf[20];
  const char* pattern = useUsRegionFormat()
    ? (withSeconds ? "%I:%M:%S %p" : "%I:%M %p")
    : (withSeconds ? "%H:%M:%S" : "%H:%M");
  strftime(buf, sizeof(buf), pattern, &tmValue);
  return String(buf);
}

static String formatDateParts(const struct tm& tmValue) {
  char buf[32];
  strftime(buf, sizeof(buf), useUsRegionFormat() ? "%a %m/%d/%Y" : "%a %d.%m.%Y", &tmValue);
  return String(buf);
}

static String formatMinuteOfDay(int minOfDay) {
  if (minOfDay < 0) return "--:--";
  if (useUsRegionFormat()) {
    int hour24 = minOfDay / 60;
    int minute = minOfDay % 60;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;
    char buf[12];
    snprintf(buf, sizeof(buf), "%d:%02d %s", hour12, minute, hour24 >= 12 ? "PM" : "AM");
    return String(buf);
  }
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", minOfDay / 60, minOfDay % 60);
  return String(buf);
}

static String tempText() {
  if (isnan(tempC)) return unitKey == "imperial" ? "--.-F" : "--.-C";

  if (unitKey == "imperial") {
    float f = tempC * 9.0f / 5.0f + 32.0f;
    return String(f, 1) + "F";
  }

  return String(tempC, 1) + "C";
}

static String formatDisplayTemp(float value) {
  if (isnan(value)) return "--";

  if (unitKey == "imperial") {
    float f = value * 9.0f / 5.0f + 32.0f;
    return String((int)roundf(f)) + "F";
  }

  return String((int)roundf(value)) + "C";
}

static String tempRangeText() {
  return "H:" + formatDisplayTemp(tempMaxC) + "  L:" + formatDisplayTemp(tempMinC);
}

static String rainText() {
  if (isnan(precipMm)) return unitKey == "imperial" ? "--.--in" : "--.-mm";

  if (unitKey == "imperial") {
    float inches = precipMm / 25.4f;
    return String(inches, 2) + "in";
  }

  return String(precipMm, 1) + "mm";
}

static String windText() {
  if (isnan(windSpeedMs)) return unitKey == "imperial" ? "--.-mph" : "--.-m/s";

  if (unitKey == "imperial") {
    float mph = windSpeedMs * 2.236936f;
    return String(mph, 1) + "mph";
  }

  return String(windSpeedMs, 1) + "m/s";
}

static String windDirectionText() {
  if (isnan(windDirectionDeg)) return "--";

  const char* dirs[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  int idx = (int)roundf(windDirectionDeg / 45.0f) % 8;
  return String(dirs[idx]);
}

static String kpText() {
  return isnan(kpIndex) ? "Kp --" : "Kp " + String(kpIndex, 1);
}

static String kpLevelText() {
  if (isnan(kpIndex)) return "--";
  if (kpIndex < 3.0f) return "Low";
  if (kpIndex < 5.0f) return "Medium";
  if (kpIndex < 7.0f) return "High";
  return "Extreme";
}

static String pressureValueText() {
  if (!isfinite(pressureHpa)) return "--";
  return unitKey == "imperial" ? String(pressureHpa / 33.8638866667f, 2) : String(pressureHpa, 0);
}
static const char *pressureUnitText() { return unitKey == "imperial" ? "inHg" : "hPa"; }
static int pressureDirection() {
  if (!isfinite(pressureDeltaHpa)) return 2;
  if (pressureDeltaHpa > 0.5f) return 1;
  if (pressureDeltaHpa < -0.5f) return -1;
  return 0;
}
static const char *pressureTrendText() {
  int direction = pressureDirection();
  return direction == 1 ? "rising" : direction == -1 ? "falling" : direction == 0 ? "steady" : "--";
}

static uint16_t statusColor() {
  if (textColorKey != "standard") return COL_TEXT;
  if (!wifiEnabled) return COL_YELLOW;
  return WiFi.status() == WL_CONNECTED ? COL_GREEN : COL_RED;
}

static String uptimeText() {
  unsigned long seconds = millis() / 1000UL;
  unsigned long days = seconds / 86400UL;
  seconds %= 86400UL;
  unsigned long hours = seconds / 3600UL;
  seconds %= 3600UL;
  unsigned long minutes = seconds / 60UL;

  if (days > 0) return String(days) + "d " + String(hours) + "h";
  if (hours > 0) return String(hours) + "h " + String(minutes) + "m";
  return String(minutes) + "m";
}

static String nextSunLabel() {
  int nowMin = minutesNowLocal();
  if (sunriseMin < 0 || sunsetMin < 0) return "Sun";
  if (nowMin < sunriseMin) return "Sunrise";
  if (nowMin < sunsetMin) return "Sunset";
  return "Sunrise";
}

static String nextSunTimeText() {
  int nowMin = minutesNowLocal();
  if (sunriseMin < 0 || sunsetMin < 0) return "--:--";
  if (nowMin < sunriseMin) return formatMinuteOfDay(sunriseMin);
  if (nowMin < sunsetMin) return formatMinuteOfDay(sunsetMin);
  return formatMinuteOfDay(sunriseMin);
}

static String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length());
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else if (c == '"') out += "&quot;";
    else out += c;
  }
  return out;
}

static String cssColorFrom565(uint16_t color) {
  uint8_t r = ((color >> 11) & 0x1F) * 255 / 31;
  uint8_t g = ((color >> 5) & 0x3F) * 255 / 63;
  uint8_t b = (color & 0x1F) * 255 / 31;
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
  return String(buf);
}

static String accentPreviewCss(const String& key) {
  if (key == "standard") return cssColorFrom565(0xEF7D);
  if (key == "ice")      return cssColorFrom565(0xEFFF);
  if (key == "white")    return cssColorFrom565(TFT_WHITE);
  if (key == "cyan")     return cssColorFrom565(0x5EFA);
  if (key == "mint")     return cssColorFrom565(0x07F0);
  if (key == "green")    return cssColorFrom565(TFT_GREEN);
  if (key == "blue")     return cssColorFrom565(0x3D9F);
  if (key == "purple")   return cssColorFrom565(0xA2F5);
  if (key == "pink")     return cssColorFrom565(0xF97F);
  if (key == "orange")   return cssColorFrom565(0xFD20);
  if (key == "amber")    return cssColorFrom565(0xFEA0);
  if (key == "red")      return cssColorFrom565(TFT_RED);
  return cssColorFrom565(0xEF7D);
}

static String themePreviewCss(const String& key) {
  if (key == "slate")    return cssColorFrom565(0x08A3);
  if (key == "deep")     return cssColorFrom565(0x0000);
  if (key == "nordic")   return cssColorFrom565(0x0864);
  if (key == "forest")   return cssColorFrom565(0x0208);
  if (key == "coffee")   return cssColorFrom565(0x18A3);
  if (key == "soft")     return cssColorFrom565(0x10A2);
  if (key == "midnight") return cssColorFrom565(0x0008);
  if (key == "graphite") return cssColorFrom565(0x1082);
  if (key == "garnet")   return cssColorFrom565(0x1004);
  if (key == "ochre")    return cssColorFrom565(0x20E1);
  return cssColorFrom565(0x08A3);
}

static String formatTimerClock(unsigned long totalSec) {
  unsigned long minutes = totalSec / 60UL;
  unsigned long seconds = totalSec % 60UL;

  char buf[10];
  snprintf(buf, sizeof(buf), "%02lu:%02lu", minutes, seconds);
  return String(buf);
}

static String focusHintText() {
  if (focusTimerFinished) return "Tap to reset";
  if (focusTimerRunning) return String((focusDurationSec / 60UL)) + " min session";
  return "Tap to start";
}

static String formatElapsedText(unsigned long totalSec) {
  unsigned long minutes = totalSec / 60UL;
  if (minutes == 0) return "< 1 minute elapsed";
  if (minutes == 1) return "1 minute elapsed";
  return String(minutes) + " minutes elapsed";
}

static String lastSyncText() {
  if (lastSyncTime <= 0) return "Sync --:--";

  struct tm tmSync;
  localtime_r(&lastSyncTime, &tmSync);
  return "Sync " + formatClockParts(tmSync, false);
}

static String weekNumberText() {
  time_t now = time(nullptr);
  struct tm tmNow;
  localtime_r(&now, &tmNow);
  char buf[4];
  strftime(buf, sizeof(buf), "%V", &tmNow);
  return String(buf);
}

static String timerDoneCountdownText() {
  if (!timerDoneDialogOpen) return "";

  unsigned long elapsedMs = millis() - timerDoneDialogStartedMs;
  unsigned long remainingMs = (elapsedMs >= TIMER_DONE_DIALOG_MS) ? 0 : (TIMER_DONE_DIALOG_MS - elapsedMs);
  unsigned long remainingSec = (remainingMs + 999UL) / 1000UL;
  return String("Auto close in ") + String(remainingSec) + "s";
}

static String homeTitleText() {
  return buddyNickname.length() > 0 ? buddyNickname : "Deskbuddy";
}

int sanitizeTimerMinutes(int value) {
  return constrain(value, 1, 180);
}

void resetFocusTimer() {
  focusTimerRunning = false;
  focusTimerFinished = false;
  focusMenuOpen = false;
  timerDoneDialogOpen = false;
  focusEndMs = 0;
  focusDurationSec = 0;
  focusRemainingSec = 0;
  cacheFocusTimer = "";
  clearHomeSlotCaches();
  cacheTimerMenu = "";
  cacheTimerDone = "";
  cacheTimerDoneCountdown = "";
  cacheTimerDoneFlash = "";
}

void startFocusTimer(unsigned long minutes) {
  focusDurationSec = minutes * 60UL;
  focusRemainingSec = focusDurationSec;
  focusEndMs = millis() + (focusDurationSec * 1000UL);
  focusTimerRunning = true;
  focusTimerFinished = false;
  focusMenuOpen = false;
  timerDoneDialogOpen = false;
  cacheFocusTimer = "";
  clearHomeSlotCaches();
  cacheTimerMenu = "";
  cacheTimerDone = "";
  cacheTimerDoneCountdown = "";
  cacheTimerDoneFlash = "";
}

void dismissTimerDoneDialog() {
  if (!timerDoneDialogOpen) return;
  timerDoneDialogOpen = false;
  timerDoneDialogStartedMs = 0;
  cacheTimerDone = "";
  cacheTimerDoneCountdown = "";
  cacheTimerDoneFlash = "";
  if (!sleepDimmed && !sleepOff) setBacklight(BL_FULL);
  pageDirty = true;
}

void openTimerDoneDialog() {
  timerDoneDialogOpen = true;
  timerDoneDialogStartedMs = millis();
  cacheTimerDone = "";
  cacheTimerDoneCountdown = "";
  wakeDisplay();
}

void updateFocusTimerState() {
  if (!focusTimerRunning) return;

  unsigned long now = millis();
  if ((long)(focusEndMs - now) <= 0) {
    focusRemainingSec = 0;
    focusTimerRunning = false;
    focusTimerFinished = true;
    focusMenuOpen = false;
    cacheFocusTimer = "";
    clearHomeSlotCaches();
    cacheTimerMenu = "";
    openTimerDoneDialog();
    return;
  }

  unsigned long remainingMs = focusEndMs - now;
  unsigned long nextRemainingSec = (remainingMs + 999UL) / 1000UL;
  if (nextRemainingSec != focusRemainingSec) {
    focusRemainingSec = nextRemainingSec;
    cacheFocusTimer = "";
    clearHomeSlotCaches();
  }
}

void updateTimerDoneDialogState() {
  if (!timerDoneDialogOpen) return;
  if (millis() - timerDoneDialogStartedMs >= TIMER_DONE_DIALOG_MS) {
    dismissTimerDoneDialog();
  }
}

static bool isWeekdayKeepAwakeTime() {
  const time_t now = time(nullptr);
  if (now < 1700000000) return false; // Same validity check as NTP startup.

  struct tm local;
  if (!localtime_r(&now, &local)) return false;
  // tm_wday is 0 on Sunday and 6 on Saturday.
  return local.tm_wday >= 1 && local.tm_wday <= 5 &&
         local.tm_hour >= 8 && local.tm_hour < 17;
}

void setBacklight(int value) {
  if (value < BL_FULL && isWeekdayKeepAwakeTime()) value = BL_FULL;
  value = constrain(value, 0, 255);
  analogWrite(BACKLIGHT_PIN, value);
}

void wakeDisplay(bool clearManualMode) {
  sleepDimmed = false;
  sleepOff = false;
  if (clearManualMode) manualDimMode = false;
  lastInteractionMs = millis();
  setBacklight(BL_FULL);
  pageDirty = true;
}

void enterSleepDim() {
  if (sleepDimmed || sleepOff || manualDimMode) return;
  sleepDimmed = true;
  setBacklight(BL_DIM);
}

void enterSleepOff() {
  if (sleepOff) return;
  sleepOff = true;
  sleepDimmed = true;
  setBacklight(BL_OFF);
  pageDirty = true;
}

void toggleSleepMode() {
  if (isWeekdayKeepAwakeTime()) return;

  if (manualDimMode) {
    wakeDisplay();
    return;
  }

  manualDimMode = true;
  sleepDimmed = true;
  sleepOff = false;
  setBacklight(BL_DIM);
  pageDirty = true;
}

void handleAutoSleep() {
  if (isWeekdayKeepAwakeTime()) {
    if (sleepOff || sleepDimmed) wakeDisplay();
    // Give the normal idle timeout a fresh start when the window ends.
    lastInteractionMs = millis();
    return;
  }

  if (focusMenuOpen || timerDoneDialogOpen) return;
  if (sleepIntervalMin <= 0) return;

  unsigned long now = millis();
  unsigned long dimAfterMs = (unsigned long)sleepIntervalMin * 60UL * 1000UL;
  unsigned long offAfterMs = dimAfterMs + ((unsigned long)sleepOffDelaySec * 1000UL);

  if (!sleepDimmed && !sleepOff && now - lastInteractionMs > dimAfterMs) {
    enterSleepDim();
  }

  if (sleepDimmed && !sleepOff && now - lastInteractionMs > offAfterMs) {
    enterSleepOff();
  }
}

// =========================================================
// THEME / SETTINGS
// =========================================================
void applyThemeByKey(const String& accentKey, const String& bgKey) {
  if (accentKey == "standard")    COL_ACCENT = 0xEF7D;
  else if (accentKey == "cyan")   COL_ACCENT = 0x5EFA;
  else if (accentKey == "ice")    COL_ACCENT = 0xEFFF;
  else if (accentKey == "white")  COL_ACCENT = TFT_WHITE;
  else if (accentKey == "mint")   COL_ACCENT = 0x07F0;
  else if (accentKey == "green")  COL_ACCENT = TFT_GREEN;
  else if (accentKey == "blue")   COL_ACCENT = 0x3D9F;
  else if (accentKey == "purple") COL_ACCENT = 0xA2F5;
  else if (accentKey == "pink")   COL_ACCENT = 0xF97F;
  else if (accentKey == "orange") COL_ACCENT = 0xFD20;
  else if (accentKey == "amber")  COL_ACCENT = 0xFEA0;
  else if (accentKey == "red")    COL_ACCENT = TFT_RED;
  else                            COL_ACCENT = 0x5EFA;

  if (bgKey == "slate") {
    COL_BG = 0x08A3; COL_PANEL = 0x1106; COL_PANEL_ALT = 0x18C7; COL_STROKE = 0x31EC;
  } else if (bgKey == "deep") {
    COL_BG = 0x0000; COL_PANEL = 0x0841; COL_PANEL_ALT = 0x1082; COL_STROKE = 0x2945;
  } else if (bgKey == "nordic") {
    COL_BG = 0x0864; COL_PANEL = 0x10C6; COL_PANEL_ALT = 0x1908; COL_STROKE = 0x3A2D;
  } else if (bgKey == "forest") {
    COL_BG = 0x0208; COL_PANEL = 0x0ACB; COL_PANEL_ALT = 0x134D; COL_STROKE = 0x2D72;
  } else if (bgKey == "coffee") {
    COL_BG = 0x18A3; COL_PANEL = 0x2945; COL_PANEL_ALT = 0x39C7; COL_STROKE = 0x5A89;
  } else if (bgKey == "soft") {
    COL_BG = 0x10A2; COL_PANEL = 0x1924; COL_PANEL_ALT = 0x2145; COL_STROKE = 0x3A49;
  } else if (bgKey == "midnight") {
    COL_BG = 0x0008; COL_PANEL = 0x0011; COL_PANEL_ALT = 0x0018; COL_STROKE = 0x3A7F;
  } else if (bgKey == "graphite") {
    COL_BG = 0x1082; COL_PANEL = 0x18C3; COL_PANEL_ALT = 0x2104; COL_STROKE = 0x4208;
  } else if (bgKey == "garnet") {
    COL_BG = 0x1004; COL_PANEL = 0x1886; COL_PANEL_ALT = 0x20E8; COL_STROKE = 0x41AC;
  } else if (bgKey == "ochre") {
    COL_BG = 0x20E1; COL_PANEL = 0x3184; COL_PANEL_ALT = 0x4226; COL_STROKE = 0x632B;
  } else {
    COL_BG = 0x08A3; COL_PANEL = 0x1106; COL_PANEL_ALT = 0x18C7; COL_STROKE = 0x31EC;
  }
}

void applyTextColorByKey(const String& key) {
  textColorKey = key;

  if (key == "standard") {
    COL_TEXT = 0xEF7D; COL_DIM  = 0x94B2;
  } else if (key == "white") {
    COL_TEXT = TFT_WHITE; COL_DIM = 0xBDF7;
  } else if (key == "ice") {
    COL_TEXT = 0xEFFF; COL_DIM = 0x9D7F;
  } else if (key == "mint") {
    COL_TEXT = 0x07F0; COL_DIM = 0x05EC;
  } else if (key == "orange") {
    COL_TEXT = 0xFD20; COL_DIM = 0xBA26;
  } else if (key == "amber") {
    COL_TEXT = 0xFEA0; COL_DIM = 0xBCE0;
  } else if (key == "green") {
    COL_TEXT = TFT_GREEN; COL_DIM = 0x86E8;
  } else if (key == "cyan") {
    COL_TEXT = 0x5EFA; COL_DIM = 0x3D96;
  } else if (key == "blue") {
    COL_TEXT = 0x3D9F; COL_DIM = 0x22B1;
  } else if (key == "purple") {
    COL_TEXT = 0xA2F5; COL_DIM = 0x79ED;
  } else if (key == "red") {
    COL_TEXT = TFT_RED; COL_DIM = 0xB9E7;
  } else if (key == "pink") {
    COL_TEXT = 0xF97F; COL_DIM = 0xC2F1;
  } else {
    COL_TEXT = 0xEF7D;
    COL_DIM  = 0x94B2;
    textColorKey = "standard";
  }
}

void loadStoredSettings() {
  prefs.begin("deskbuddy", false);
  if (prefs.getBytesLength("touchCal") == sizeof(touchCal)) {
    prefs.getBytes("touchCal", touchCal, sizeof(touchCal));
    touchCalValid = true;
    for (int i = 0; i < 6; ++i) {
      if (!isfinite(touchCal[i])) touchCalValid = false;
    }
    if (fabsf(touchCal[0] * touchCal[4] - touchCal[1] * touchCal[3]) < 0.00001f)
      touchCalValid = false;
  }

  String accent = prefs.getString("accent", "cyan");
  String bg     = prefs.getString("bg", "slate");
  String txt    = prefs.getString("text", "standard");

  notesText        = prefs.getString("notes", "No notes yet.");
  buddyNickname    = prefs.getString("nickname", "");
  // Use the configured Thomson defaults when this board has no saved location.
  // The offline radar basemap must share the radar's geographic center.
  locationName     = prefs.getString("locname", locationName);
  LAT              = prefs.getFloat("lat", LAT);
  LNG              = prefs.getFloat("lng", LNG);
  sleepIntervalMin = prefs.getInt("sleepMin", 10);
  unitKey          = prefs.getString("units", "metric");
  regionFormatKey  = prefs.getString("region", "europe");
  timezoneKey      = sanitizeTimezoneKey(prefs.getString("tz", "europe_central"));
  flashModeEnabled = prefs.getBool("flashMode", false);
  wifiEnabled      = prefs.getBool("wifiEnabled", true);
  savedWifiSsid = prefs.getString("wifiSsid", WIFI_SSID);
  savedWifiPass = prefs.getString("wifiPass", WIFI_PASS);

  for (int i = 0; i < HOME_SLOT_COUNT; i++) {
    String key = String("homeSlot") + String(i);
    homeWidgetSlots[i] = homeWidgetFromKey(prefs.getString(key.c_str(), homeWidgetKey(homeWidgetSlots[i])));
  }

  for (int i = 0; i < 6; i++) {
    String key = String("timer") + String(i);
    timerPresetMin[i] = sanitizeTimerMinutes(prefs.getInt(key.c_str(), timerPresetMin[i]));
  }

  if (unitKey != "metric" && unitKey != "imperial") unitKey = "metric";
  if (regionFormatKey != "europe" && regionFormatKey != "us") regionFormatKey = "europe";
  buddyNickname.trim();
  applyThemeByKey(accent, bg);
  applyTextColorByKey(txt);
  applyDeviceTimezoneByKey(timezoneKey);
}

void resetDataCaches() {
  tempC = NAN;
  precipMm = NAN;
  windSpeedMs = NAN;
  windDirectionDeg = NAN;
  kpIndex = NAN;
  sunriseMin = -1;
  sunsetMin = -1;
  lastSunYmd = -1;
  lastWeatherFetch = 0;
  lastKpFetch = 0;
  dataDirty = true;
  pageDirty = true;
}

// =========================================================
// TOUCH
// =========================================================
// Capture a steady press and require release, without relying on the old mapping.
bool captureCalibrationPoint(float& rawX, float& rawY) {
  unsigned long started = millis(), releasedSince = 0;
  while (millis() - started < 45000UL) {
    if (ts.touched()) releasedSince = 0;
    else {
      if (!releasedSince) releasedSince = millis();
      if (millis() - releasedSince >= 150) break;
    }
    delay(10);
  }
  if (millis() - started >= 45000UL) return false;
  int samples = 0, minX = 4095, maxX = 0, minY = 4095, maxY = 0;
  long sumX = 0, sumY = 0;
  while (millis() - started < 45000UL) {
    if (ts.touched()) {
      TS_Point p = ts.getPoint();
      if (p.z >= 80 && p.z <= 4000) {
        minX = min(minX, (int)p.x); maxX = max(maxX, (int)p.x);
        minY = min(minY, (int)p.y); maxY = max(maxY, (int)p.y);
        sumX += p.x; sumY += p.y; ++samples;
        if (samples >= 20) {
          if (maxX - minX <= 180 && maxY - minY <= 180) {
            rawX = sumX / (float)samples;
            rawY = sumY / (float)samples;
            releasedSince = 0;
            while (millis() - started < 45000UL) {
              if (ts.touched()) releasedSince = 0;
              else {
                if (!releasedSince) releasedSince = millis();
                if (millis() - releasedSince >= 150) return true;
              }
              delay(10);
            }
            return false;
          }
          samples = 0; sumX = sumY = 0;
          minX = minY = 4095; maxX = maxY = 0;
        }
      } else {
        samples = 0; sumX = sumY = 0;
        minX = minY = 4095; maxX = maxY = 0;
      }
    } else {
      samples = 0; sumX = sumY = 0;
      minX = minY = 4095; maxX = maxY = 0;
    }
    delay(10);
  }
  return false;
}

void runTouchCalibration() {
  wakeDisplay();
  const int targetX[5] = {20, SCREEN_W - 21, 20, SCREEN_W - 21, SCREEN_W / 2};
  const int targetY[5] = {50, 50, SCREEN_H - 51, SCREEN_H - 51, SCREEN_H / 2};
  for (int attempt = 0; attempt < 3; ++attempt) {
    float rawX[5], rawY[5];
    bool captured = true;
    for (int i = 0; i < 5; ++i) {
      tft.fillScreen(COL_BG);
      tft.setTextColor(COL_TEXT, COL_BG);
      tft.drawCentreString("Touch calibration", SCREEN_W / 2, 8, 2);
      tft.drawCentreString("Press the cross, hold briefly,", SCREEN_W / 2, 94, 1);
      tft.drawCentreString("then lift your finger.", SCREEN_W / 2, 108, 1);
      tft.drawCentreString(String(i + 1) + " / 5", SCREEN_W / 2, 128, 2);
      tft.drawCentreString("Use a stylus tip for accuracy.", SCREEN_W / 2, 294, 1);
      tft.drawCircle(targetX[i], targetY[i], 10, COL_ACCENT);
      tft.drawFastHLine(targetX[i] - 15, targetY[i], 31, COL_ACCENT);
      tft.drawFastVLine(targetX[i], targetY[i] - 15, 31, COL_ACCENT);
      if (!captureCalibrationPoint(rawX[i], rawY[i])) { captured = false; break; }
    }
    if (!captured) break;
    float ux = rawX[1] - rawX[0], uy = rawY[1] - rawY[0];
    float vx = rawX[2] - rawX[0], vy = rawY[2] - rawY[0];
    float det = ux * vy - uy * vx;
    float nextCal[6];
    bool accurate = fabsf(det) > 100000.0f;
    if (accurate) {
      float width = targetX[1] - targetX[0], height = targetY[2] - targetY[0];
      nextCal[0] = width * vy / det;
      nextCal[1] = -width * vx / det;
      nextCal[2] = targetX[0] - nextCal[0] * rawX[0] - nextCal[1] * rawY[0];
      nextCal[3] = -height * uy / det;
      nextCal[4] = height * ux / det;
      nextCal[5] = targetY[0] - nextCal[3] * rawX[0] - nextCal[4] * rawY[0];
      // Independent corner and center taps must agree within 14 pixels.
      for (int i = 3; i < 5; ++i) {
        float dx = nextCal[0] * rawX[i] + nextCal[1] * rawY[i] + nextCal[2] - targetX[i];
        float dy = nextCal[3] * rawX[i] + nextCal[4] * rawY[i] + nextCal[5] - targetY[i];
        if (!isfinite(dx) || !isfinite(dy) || dx * dx + dy * dy > 196) accurate = false;
      }
    }
    tft.fillScreen(COL_BG);
    tft.setTextColor(COL_TEXT, COL_BG);
    if (accurate) {
      if (prefs.putBytes("touchCal", nextCal, sizeof(nextCal)) == sizeof(nextCal)) {
        memcpy(touchCal, nextCal, sizeof(touchCal));
        touchCalValid = true;
        tft.drawCentreString("Calibration saved", SCREEN_W / 2, 140, 2);
        delay(1200);
        pageDirty = true;
        lastInteractionMs = millis();
        return;
      }
      tft.drawCentreString("Could not save calibration", SCREEN_W / 2, 140, 1);
      delay(1500);
      break;
    }
    tft.drawCentreString("Taps did not line up.", SCREEN_W / 2, 130, 2);
    tft.drawCentreString("Please try again.", SCREEN_W / 2, 155, 2);
    delay(1500);
  }
  // A timeout or failed validation retains the previous calibration.
  tft.fillScreen(COL_BG);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawCentreString("Calibration unchanged", SCREEN_W / 2, 140, 2);
  delay(1200);
  pageDirty = true;
  lastInteractionMs = millis();
}

bool readTouchXY(int& sx, int& sy) {
  if (!ts.touched()) return false;

  TS_Point p = ts.getPoint();
  if (p.z < 80 || p.z > 4000) return false;

  if (touchCalValid) {
    sx = constrain((int)lroundf(touchCal[0] * p.x + touchCal[1] * p.y + touchCal[2]), 0, SCREEN_W - 1);
    sy = constrain((int)lroundf(touchCal[3] * p.x + touchCal[4] * p.y + touchCal[5]), 0, SCREEN_H - 1);
    return true;
  }

  int x = map(p.x, TOUCH_X_MIN, TOUCH_X_MAX, 0, SCREEN_W - 1);
  int y = map(p.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, SCREEN_H - 1);

  x = constrain(x, 0, SCREEN_W - 1);
  y = constrain(y, 0, SCREEN_H - 1);

  if (TOUCH_SWAP_XY) { int tmp = x; x = y; y = tmp; }
  if (TOUCH_FLIP_X)  x = (SCREEN_W - 1) - x;
  if (TOUCH_FLIP_Y)  y = (SCREEN_H - 1) - y;

  sx = x;
  sy = y;
  return true;
}

bool touchNewPress(int& tx, int& ty) {
  static bool wasDown = false;
  static unsigned long lastPressMs = 0;

  bool down = false;
  int x = 0, y = 0;

  if (readTouchXY(x, y)) down = true;

  bool fire = false;
  unsigned long now = millis();

  if (down && !wasDown && (now - lastPressMs > 220)) {
    fire = true;
    lastPressMs = now;
    tx = x;
    ty = y;
  }

  wasDown = down;
  return fire;
}

// =========================================================
// API
// =========================================================
bool fetchSunriseSunset() {
  if (WiFi.status() != WL_CONNECTED) return false;

  // The display loop must never wait behind background HTTPS downloads.
  NetworkFetchGuard fetchGuard(0);
  if (!fetchGuard.locked) return false;

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(5);
  client.setTimeout(5000);

  String url = String("https://api.sunrise-sunset.org/json?lat=") + String(LAT, 4) +
               "&lng=" + String(LNG, 4) + "&formatted=0";

  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  if (!http.begin(client, url)) {
    setRadarFetchError("HTTP setup failed");
    return false;
  }

  int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  StaticJsonDocument<1024> doc;
  if (deserializeJson(doc, body)) return false;

  const char* sunriseStr = doc["results"]["sunrise"];
  const char* sunsetStr  = doc["results"]["sunset"];
  if (!sunriseStr || !sunsetStr) return false;

  auto parseIsoToEpochUTC = [](const char* iso) -> time_t {
    int Y, M, D, h, m, s;
    if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &s) != 6) return (time_t)-1;

    struct tm t{};
    t.tm_year = Y - 1900;
    t.tm_mon  = M - 1;
    t.tm_mday = D;
    t.tm_hour = h;
    t.tm_min  = m;
    t.tm_sec  = s;

    char* oldTz = getenv("TZ");
    String old = oldTz ? String(oldTz) : String("");

    setenv("TZ", "UTC0", 1);
    tzset();
    time_t epoch = mktime(&t);

    if (old.length()) setenv("TZ", old.c_str(), 1);
    else unsetenv("TZ");
    tzset();

    return epoch;
  };

  time_t srEpoch = parseIsoToEpochUTC(sunriseStr);
  time_t ssEpoch = parseIsoToEpochUTC(sunsetStr);
  if (srEpoch < 0 || ssEpoch < 0) return false;

  sunriseMin = minutesFromLocalEpoch(srEpoch);
  sunsetMin  = minutesFromLocalEpoch(ssEpoch);
  lastSunYmd = ymdFromLocal(time(nullptr));
  lastSyncTime = time(nullptr);
  return true;
}

void ensureSunTimesForToday() {
  time_t nowT = time(nullptr);
  int ymd = ymdFromLocal(nowT);

  if ((sunriseMin < 0 || sunsetMin < 0 || ymd != lastSunYmd) &&
      WiFi.status() == WL_CONNECTED) {
    if (fetchSunriseSunset()) dataDirty = true;
  }
}

bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) return false;

  // The display loop must never wait behind background HTTPS downloads.
  NetworkFetchGuard fetchGuard(0);
  if (!fetchGuard.locked) return false;

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(5);
  client.setTimeout(5000);

  String url = String("https://api.open-meteo.com/v1/forecast?latitude=") + String(LAT, 4) +
               "&longitude=" + String(LNG, 4) +
               "&current=temperature_2m,wind_speed_10m,wind_direction_10m,pressure_msl" +
               "&hourly=precipitation,pressure_msl&past_hours=3&forecast_hours=24" +
               "&daily=temperature_2m_max,temperature_2m_min" +
               "&forecast_days=1&timezone=auto&wind_speed_unit=ms";

  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  if (!http.begin(client, url)) {
    setRadarFetchError("HTTP setup failed");
    return false;
  }

  int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  StaticJsonDocument<4096> doc;
  if (deserializeJson(doc, body)) return false;

  tempC = doc["current"]["temperature_2m"] | NAN;
  windSpeedMs = doc["current"]["wind_speed_10m"] | NAN;
  windDirectionDeg = doc["current"]["wind_direction_10m"] | NAN;
  pressureHpa = doc["current"]["pressure_msl"] | NAN;
  if (String(doc["current_units"]["pressure_msl"] | "") != "hPa") pressureHpa = NAN;
  pressureDeltaHpa = NAN;
  tempMaxC = NAN;
  tempMinC = NAN;

  JsonArray maxTemps = doc["daily"]["temperature_2m_max"];
  JsonArray minTemps = doc["daily"]["temperature_2m_min"];
  if (maxTemps && !maxTemps.isNull() && maxTemps.size() > 0) tempMaxC = maxTemps[0] | NAN;
  if (minTemps && !minTemps.isNull() && minTemps.size() > 0) tempMinC = minTemps[0] | NAN;

  JsonArray times = doc["hourly"]["time"];
  JsonArray precs = doc["hourly"]["precipitation"];

  if (times && precs) {
    String observation = doc["current"]["time"] | "";
    String key = observation.length() >= 16 ? observation.substring(0, 13) + ":00" : "";

    int idx = -1;
    for (int i = 0; i < (int)times.size(); i++) {
      const char* t = times[i];
      if (t && key.length() && String(t).startsWith(key)) {
        idx = i;
        break;
      }
    }
    precipMm = idx >= 0 ? (precs[idx] | NAN) : NAN;
    JsonArray pressures = doc["hourly"]["pressure_msl"].as<JsonArray>();
    if (idx >= 3 && pressures.size() > size_t(idx) && isfinite(pressureHpa) &&
        String(doc["hourly_units"]["pressure_msl"] | "") == "hPa") {
      float before = pressures[idx - 3] | NAN, after = pressures[idx - 2] | NAN;
      float fraction = observation.substring(14, 16).toInt() / 60.0f;
      if (isfinite(before) && isfinite(after)) pressureDeltaHpa = pressureHpa - (before + (after - before) * fraction);
    }
  }

  lastWeatherFetch = time(nullptr);
  lastSyncTime = lastWeatherFetch;
  return true;
}

void ensureWeather() {
  time_t nowT = time(nullptr);
  if ((isnan(tempC) || isnan(tempMinC) || isnan(tempMaxC) || isnan(precipMm) || isnan(windSpeedMs) || isnan(windDirectionDeg) || isnan(pressureHpa) ||
       (nowT - lastWeatherFetch) > WEATHER_INTERVAL_SEC) &&
      WiFi.status() == WL_CONNECTED) {
    if (fetchWeather()) dataDirty = true;
  }
}

bool fetchKpIndex() {
  if (WiFi.status() != WL_CONNECTED) return false;

  // The display loop must never wait behind background HTTPS downloads.
  NetworkFetchGuard fetchGuard(0);
  if (!fetchGuard.locked) return false;

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(5);
  client.setTimeout(5000);

  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  if (!http.begin(client, "https://services.swpc.noaa.gov/products/noaa-planetary-k-index.json")) {
    return false;
  }

  int code = http.GET();
  if (code != 200) {
    http.end();
    return false;
  }

  String body = http.getString();
  http.end();

  int lastRow = body.lastIndexOf('[');
  if (lastRow < 0) return false;

  int firstComma = body.indexOf(',', lastRow);
  if (firstComma < 0) return false;

  int q1 = body.indexOf('"', firstComma);
  if (q1 < 0) return false;
  int q2 = body.indexOf('"', q1 + 1);
  if (q2 < 0) return false;

  String kpStrLocal = body.substring(q1 + 1, q2);
  kpIndex = kpStrLocal.toFloat();

  lastKpFetch = time(nullptr);
  lastSyncTime = lastKpFetch;
  return true;
}

void ensureKpIndex() {
  time_t nowT = time(nullptr);
  if ((isnan(kpIndex) || (nowT - lastKpFetch) > KP_INTERVAL_SEC) &&
      WiFi.status() == WL_CONNECTED) {
    if (fetchKpIndex()) dataDirty = true;
  }
}

// =========================================================
// DRAW HELPERS
// =========================================================
void drawCard(int x, int y, int w, int h, bool accent = false) {
  tft.fillRoundRect(x, y, w, h, 10, COL_PANEL);
  tft.drawRoundRect(x, y, w, h, 10, accent ? COL_ACCENT : COL_STROKE);
}

void drawTopBar(const String& title) {
  tft.fillRect(0, 0, SCREEN_W, TOPBAR_H, COL_PANEL_ALT);
  tft.drawFastHLine(0, TOPBAR_H - 1, SCREEN_W, COL_STROKE);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_TEXT, COL_PANEL_ALT);
  tft.drawString(title, 10, 9, 2);

  const int bs = 25;
  const int bx = SCREEN_W - bs - 6;
  const int by = 4;

  uint16_t bg = (sleepDimmed || sleepOff) ? COL_ACCENT : COL_PANEL;
  uint16_t fg = (sleepDimmed || sleepOff) ? TFT_BLACK : COL_TEXT;

  tft.fillRoundRect(bx, by, bs, bs, 7, bg);
  tft.drawRoundRect(bx, by, bs, bs, 7, COL_ACCENT);

  const int cx = bx + 12;
  const int cy = by + 12;

  tft.drawCircle(cx, cy + 1, 6, fg);
  tft.drawFastHLine(cx - 4, cy - 5, 9, bg);
  tft.drawFastVLine(cx, cy - 7, 6, fg);
}

// Small top-bar shortcut; does not replace the power button.
void drawStatusShortcut(bool isBack) {
  const int x = 172, y = 5, w = 29, h = 24;
  tft.fillRoundRect(x, y, w, h, 5, COL_PANEL);
  tft.drawRoundRect(x, y, w, h, 5, COL_ACCENT);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_TEXT, COL_PANEL);
  tft.drawString(isBack ? "<" : "S", x + w / 2, y + h / 2, 2);
  tft.setTextDatum(TL_DATUM);
}

void drawNavBar() {
  const int y = SCREEN_H - NAV_H;
  tft.fillRect(0, y, SCREEN_W, NAV_H, COL_PANEL_ALT);
  tft.drawFastHLine(0, y, SCREEN_W, COL_STROKE);

  const int btnW = SCREEN_W / 4;
  const char* names[4] = {"Weather", "Stocks", "Flight Radar", "Weather Radar"};

  for (int i = 0; i < 4; i++) {
    int bx = i * btnW;
    bool active = (currentPage == (i == 3 ? PAGE_WEATHER_RADAR : (Page)i));

    uint16_t bg = active ? COL_ACCENT : COL_PANEL;
    uint16_t fg = active ? TFT_BLACK : COL_TEXT;

    tft.fillRoundRect(bx + 4, y + 6, btnW - 8, NAV_H - 12, 8, bg);
    tft.drawRoundRect(bx + 4, y + 6, btnW - 8, NAV_H - 12, 8, active ? COL_ACCENT : COL_STROKE);

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(fg, bg);
    if (i == 2) {
      tft.drawString("Flight", bx + btnW / 2, y + 15, 1);
      tft.drawString("Radar", bx + btnW / 2, y + 28, 1);
    } else if (i == 3) {
      tft.drawString("Weather", bx + btnW / 2, y + 15, 1);
      tft.drawString("Radar", bx + btnW / 2, y + 28, 1);
    } else {
      tft.drawString(names[i], bx + btnW / 2, y + NAV_H / 2, 1);
    }
  }

  tft.setTextDatum(TL_DATUM);
}

bool makeSpriteCard(TFT_eSprite& spr, int w, int h, bool accent = false) {
  spr.setColorDepth(16);
  if (!spr.createSprite(w, h)) {
    spr.setColorDepth(8);
    if (!spr.createSprite(w, h)) return false;
  }
  spr.fillSprite(COL_BG);
  spr.fillRoundRect(0, 0, w, h, 10, COL_PANEL);
  spr.drawRoundRect(0, 0, w, h, 10, accent ? COL_ACCENT : COL_STROKE);
  return true;
}

void pushSpriteAndDelete(TFT_eSprite& spr, int x, int y) {
  spr.pushSprite(x, y, COL_BG);
  spr.deleteSprite();
}

void drawCleanSunIcon(TFT_eSprite& spr, int cx, int cy, uint16_t c) {
  spr.fillCircle(cx, cy, 4, c);
  spr.drawLine(cx, cy - 9, cx, cy - 7, c);
  spr.drawLine(cx, cy + 7, cx, cy + 9, c);
  spr.drawLine(cx - 9, cy, cx - 7, cy, c);
  spr.drawLine(cx + 7, cy, cx + 9, cy, c);
  spr.drawLine(cx - 6, cy - 6, cx - 5, cy - 5, c);
  spr.drawLine(cx + 5, cy - 5, cx + 6, cy - 6, c);
  spr.drawLine(cx - 6, cy + 6, cx - 5, cy + 5, c);
  spr.drawLine(cx + 5, cy + 5, cx + 6, cy + 6, c);
}

void drawMoonIcon(TFT_eSprite& spr, int cx, int cy, uint16_t c) {
  spr.fillCircle(cx, cy, 6, c);
  spr.fillCircle(cx + 4, cy - 2, 6, COL_PANEL);
}

int drawWrappedTextLimited(int x, int y, int maxW, const String& text, int font, uint16_t fg, uint16_t bg, int maxLines) {
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(fg, bg);

  const int lineH = tft.fontHeight(font) + 2;
  String line = "";
  String word = "";
  int linesDrawn = 0;

  auto flushLine = [&]() {
    if (linesDrawn >= maxLines) return;
    if (line.length() > 0) tft.drawString(line, x, y, font);
    y += lineH;
    line = "";
    linesDrawn++;
  };

  auto placeWordOnEmptyLine = [&]() {
    if (word.length() == 0 || linesDrawn >= maxLines) return;

    while (tft.textWidth(word, font) > maxW && word.length() > 1) {
      int cut = word.length();
      while (cut > 1 && tft.textWidth(word.substring(0, cut), font) > maxW) cut--;
      if (linesDrawn >= maxLines) return;
      tft.drawString(word.substring(0, cut), x, y, font);
      y += lineH;
      linesDrawn++;
      word = word.substring(cut);
    }

    if (linesDrawn < maxLines) {
      line = word;
      word = "";
    }
  };

  auto flushWord = [&]() {
    if (word.length() == 0 || linesDrawn >= maxLines) return;

    if (line.length() == 0) {
      placeWordOnEmptyLine();
      return;
    }

    String candidate = line + " " + word;
    if (tft.textWidth(candidate, font) <= maxW) {
      line = candidate;
      word = "";
      return;
    }

    flushLine();
    placeWordOnEmptyLine();
  };

  for (int i = 0; i < (int)text.length(); i++) {
    if (linesDrawn >= maxLines) break;
    char c = text[i];

    if (c == '\n') {
      flushWord();
      flushLine();
      continue;
    }

    if (c == ' ') {
      flushWord();
      continue;
    }

    word += c;
  }

  if (linesDrawn < maxLines) {
    flushWord();
    if (line.length() > 0) flushLine();
  }

  return y;
}

// =========================================================
// HOME SPRITES
// =========================================================
void drawClockCardDirect(const String& timeBuf, const String& dateBuf, const String& sr, const String& ss) {
  // A memory-independent fallback keeps the clock visible if sprite allocation fails.
  const int x = 8, y = PAGE_ROW1_Y;
  drawCard(x, y, 224, HOME_WIDGET_H, true);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_TEXT, COL_PANEL);
  if (useUsRegionFormat()) {
    int splitAt = timeBuf.lastIndexOf(' ');
    String main = splitAt > 0 ? timeBuf.substring(0, splitAt) : timeBuf;
    String suffix = splitAt > 0 ? timeBuf.substring(splitAt + 1) : "";
    tft.drawString(main, x + 10, y + 11, 4);
    if (suffix.length()) tft.drawString(suffix, x + 10 + tft.textWidth(main, 4) + 4, y + 18, 2);
  } else tft.drawString(timeBuf, x + 10, y + 11, 4);
  tft.setTextColor(COL_DIM, COL_PANEL);
  tft.drawString(dateBuf, x + 10, y + 45, 2);
  tft.setTextColor(COL_ACCENT, COL_PANEL);
  tft.drawString("SR", x + 146, y + 20, 1);
  tft.drawString("SS", x + 146, y + 48, 1);
  tft.drawString(sr, x + 165, y + 15, 2);
  tft.drawString(ss, x + 165, y + 43, 2);
}

void drawClockCardSprite(bool force = false) {
  const int x = 8, y = PAGE_ROW1_Y, w = 224, h = HOME_WIDGET_H;

  time_t now = time(nullptr);
  struct tm tmNow;
  localtime_r(&now, &tmNow);

  String timeBuf = formatClockParts(tmNow, true);
  String dateBuf = formatDateParts(tmNow);

  String sr = formatMinuteOfDay(sunriseMin);
  String ss = formatMinuteOfDay(sunsetMin);

  String combined = timeBuf + "|" + dateBuf + "|" + sr + "|" + ss + "|" +
                    String(COL_ACCENT) + "|" + String(COL_TEXT);

  if (!force && combined == cacheClock) return;
  if (!makeSpriteCard(sprClock, w, h, true)) {
    sprClock.deleteSprite();
    drawClockCardDirect(timeBuf, dateBuf, sr, ss);
    cacheClock = ""; // Retry buffered drawing on the next clock tick.
    return;
  }

  sprClock.setTextDatum(TL_DATUM);

  sprClock.setTextColor(COL_TEXT, COL_PANEL);
  if (useUsRegionFormat()) {
    int splitAt = timeBuf.lastIndexOf(' ');
    String clockMain = splitAt > 0 ? timeBuf.substring(0, splitAt) : timeBuf;
    String clockSuffix = splitAt > 0 ? timeBuf.substring(splitAt + 1) : "";
    sprClock.drawString(clockMain, 10, 11, 4);
    if (clockSuffix.length() > 0) {
      int suffixX = 10 + sprClock.textWidth(clockMain, 4) + 4;
      sprClock.drawString(clockSuffix, suffixX, 18, 2);
    }
  } else {
    sprClock.drawString(timeBuf, 10, 11, 4);
  }

  sprClock.setTextColor(COL_DIM, COL_PANEL);
  sprClock.drawString(dateBuf, 10, 45, 2);

  drawCleanSunIcon(sprClock, 151, 22, COL_ACCENT);
  drawMoonIcon(sprClock, 151, 50, COL_ACCENT);

  sprClock.setTextColor(COL_ACCENT, COL_PANEL);
  sprClock.drawString(sr, 165, 15, 2);
  sprClock.drawString(ss, 165, 43, 2);

  pushSpriteAndDelete(sprClock, x, y);
  cacheClock = combined;
}

void drawMetricSprite(int x, int y, int w, int h, const char* label, const String& value, String& cache, bool force = false, const String& detail = "") {
  String combined = String(label) + "|" + value + "|" + detail + "|" + String(COL_PANEL) + "|" +
                    String(COL_STROKE) + "|" + String(COL_TEXT);

  if (!force && combined == cache) return;
  cache = combined;

  makeSpriteCard(sprSmall, w, h, true);

  sprSmall.setTextDatum(TL_DATUM);
  sprSmall.setTextColor(COL_DIM, COL_PANEL);
  sprSmall.drawString(label, 10, 8, 2);

  sprSmall.setTextColor(COL_TEXT, COL_PANEL);
  sprSmall.drawString(value, 10, 31, 4);

  if (detail.length() > 0) {
    sprSmall.setTextColor(COL_ACCENT, COL_PANEL);
    sprSmall.drawString(detail, 10, 55, 1);
  }

  pushSpriteAndDelete(sprSmall, x, y);
}

void drawWeatherStyleMetricSprite(int x, int y, int w, int h, const char* label, const String& value, String& cache, bool force = false, const String& detail = "") {
  String combined = String(label) + "|" + value + "|" + detail + "|" + String(COL_PANEL) + "|" +
                    String(COL_STROKE) + "|" + String(COL_TEXT);

  if (!force && combined == cache) return;
  cache = combined;

  makeSpriteCard(sprSmall, w, h, true);

  sprSmall.setTextDatum(TL_DATUM);
  sprSmall.setTextColor(COL_DIM, COL_PANEL);
  sprSmall.drawString(label, 10, 8, 2);

  sprSmall.setTextColor(COL_TEXT, COL_PANEL);
  if (strcmp(label, "Wind") == 0 && value.endsWith("mph")) {
    String speed = value.substring(0, value.length() - 3);
    const int windFont = 4;
    sprSmall.drawString(speed, 10, 28, windFont);
    int unitX = 10 + sprSmall.textWidth(speed, windFont) + 2;
    // Scale the unit halfway between bitmap fonts 2 and 4; speed stays font 4.
    TFT_eSprite unitSprite(&tft);
    unitSprite.setColorDepth(16);
    int sourceW = sprSmall.textWidth("mph", windFont);
    int sourceH = sprSmall.fontHeight(windFont);
    int targetH = (sourceH + sprSmall.fontHeight(2)) / 2;
    int targetW = max(1, sourceW * targetH / sourceH);
    int unitY = 28 + sourceH - targetH;
    if (unitSprite.createSprite(sourceW, sourceH)) {
      unitSprite.fillSprite(COL_PANEL);
      unitSprite.setTextDatum(TL_DATUM);
      unitSprite.setTextColor(COL_TEXT, COL_PANEL);
      unitSprite.drawString("mph", 0, 0, windFont);
      for (int dy = 0; dy < targetH; ++dy) {
        for (int dx = 0; dx < targetW && unitX + dx < w - 3; ++dx) {
          uint16_t pixel = unitSprite.readPixel(dx * sourceW / targetW, dy * sourceH / targetH);
          sprSmall.drawPixel(unitX + dx, unitY + dy, pixel);
        }
      }
      unitSprite.deleteSprite();
    } else {
      sprSmall.drawString("mph", unitX, 28 + sourceH - sprSmall.fontHeight(2), 2);
    }
  } else {
    sprSmall.drawString(value, 10, 28, 4);
  }

  if (detail.length() > 0) {
    sprSmall.setTextColor(COL_ACCENT, COL_PANEL);
    sprSmall.drawString(detail, 10, 52, 1);
  }

  pushSpriteAndDelete(sprSmall, x, y);
}

void drawPressureWidget(int x, int y, int w, int h, String &cache, bool force = false) {
  String value = pressureValueText();
  String combined = value + "|" + pressureUnitText() + "|" + pressureTrendText() + "|" +
      String(COL_TEXT) + "|" + String(COL_PANEL) + "|" + String(COL_ACCENT);
  if (!force && combined == cache) return;
  if (!makeSpriteCard(sprSmall, w, h, true)) return;
  cache = combined;
  sprSmall.setTextDatum(TL_DATUM); sprSmall.setTextColor(COL_DIM, COL_PANEL);
  sprSmall.drawString("Pressure", 10, 8, 2);
  sprSmall.setTextColor(COL_TEXT, COL_PANEL);
  int valueFont = sprSmall.textWidth(value, 4) <= w - 36 ? 4 : 2;
  sprSmall.drawString(value, 10, 28, valueFont);
  int arrowX = w - 16, direction = pressureDirection();
  if (direction == 1) {
    sprSmall.fillRect(arrowX - 1, 35, 3, 14, COL_ACCENT);
    sprSmall.fillTriangle(arrowX, 29, arrowX - 5, 36, arrowX + 5, 36, COL_ACCENT);
  } else if (direction == -1) {
    sprSmall.fillRect(arrowX - 1, 29, 3, 14, COL_ACCENT);
    sprSmall.fillTriangle(arrowX - 5, 42, arrowX + 5, 42, arrowX, 49, COL_ACCENT);
  } else if (direction == 2) sprSmall.drawFastHLine(arrowX - 5, 39, 10, COL_DIM);
  // Little change: no arrow. The footer still identifies the steady trend.
  sprSmall.setTextColor(COL_ACCENT, COL_PANEL);
  sprSmall.drawString(pressureUnitText(), 10, 54, 1);
  sprSmall.drawRightString(String("3h ") + pressureTrendText(), w - 10, 54, 1);
  pushSpriteAndDelete(sprSmall, x, y);
}

void drawSunEventWidget(int x, int y, int w, int h, String& cache, bool force = false) {
  String label = nextSunLabel();
  String value = nextSunTimeText();
  String combined = label + "|" + value + "|" + String(COL_PANEL) + "|" +
                    String(COL_STROKE) + "|" + String(COL_TEXT) + "|" + String(COL_ACCENT);

  if (!force && combined == cache) return;
  cache = combined;

  makeSpriteCard(sprSmall, w, h, true);

  sprSmall.setTextDatum(TL_DATUM);
  sprSmall.setTextColor(COL_DIM, COL_PANEL);
  sprSmall.drawString(label, 10, 8, 2);

  sprSmall.setTextColor(COL_TEXT, COL_PANEL);
  if (useUsRegionFormat()) {
    int splitAt = value.lastIndexOf(' ');
    String mainValue = splitAt > 0 ? value.substring(0, splitAt) : value;
    String suffix = splitAt > 0 ? value.substring(splitAt + 1) : "";
    sprSmall.drawString(mainValue, 10, 30, 4);
    if (suffix.length() > 0) {
      int suffixX = 10 + sprSmall.textWidth(mainValue, 4) + 3;
      sprSmall.drawString(suffix, suffixX, 35, 2);
    }
  } else {
    sprSmall.drawString(value, 10, 30, 4);
  }

  pushSpriteAndDelete(sprSmall, x, y);
}

void drawPlaceholderSprite(int x, int y, int w, int h, const char* label, String& cache, bool force = false) {
  String combined = String(label) + "|" + String(COL_PANEL) + "|" + String(COL_STROKE) + "|" + String(COL_TEXT);

  if (!force && combined == cache) return;
  cache = combined;

  makeSpriteCard(sprSmall, w, h, true);

  sprSmall.setTextDatum(TL_DATUM);
  sprSmall.setTextColor(COL_DIM, COL_PANEL);
  sprSmall.drawString(label, 10, 8, 2);

  sprSmall.setTextColor(COL_STROKE, COL_PANEL);
  sprSmall.drawString("Empty", 10, 31, 2);

  pushSpriteAndDelete(sprSmall, x, y);
}

void drawFocusTimerWidget(int x, int y, int w, int h, String& cache, bool force = false) {
  String value = formatTimerClock(focusRemainingSec);
  String hint = focusHintText();
  String combined = value + "|" + hint + "|" + String(focusMenuOpen ? 1 : 0) +
                    "|" + String(COL_PANEL) + "|" + String(COL_ACCENT) + "|" + String(COL_TEXT);

  if (!force && combined == cache) return;
  cache = combined;

  makeSpriteCard(sprSmall, w, h, true);

  sprSmall.setTextDatum(TL_DATUM);
  sprSmall.setTextColor(COL_DIM, COL_PANEL);
  sprSmall.drawString("Timer", 10, 8, 2);

  if (focusMenuOpen) {
    sprSmall.setTextColor(COL_TEXT, COL_PANEL);
    sprSmall.drawString("Select", 10, 22, 4);
    sprSmall.setTextColor(COL_DIM, COL_PANEL);
    sprSmall.drawString("duration", 10, 44, 2);
  } else {
    sprSmall.setTextColor(focusTimerFinished ? COL_GREEN : COL_TEXT, COL_PANEL);
    sprSmall.drawString(value, 10, 24, 4);
    sprSmall.setTextColor(COL_DIM, COL_PANEL);
    sprSmall.drawString(hint, 10, 49, 1);
  }

  pushSpriteAndDelete(sprSmall, x, y);
}

void drawHomeSlotWidget(int slot, bool force = false) {
  int x, y, w, h;
  getHomeSlotRect(slot, x, y, w, h);

  switch (homeWidgetSlots[slot]) {
    case HOME_WIDGET_WEEK:
      drawMetricSprite(x, y, w, h, "Week", weekNumberText(), cacheHomeSlots[slot], force);
      break;
    case HOME_WIDGET_TIMER:
      drawFocusTimerWidget(x, y, w, h, cacheHomeSlots[slot], force);
      break;
    case HOME_WIDGET_RAIN:
      drawWeatherStyleMetricSprite(x, y, w, h, "Rain", rainText(), cacheHomeSlots[slot], force);
      break;
    case HOME_WIDGET_OUTDOOR:
      drawWeatherStyleMetricSprite(x, y, w, h, "Outdoor", tempText(), cacheHomeSlots[slot], force, tempRangeText());
      break;
    case HOME_WIDGET_KP:
      drawWeatherStyleMetricSprite(x, y, w, h, "KP index", kpText(), cacheHomeSlots[slot], force, kpLevelText());
      break;
    case HOME_WIDGET_PRESSURE:
      drawPressureWidget(x, y, w, h, cacheHomeSlots[slot], force);
      break;
    case HOME_WIDGET_WIND:
      drawWeatherStyleMetricSprite(x, y, w, h, "Wind", windText(), cacheHomeSlots[slot], force, windDirectionText());
      break;
    case HOME_WIDGET_SUN:
      drawSunEventWidget(x, y, w, h, cacheHomeSlots[slot], force);
      break;
  }
}

void drawFocusMenuOverlay(bool force = false) {
  String combined = String(focusTimerRunning ? 1 : 0) + "|" + String(focusTimerFinished ? 1 : 0) +
                    "|" + String(COL_PANEL_ALT) + "|" + String(COL_PANEL) + "|" + String(COL_ACCENT);
  if (!force && combined == cacheTimerMenu) return;
  cacheTimerMenu = combined;

  tft.fillRect(0, 0, SCREEN_W, SCREEN_H, COL_BG);
  tft.fillRoundRect(TIMER_MENU_X, TIMER_MENU_Y, TIMER_MENU_W, TIMER_MENU_H, 12, COL_PANEL_ALT);
  tft.drawRoundRect(TIMER_MENU_X, TIMER_MENU_Y, TIMER_MENU_W, TIMER_MENU_H, 12, COL_ACCENT);

  tft.setTextColor(COL_TEXT, COL_PANEL_ALT);
  tft.drawString("Start timer", TIMER_MENU_X + 14, TIMER_MENU_Y + 12, 2);
  tft.setTextColor(COL_DIM, COL_PANEL_ALT);
  tft.drawString("Choose a session length", TIMER_MENU_X + 14, TIMER_MENU_Y + 34, 1);

  const int btnW = 74;
  const int btnH = 28;
  const int col1X = TIMER_MENU_X + 14;
  const int col2X = TIMER_MENU_X + 112;
  const int row1Y = TIMER_MENU_Y + 54;
  const int row2Y = TIMER_MENU_Y + 88;
  const int row3Y = TIMER_MENU_Y + 122;

  String labels[6];
  const int xs[] = {col1X, col2X, col1X, col2X, col1X, col2X};
  const int ys[] = {row1Y, row1Y, row2Y, row2Y, row3Y, row3Y};
  for (int i = 0; i < 6; i++) {
    labels[i] = String(timerPresetMin[i]) + " min";
  }

  for (int i = 0; i < 6; i++) {
    tft.fillRoundRect(xs[i], ys[i], btnW, btnH, 8, COL_PANEL);
    tft.drawRoundRect(xs[i], ys[i], btnW, btnH, 8, COL_STROKE);
    tft.setTextColor(COL_TEXT, COL_PANEL);
    tft.drawCentreString(labels[i].c_str(), xs[i] + btnW / 2, ys[i] + 7, 2);
  }

  const int actionY = TIMER_MENU_Y + 160;
  const int actionW = 152;
  const int actionX = TIMER_MENU_X + 24;
  const char* actionLabel = focusTimerRunning ? "Stop" : (focusTimerFinished ? "Reset" : nullptr);
  uint16_t actionColor = focusTimerRunning ? COL_RED : COL_ACCENT;

  if (actionLabel) {
    tft.fillRoundRect(actionX, actionY - 4, actionW, 26, 8, COL_PANEL);
    tft.drawRoundRect(actionX, actionY - 4, actionW, 26, 8, actionColor);
    tft.setTextColor(actionColor, COL_PANEL);
    tft.drawCentreString(actionLabel, actionX + actionW / 2, actionY + 4, 2);
  } else {
    tft.setTextColor(COL_DIM, COL_PANEL_ALT);
    tft.drawCentreString("Tap outside to close", TIMER_MENU_X + TIMER_MENU_W / 2, TIMER_MENU_Y + TIMER_MENU_H - 13, 1);
  }
}

void drawTimerDoneOverlay(bool force = false) {
  if (!timerDoneDialogOpen) return;

  String elapsed = formatElapsedText(focusDurationSec);
  String countdown = timerDoneCountdownText();
  bool flashOn = flashModeEnabled && ((millis() / 300UL) % 2UL == 0);
  setBacklight(flashModeEnabled ? (flashOn ? FLASH_BL_HIGH : FLASH_BL_LOW) : BL_FULL);
  String combined = elapsed + "|" + String(COL_PANEL_ALT) + "|" + String(COL_ACCENT) + "|" + String(COL_TEXT);
  String flashKey = String(flashOn ? 1 : 0);
  if (force || combined != cacheTimerDone || flashKey != cacheTimerDoneFlash) {
    cacheTimerDone = combined;
    cacheTimerDoneFlash = flashKey;
    cacheTimerDoneCountdown = "";

    uint16_t backdrop = flashOn ? COL_ACCENT : COL_BG;
    uint16_t panelBorder = flashOn ? TFT_WHITE : COL_ACCENT;
    tft.fillRect(0, 0, SCREEN_W, SCREEN_H, backdrop);
    tft.fillRoundRect(TIMER_DONE_X, TIMER_DONE_Y, TIMER_DONE_W, TIMER_DONE_H, 12, COL_PANEL_ALT);
    tft.drawRoundRect(TIMER_DONE_X, TIMER_DONE_Y, TIMER_DONE_W, TIMER_DONE_H, 12, panelBorder);

    tft.setTextColor(COL_TEXT, COL_PANEL_ALT);
    tft.drawCentreString("Timer complete", TIMER_DONE_X + TIMER_DONE_W / 2, TIMER_DONE_Y + 14, 2);
    tft.setTextColor(COL_ACCENT, COL_PANEL_ALT);
    tft.drawCentreString(elapsed, TIMER_DONE_X + TIMER_DONE_W / 2, TIMER_DONE_Y + 42, 2);
    tft.setTextColor(COL_DIM, COL_PANEL_ALT);
    tft.drawCentreString("Tap anywhere to acknowledge", TIMER_DONE_X + TIMER_DONE_W / 2, TIMER_DONE_Y + 68, 1);
  }

  if (force || countdown != cacheTimerDoneCountdown) {
    cacheTimerDoneCountdown = countdown;
    tft.fillRect(TIMER_DONE_X + 28, TIMER_DONE_Y + 82, TIMER_DONE_W - 56, 12, COL_PANEL_ALT);
    tft.setTextColor(COL_DIM, COL_PANEL_ALT);
    tft.drawCentreString(countdown.c_str(), TIMER_DONE_X + TIMER_DONE_W / 2, TIMER_DONE_Y + 82, 1);
  }
}

// =========================================================
// PAGES
// =========================================================
void drawHomePageFull() {
  tft.fillScreen(COL_BG);
  drawTopBar(homeTitleText());
  drawStatusShortcut(false);
  drawNavBar();

  cacheClock = "";
  cacheHomeEmpty1 = "";
  cacheHomeEmpty2 = "";
  cacheFocusTimer = "";
  for (int i = 0; i < HOME_SLOT_COUNT; i++) {
    cacheHomeSlots[i] = "";
  }

  pageDirty = false;
  lastDrawnPage = PAGE_HOME;

  drawClockCardSprite(true);
  for (int i = 0; i < HOME_SLOT_COUNT; i++) {
    drawHomeSlotWidget(i, true);
  }
  if (focusMenuOpen) drawFocusMenuOverlay(true);
  if (timerDoneDialogOpen) drawTimerDoneOverlay(true);
}

void updateHomeDynamic() {
  if (timerDoneDialogOpen) {
    drawTimerDoneOverlay(false);
    return;
  }

  if (focusMenuOpen) {
    drawFocusMenuOverlay(false);
    return;
  }

  drawClockCardSprite(false);
  for (int i = 0; i < HOME_SLOT_COUNT; i++) {
    drawHomeSlotWidget(i, false);
  }
}

// Decode HTTP bodies with explicit yielding and bounded waits. ArduinoJson calls
// Stream::readBytes(), whose default timeout loop does not yield on ESP32.
// This wrapper owns the wait, so its inherited timeout must remain zero.
class RadarBodyStream : public Stream {
  Stream& source;
  bool chunked;
  bool finished = false;
  size_t chunkRemaining = 0;
  int cached = -1;
  uint32_t waitTimeoutMs;
  uint32_t startedMs;
  uint32_t lastYieldMs;
  uint16_t bytesSinceYield = 0;
  int contentRemaining;

  int readSourceByte() {
    uint32_t waitingSince = millis();
    for (;;) {
      uint32_t now = millis();
      if (now - startedMs >= 45000UL) { finished = true; return -1; }
      // Even a fully buffered JSON response must give the CPU's idle task time.
      if (++bytesSinceYield >= 256 || now - lastYieldMs >= 8UL) {
        vTaskDelay(1);
        lastYieldMs = millis(); bytesSinceYield = 0;
      }
      int value = source.read();
      if (value >= 0) return value;
      if (millis() - waitingSince >= waitTimeoutMs) { finished = true; return -1; }
      vTaskDelay(1); // Never spin while the TLS socket is waiting for data.
      lastYieldMs = millis(); bytesSinceYield = 0;
    }
  }

 public:
  RadarBodyStream(Stream& input, bool chunks, uint32_t timeoutMs = 15000UL,
                  int contentLength = -1)
      : source(input), chunked(chunks), waitTimeoutMs(timeoutMs),
        startedMs(millis()), lastYieldMs(startedMs),
        contentRemaining(contentLength) {
    setTimeout(0); // Prevent a second non-yielding timeout inside readBytes().
    if (!chunked && contentRemaining == 0) finished = true;
  }
  int available() override { return cached >= 0 ? 1 : (finished ? 0 : source.available()); }
  int read() override {
    if (cached >= 0) { int value = cached; cached = -1; return value; }
    if (finished) return -1;
    if (chunked && !chunkRemaining) {
      char line[96]; size_t length = 0;
      for (;;) {
        int c = readSourceByte();
        if (c < 0) return -1;
        if (c == '\n') break;
        if (c == '\r') continue;
        if (length >= sizeof(line) - 1) { finished = true; return -1; }
        line[length++] = (char)c;
      }
      line[length] = 0;
      // Ignore optional chunk extensions and surrounding whitespace.
      for (size_t i = 0; i < length; ++i) if (line[i] == ';') { line[i] = 0; break; }
      char* first = line;
      while (*first == ' ' || *first == '\t') ++first;
      char* end = nullptr;
      unsigned long size = strtoul(first, &end, 16);
      if (end) while (*end == ' ' || *end == '\t') ++end;
      if (!*first || end == first || !end || *end || size > 1048576UL) {
        finished = true; return -1;
      }
      if (!size) { finished = true; return -1; }
      chunkRemaining = size;
    }
    int value = readSourceByte();
    if (value < 0) return -1;
    if (chunked && --chunkRemaining == 0) {
      if (readSourceByte() != '\r' || readSourceByte() != '\n') {
        finished = true; return -1;
      }
    } else if (!chunked && contentRemaining > 0 && --contentRemaining == 0) {
      finished = true;
    }
    return value;
  }
  int peek() override { if (cached < 0) cached = read(); return cached; }
  void flush() override {}
  size_t write(uint8_t) override { return 0; }
};

// Dow Jones Industrial Average and Nasdaq Composite intraday charts.
// Yahoo Finance chart data: five-minute bars, refreshed once per minute while open.
const uint32_t STOCK_REFRESH_MS = 60000UL;
const int INDEX_CHART_POINTS = 82;
struct IndexChart {
  float opening, latest;
  float values[INDEX_CHART_POINTS];
  uint16_t minutes[INDEX_CHART_POINTS];
  uint8_t count;
  uint32_t day, fetchedMs;
  bool valid, failed;
  char error[24];
};
IndexChart indexCharts[2] = {};
portMUX_TYPE stocksMux = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t stocksTaskHandle = nullptr;
uint32_t stocksRevision = 0;
String stocksRenderCache = "";

bool fetchIndexChart(int index, IndexChart& result, const MarketSession& session) {
  result = {};
  const char* symbol = index == 0 ? "%5EDJI" : "%5EIXIC";
  NetworkFetchGuard fetchGuard;
  WiFiClientSecure client; client.setInsecure(); client.setTimeout(15000);
  HTTPClient http; http.useHTTP10(true);
  http.setConnectTimeout(5000); http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  for (int host=1; host<=2; ++host) {
    String url = String("https://query") + String(host) +
      ".finance.yahoo.com/v8/finance/chart/" + symbol +
      "?range=1d&interval=5m&includePrePost=false";
    if (!http.begin(client,url)) { snprintf(result.error,sizeof(result.error),"Connection failed"); continue; }
    const char* headers[] = {"Transfer-Encoding"}; http.collectHeaders(headers,1);
    http.addHeader("User-Agent","Mozilla/5.0 Deskbuddy/9.0");
    http.addHeader("Accept-Encoding","identity");
    int code=http.GET();
    if (code!=HTTP_CODE_OK) {
      snprintf(result.error,sizeof(result.error),"%s %d",code>0?"HTTP":"Network",code);
      http.end(); client.stop(); continue;
    }
    DynamicJsonDocument filter(2048);
    filter["chart"]["result"][0]["meta"]["regularMarketPrice"]=true;
    filter["chart"]["result"][0]["meta"]["regularMarketTime"]=true;
    filter["chart"]["result"][0]["meta"]["currentTradingPeriod"]["regular"]["start"]=true;
    filter["chart"]["result"][0]["meta"]["currentTradingPeriod"]["regular"]["end"]=true;
    filter["chart"]["result"][0]["timestamp"][0]=true;
    filter["chart"]["result"][0]["indicators"]["quote"][0]["close"][0]=true;
    filter["chart"]["result"][0]["indicators"]["quote"][0]["open"][0]=true;
    DynamicJsonDocument doc(16384);
    http.getStream().setTimeout(15000);
    String encoding=http.header("Transfer-Encoding"); encoding.toLowerCase();
    RadarBodyStream body(http.getStream(),encoding.indexOf("chunked")>=0,15000UL,http.getSize());
    DeserializationError error=deserializeJson(doc,body,DeserializationOption::Filter(filter));
    http.end(); client.stop();
    if (error) { snprintf(result.error,sizeof(result.error),"Chart data error"); continue; }
    JsonObject chart=doc["chart"]["result"][0];
    JsonArray times=chart["timestamp"];
    JsonArray closes=chart["indicators"]["quote"][0]["close"];
    JsonArray opens=chart["indicators"]["quote"][0]["open"];
    if (chart.isNull() || times.isNull() || closes.isNull() || opens.isNull()) {
      snprintf(result.error,sizeof(result.error),"Chart unavailable"); continue;
    }
    // Do not carry yesterday's trace or baseline into a new session.
    time_t providerStart=chart["meta"]["currentTradingPeriod"]["regular"]["start"].as<long>();
    time_t providerEnd=chart["meta"]["currentTradingPeriod"]["regular"]["end"].as<long>();
    if (providerStart!=session.start || providerEnd<=session.start) {
      snprintf(result.error,sizeof(result.error),"Waiting for today"); continue;
    }
    for (size_t i=0; i<times.size() && i<opens.size(); ++i) {
      time_t stamp=times[i].as<long>();
      if (stamp==session.start && !opens[i].isNull()) {
        result.opening=opens[i].as<float>(); break;
      }
    }
    if (!isfinite(result.opening) || result.opening<=0) {
      snprintf(result.error,sizeof(result.error),"Waiting for opening"); continue;
    }
    // Anchor the start of every day's chart at exactly zero.
    result.values[0]=result.opening; result.minutes[0]=0; result.count=1;
    time_t now=time(nullptr);
    for (size_t i=0; i<times.size() && i<closes.size(); ++i) {
      if (times[i].isNull() || closes[i].isNull()) continue;
      time_t stamp=times[i].as<long>(); float value=closes[i].as<float>();
      if (stamp<session.start || stamp>session.end || stamp>now || !isfinite(value) || value<=0) continue;
      // A candle close belongs at the end of its interval, or at the latest quote
      // time for the current unfinished candle, rather than at the opening tick.
      time_t quoteTime=chart["meta"]["regularMarketTime"].as<long>();
      time_t closeTime=stamp+300;
      if (closeTime>session.end) closeTime=session.end;
      if (closeTime>quoteTime) closeTime=quoteTime;
      if (closeTime<stamp) continue;
      uint16_t minute=(closeTime-session.start)/60;
      if (result.count>1 && minute<=result.minutes[result.count-1]) continue;
      if (result.count>=INDEX_CHART_POINTS) break;
      result.minutes[result.count]=minute; result.values[result.count++]=value;
    }
    result.latest=chart["meta"]["regularMarketPrice"].as<float>();
    time_t latestTime=chart["meta"]["regularMarketTime"].as<long>();
    if (isfinite(result.latest) && result.latest>0 && latestTime>=session.start && latestTime<=session.end && latestTime<=now) {
      uint16_t minute=(latestTime-session.start)/60;
      if (minute>result.minutes[result.count-1] && result.count<INDEX_CHART_POINTS) {
        result.minutes[result.count]=minute; result.values[result.count++]=result.latest;
      }
    } else result.latest=result.values[result.count-1];
    result.day=session.day; result.fetchedMs=millis(); result.valid=true;
    result.error[0]=0; return true;
  }
  return false;
}

void stocksNetworkTask(void*) {
  uint32_t lastFetched=0, lastDay=0;
  for (;;) {
    MarketSession session=marketSessionAt(time(nullptr));
    bool refresh=session.open && wifiEnabled && WiFi.status()==WL_CONNECTED &&
      (lastDay!=session.day || lastFetched==0 || millis()-lastFetched>=STOCK_REFRESH_MS);
    if (refresh) {
      lastDay=session.day; lastFetched=millis();
      for (int i=0; i<2; ++i) {
        if (!marketSessionAt(time(nullptr)).open) break;
        IndexChart result={}; bool ok=fetchIndexChart(i,result,session);
        portENTER_CRITICAL(&stocksMux);
        if (ok) indexCharts[i]=result;
        else {
          if (indexCharts[i].day!=session.day) indexCharts[i]={};
          indexCharts[i].failed=true;
          snprintf(indexCharts[i].error,sizeof(indexCharts[i].error),"%s",result.error);
        }
        ++stocksRevision;
        portEXIT_CRITICAL(&stocksMux);
      }
    }
    // One-second checks detect opening, reconnection and early closing promptly.
    ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(1000));
  }
}

void drawIndexChangeSegment(int x1,int y1,int x2,int y2,int zero) {
  if ((y1<zero && y2>zero) || (y1>zero && y2<zero)) {
    int cross=x1+(int)roundf((x2-x1)*(zero-y1)/(float)(y2-y1));
    tft.drawLine(x1,y1,cross,zero,y1<zero?COL_GREEN:COL_RED);
    tft.drawLine(cross,zero,x2,y2,y2<zero?COL_GREEN:COL_RED);
  } else {
    int sample=y1==zero?y2:y1;
    tft.drawLine(x1,y1,x2,y2,sample<zero?COL_GREEN:sample>zero?COL_RED:COL_DIM);
  }
}

void drawIndexChartCard(int index,int y,const IndexChart& chart,const MarketSession& session) {
  const int left=58,right=224,top=y+37,bottom=y+91,zero=(top+bottom)/2;
  tft.fillRoundRect(8,y,224,110,6,COL_PANEL);
  tft.setTextDatum(TL_DATUM); tft.setTextColor(COL_TEXT,COL_PANEL);
  tft.drawString(index==0?"Dow Jones":"Nasdaq",14,y+5,2);
  bool valid=chart.valid && chart.day==session.day;
  if (valid) {
    float delta=chart.latest-chart.opening;
    tft.setTextColor(delta>=0?COL_GREEN:COL_RED,COL_PANEL);
    float percent=delta/chart.opening*100.0f;
    String change=(delta>=0?"+$":"-$")+String(fabsf(delta),2)+
      " ("+(percent>=0?String("+"):String("-"))+String(fabsf(percent),2)+"%)";
    tft.drawRightString(change,225,y+10,1);
    // Show the index level below the change, independently of its direction.
    String total = String(chart.latest, 2);
    for (int at = total.indexOf('.') - 3; at > 0; at -= 3)
      total = total.substring(0, at) + "," + total.substring(at);
    tft.setTextColor(COL_TEXT, COL_PANEL);
    tft.drawRightString(total + " pts", 225, y+23, 1);
  }
  float extent=1.0f;
  if (valid) for (int i=0; i<chart.count; ++i) extent=fmaxf(extent,fabsf(chart.values[i]-chart.opening));
  extent*=1.10f;
  // Equal positive/negative limits and an even plot height keep zero centered.
  float magnitude=powf(10.0f,floorf(log10f(extent)));
  extent=ceilf(extent/magnitude)*magnitude;
  int decimals=extent<10?1:0;
  float halfExtent=extent*0.5f;
  int halfDecimals=fabsf(halfExtent-roundf(halfExtent))>0.001f?1:decimals;
  int positiveHalf=zero-(int)roundf((zero-top)*0.5f);
  int negativeHalf=zero+(int)roundf((bottom-zero)*0.5f);
  tft.setTextColor(COL_GREEN,COL_PANEL);
  tft.drawRightString("+$"+String(extent,decimals),left-5,top-3,1);
  tft.drawRightString("+$"+String(halfExtent,halfDecimals),left-5,positiveHalf-3,1);
  tft.setTextColor(COL_DIM,COL_PANEL);
  tft.drawRightString("$0",left-5,zero-3,1);
  tft.setTextColor(COL_RED,COL_PANEL);
  tft.drawRightString("-$"+String(halfExtent,halfDecimals),left-5,negativeHalf-3,1);
  tft.drawRightString("-$"+String(extent,decimals),left-5,bottom-3,1);
  tft.drawFastVLine(left,top,bottom-top+1,COL_STROKE);
  tft.drawFastHLine(left,zero,right-left+1,COL_DIM);
  tft.drawFastHLine(left,bottom,right-left+1,COL_DIM);
  const int dollarTicks[] = {top,positiveHalf,zero,negativeHalf,bottom};
  for (int tick : dollarTicks) tft.drawFastHLine(left-3,tick,7,COL_DIM);
  int sessionMinutes=(session.end-session.start)/60;
  tft.setTextColor(COL_DIM,COL_PANEL);
  // Unlabeled half-hour ticks, including the 9:30 session start.
  for (int minute=0; minute<sessionMinutes; minute+=60) {
    int x=left+(int)roundf((right-left)*minute/(float)sessionMinutes);
    tft.drawFastVLine(x,bottom-1,5,COL_TEXT);
  }
  for (int hour=10; hour<=16; ++hour) {
    int minute=hour*60-570;
    if (minute>sessionMinutes) continue;
    int x=left+(int)roundf((right-left)*minute/(float)sessionMinutes);
    tft.drawFastVLine(x,bottom-3,7,COL_TEXT);
    String label=String(hour>12?hour-12:hour)+(hour<12?"a":"p");
    tft.setTextDatum(MC_DATUM); tft.drawString(label,x,bottom+10,1);
  }
  tft.setTextDatum(TL_DATUM);
  tft.drawString("ET",14,bottom+7,1);
  if (!valid) {
    tft.setTextDatum(MC_DATUM); tft.setTextColor(COL_DIM,COL_PANEL);
    String message=WiFi.status()!=WL_CONNECTED?"WiFi offline":chart.failed?String(chart.error):"Loading chart...";
    while (tft.textWidth(message,1)>right-left-8 && message.length()>1) message.remove(message.length()-1);
    tft.drawString(message,(left+right)/2,zero-10,1); tft.setTextDatum(TL_DATUM); return;
  }
  int lastX=left,lastY=zero;
  for (int i=1; i<chart.count; ++i) {
    int x=left+(int)roundf((right-left)*chart.minutes[i]/(float)sessionMinutes);
    float delta=chart.values[i]-chart.opening;
    int py=zero-(int)roundf(delta/extent*(bottom-top)/2);
    py=constrain(py,top,bottom);
    // Leave gaps for missing bars instead of inventing intermediate data.
    if (chart.minutes[i]-chart.minutes[i-1]<=10) drawIndexChangeSegment(lastX,lastY,x,py,zero);
    lastX=x; lastY=py;
  }
  tft.fillCircle(lastX,lastY,2,lastY<zero?COL_GREEN:lastY>zero?COL_RED:COL_DIM);
  if (chart.failed || millis()-chart.fetchedMs>3UL*STOCK_REFRESH_MS || WiFi.status()!=WL_CONNECTED) {
    tft.setTextColor(COL_DIM,COL_PANEL); tft.drawString("stale",14,y+22,1);
  }
}

void drawStocksPageFull() {
  tft.fillScreen(COL_BG); drawTopBar("Stocks"); drawNavBar();
  stocksRenderCache=""; pageDirty=false; lastDrawnPage=PAGE_STOCKS;
}

void updateStocksDynamic() {
  MarketSession session=marketSessionAt(time(nullptr));
  IndexChart snapshot[2]; uint32_t revision;
  portENTER_CRITICAL(&stocksMux);
  memcpy(snapshot,indexCharts,sizeof(snapshot)); revision=stocksRevision;
  portEXIT_CRITICAL(&stocksMux);
  bool stale0=snapshot[0].valid && millis()-snapshot[0].fetchedMs>3UL*STOCK_REFRESH_MS;
  bool stale1=snapshot[1].valid && millis()-snapshot[1].fetchedMs>3UL*STOCK_REFRESH_MS;
  String signature=String(session.clockReady)+String(session.open)+String(session.day)+"|"+
    String(revision)+String(WiFi.status())+String(stale0)+String(stale1);
  if (signature==stocksRenderCache) return;
  stocksRenderCache=signature;
  tft.fillRect(0,TOPBAR_H,SCREEN_W,SCREEN_H-NAV_H-TOPBAR_H,COL_BG);
  if (!session.clockReady || !session.open) {
    tft.setTextDatum(MC_DATUM); tft.setTextColor(COL_TEXT,COL_BG);
    tft.drawString(session.clockReady?"Market Closed":"Syncing time...",SCREEN_W/2,155,2);
    tft.setTextDatum(TL_DATUM); return;
  }
  drawIndexChartCard(0,42,snapshot[0],session);
  drawIndexChartCard(1,158,snapshot[1],session);
}

// =========================================================
// PLANE RADAR (adapted from ESP32-Plane-Radar 1.1.5)
// Uses Desk Buddy WiFi, TFT_eSPI display, touch, and LAT/LNG settings.
// ADS-B source: adsb.fi public API.
// =========================================================
struct RadarAircraft {
  float lat, lon, heading, track, gs;
  char callsign[10];
  char planeType[9]; // Used only to choose the silhouette, never printed.
  char category[3];
  char alt[12];
};

static constexpr float RADAR_OUTER_MILES = 15.0f;
static RadarAircraft radarAircraft[64];
static size_t radarAircraftCount = 0;
static SemaphoreHandle_t radarMutex = nullptr;
static volatile bool radarDataChanged = false;
static unsigned long radarLastFetchMs = 0;
static bool radarFetchOK = false;
static bool radarHasData = false;
static char radarLastError[40] = "";
static TaskHandle_t radarTaskHandle = nullptr;

float radarOuterKm() { return RADAR_OUTER_MILES * 1.609344f; }

static float radarJsonFloat(JsonObject p, const char* key, float fallback = 0.0f) {
  if (p.containsKey(key) && !p[key].isNull() && !p[key].is<const char*>()) return p[key].as<float>();
  return fallback;
}

static void radarCopyTrimmed(JsonObject p, const char* key, char* out, size_t n) {
  if (!n) return;
  out[0] = 0;
  const char* s = p[key] | "";
  strncpy(out, s, n - 1); out[n - 1] = 0;
  int len = strlen(out);
  while (len > 0 && out[len - 1] == ' ') out[--len] = 0;
}

void setRadarFetchError(const char* error) {
  if (radarMutex) xSemaphoreTake(radarMutex, portMAX_DELAY);
  radarFetchOK = false;
  snprintf(radarLastError, sizeof(radarLastError), "%s", error);
  radarDataChanged = true;
  if (radarMutex) xSemaphoreGive(radarMutex);
  Serial.printf("radar: %s; free heap: %u\n", error, (unsigned)ESP.getFreeHeap());
}
bool fetchRadarAircraft() {
  if (WiFi.status() != WL_CONNECTED || !wifiEnabled) {
    setRadarFetchError(wifiEnabled ? "WiFi offline" : "WiFi off");
    return false;
  }
  int distNm = (int)ceilf(radarOuterKm() / 1.852f);
  String url = String("https://opendata.adsb.fi/api/v3/lat/") + String(WR_MAP_LAT, 6) +
               "/lon/" + String(WR_MAP_LON, 6) + "/dist/" + String(distNm);

  NetworkFetchGuard fetchGuard;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true); // Also handle chunking if the server sends it anyway.
  http.setConnectTimeout(5000);
  http.setTimeout(30000);
  client.setTimeout(30000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) {
    setRadarFetchError("HTTP setup failed");
    return false;
  }
  const char* responseHeaders[] = {"Transfer-Encoding"};
  http.collectHeaders(responseHeaders, 1);
  http.addHeader("Accept-Encoding", "identity");
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    String message = code > 0 ? "HTTP " + String(code) : "Network " + String(code);
    Serial.println(HTTPClient::errorToString(code));
    setRadarFetchError(message.c_str());
    http.end();
    return false;
  }

  DynamicJsonDocument filter(2048);
  // Array filters must describe element zero, not an object at the array key.
  const char* keys[] = {"lat","lon","true_heading","mag_heading","track","dir","gs","alt_baro","alt_geom","flight","hex","t","category"};
  for (const char* k : keys) {
    filter["ac"][0][k] = true;
    filter["aircraft"][0][k] = true;
  }
  DynamicJsonDocument doc(24576);
  http.getStream().setTimeout(30000);
  String encoding = http.header("Transfer-Encoding");
  encoding.toLowerCase();
  RadarBodyStream body(http.getStream(), encoding.indexOf("chunked") >= 0,15000UL,http.getSize());
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    String message = "JSON " + String(err.c_str());
    setRadarFetchError(message.c_str());
    return false;
  }
  // An empty array is a valid zero count. A missing array is a response error.
  if (!doc["ac"].is<JsonArray>() && !doc["aircraft"].is<JsonArray>()) {
    setRadarFetchError("No aircraft array");
    return false;
  }

  RadarAircraft temp[64] = {};
  size_t count = 0;
  JsonArray ac = doc["ac"].is<JsonArray>() ? doc["ac"].as<JsonArray>() : doc["aircraft"].as<JsonArray>();
  for (JsonObject p : ac) {
    if (count >= 64 || p["lat"].isNull() || p["lon"].isNull()) continue;
    if (p["alt_baro"].is<const char*>() && !strcmp(p["alt_baro"].as<const char*>(), "ground")) continue;
    RadarAircraft &a = temp[count];
    a.lat = p["lat"].as<float>(); a.lon = p["lon"].as<float>();
    a.track = radarJsonFloat(p, "track", radarJsonFloat(p, "dir", radarJsonFloat(p, "true_heading", NAN)));
    a.heading = radarJsonFloat(p, "true_heading", radarJsonFloat(p, "mag_heading", a.track));
    a.gs = radarJsonFloat(p, "gs", 0);
    radarCopyTrimmed(p, "t", a.planeType, sizeof(a.planeType));
    radarCopyTrimmed(p, "category", a.category, sizeof(a.category));
    radarCopyTrimmed(p, "flight", a.callsign, sizeof(a.callsign));
    if (!a.callsign[0]) radarCopyTrimmed(p, "hex", a.callsign, sizeof(a.callsign));
    if (p["alt_baro"].is<float>() || p["alt_baro"].is<int>()) snprintf(a.alt, sizeof(a.alt), "%dft", (int)p["alt_baro"].as<float>());
    else if (p["alt_geom"].is<float>() || p["alt_geom"].is<int>()) snprintf(a.alt, sizeof(a.alt), "%dft", (int)p["alt_geom"].as<float>());
    else a.alt[0] = 0;
    count++;
  }
  if (radarMutex) xSemaphoreTake(radarMutex, portMAX_DELAY);
  memcpy(radarAircraft, temp, count * sizeof(RadarAircraft));
  radarAircraftCount = count;
  radarLastFetchMs = millis(); radarFetchOK = true; radarHasData = true;
  radarLastError[0] = 0; radarDataChanged = true;
  if (radarMutex) xSemaphoreGive(radarMutex);
  Serial.printf("radar: %u aircraft\n", (unsigned)count);
  return true;
}

void radarFetchTask(void*) {
  for (;;) {
    fetchRadarAircraft();
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}

void initRadar() {
  // Fixed Thomson 15-mile-radius view; ignore obsolete saved range settings.
  radarMutex = xSemaphoreCreateMutex();
  if (!radarMutex) {
    setRadarFetchError("Radar memory error");
    return;
  }
  if (!radarTaskHandle && xTaskCreate(radarFetchTask, "radarFetch", 12288, nullptr, 1, &radarTaskHandle) != pdPASS) {
    setRadarFetchError("Radar task failed");
  }
}

// Compose each frame in memory so the sweep does not flash the display.
TFT_eSprite sprRadar = TFT_eSprite(&tft);
bool radarSpriteReady = false;
static SemaphoreHandle_t radarWorkspaceMutex = nullptr;
static uint8_t *radarWorkspace = nullptr;
void initRadarWorkspace() {
  if (!radarWorkspaceMutex) radarWorkspaceMutex = xSemaphoreCreateMutex();
  if (!radarSpriteReady) {
    sprRadar.setColorDepth(8);
    radarSpriteReady = sprRadar.createSprite(221, 221) != nullptr;
  }
  if (radarSpriteReady) radarWorkspace = static_cast<uint8_t *>(sprRadar.getPointer());
}


// Compact 3x5 glyphs: four pixels per character, six pixels per label line.
void drawCompactFlightText(const String& text, int x, int y, uint16_t color) {
  const char* characters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.? ";
  static const uint16_t glyphs[] = {
    0b010101111101101, 0b110101110101110, 0b011100100100011,
    0b110101101101110, 0b111100110100111, 0b111100110100100,
    0b011100101101011, 0b101101111101101, 0b111010010010111,
    0b001001001101010, 0b101101110101101, 0b100100100100111,
    0b101111111101101, 0b101111111111101, 0b010101101101010,
    0b110101110100100, 0b010101101111011, 0b110101110101101,
    0b011100010001110, 0b111010010010010, 0b101101101101111,
    0b101101101101010, 0b101101111111101, 0b101101010101101,
    0b101101010010010, 0b111001010100111,
    0b111101101101111, 0b010110010010111, 0b110001010100111,
    0b110001010001110, 0b101101111001001, 0b111100110001110,
    0b011100111101111, 0b111001010010010, 0b111101111101111,
    0b111101111001110, 0b000000111000000, 0b000000000000010,
    0b110001010000010, 0
  };
  for (unsigned int i = 0; i < text.length(); ++i) {
    char c = text[i];
    if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    const char* match = strchr(characters, c);
    uint16_t glyph = glyphs[match ? match - characters : 38];
    int tx = x + i * 4;
    sprRadar.fillRect(tx, y, 4, 6, TFT_BLACK);
    for (int row = 0; row < 5; ++row) {
      for (int col = 0; col < 3; ++col) {
        if (glyph & (1U << (14 - row * 3 - col))) sprRadar.drawPixel(tx + col, y + row, color);
      }
    }
  }
}



// Blend ring pixels with the completed sweep frame; the TFT has no alpha channel.
void blendRadarRingPixel(int x, int y, uint16_t color, uint8_t opacity) {
  if (x < 0 || x >= 221 || y < 0 || y >= 221) return;
  uint16_t background = sprRadar.readPixel(x, y);
  uint32_t inverse = 255 - opacity;
  uint32_t red = (((color >> 11) & 31) * opacity + ((background >> 11) & 31) * inverse + 127) / 255;
  uint32_t green = (((color >> 5) & 63) * opacity + ((background >> 5) & 63) * inverse + 127) / 255;
  uint32_t blue = ((color & 31) * opacity + (background & 31) * inverse + 127) / 255;
  sprRadar.drawPixel(x, y, (uint16_t)((red << 11) | (green << 5) | blue));
}

void drawTransparentRadarRing(int cx, int cy, int radius, uint16_t color) {
  int x = 0, y = radius, decision = 1 - radius;
  while (x <= y) {
    int offsetsX[8] = {x, y, y, x, -x, -y, -y, -x};
    int offsetsY[8] = {y, x, -x, -y, -y, -x, x, y};
    for (int point = 0; point < 8; ++point) {
      bool duplicate = false;
      for (int previous = 0; previous < point; ++previous) {
        if (offsetsX[point] == offsetsX[previous] && offsetsY[point] == offsetsY[previous]) duplicate = true;
      }
      if (!duplicate) blendRadarRingPixel(cx + offsetsX[point], cy + offsetsY[point], color, 128);
    }
    ++x;
    if (decision < 0) decision += 2 * x + 1;
    else { --y; decision += 2 * (x - y) + 1; }
  }
}



// Offline Thomson map. Hide it if the configured center changes rather than
// displaying geography under an incorrect radar position. Rebuild for a new center.
void drawRadarBasemap() {
  for (size_t i = 0; i < sizeof(PLANE_MAP_RUNS) / sizeof(PLANE_MAP_RUNS[0]); ++i) {
    uint8_t run[4];
    memcpy_P(run, PLANE_MAP_RUNS[i], sizeof(run));
    uint16_t color = pgm_read_word(WR_BASEMAP_PALETTE + run[3]);
    sprRadar.drawFastHLine(run[1], run[0], run[2], color);
  }
}

// ICAO type designators are a best-effort family lookup. Unknown types use jets.
// ADS-B category A7 identifies rotorcraft even when the type is absent.
int radarAircraftIconKind(const char* type, const char* category) {
  String t(type); t.toUpperCase();
  const char* helicopters[] = {"R22","R44","R66","B06","B407","B412","B429","B430",
    "B47","B505","S76","S92","AS50","AS55","AS65","AS32","EC20","EC25","EC30",
    "EC35","EC45","EC55","H500","H269","MD52","MD60","A109","A119","A139",
    "A169","A189","UH1","H60","H64","H47"};
  if (!strcmp(category, "A7")) return 2;
  for (const char* known : helicopters) if (t == known) return 2;
  const char* props[] = {"C150","C152","C162","C170","C172","C175","C177","C180",
    "C182","C185","C188","C206","C207","C208","C210","C310","C337","C340",
    "C402","C404","C414","C421","C425","C441","P28A","P28B","P28R","P28T",
    "P32R","PA18","PA23","PA24","PA27","PA30","PA31","PA32","PA34","PA38",
    "PA44","PA46","M20P","M20T","BE20","BE30","BE35","BE36","BE55",
    "BE58","BE60","BE65","BE76","BE90","BE99","B350","PC6","PC12","TBM7",
    "TBM8","TBM9","AT43","AT45","AT46","AT72","AT75","AT76","DH8A","DH8B",
    "DH8C","DH8D","DHC2","DHC3","DHC6","DA20","DA40","DA42","DA50","DA62",
    "SR20","SR22","S22T","C130","C30J","P3","A400","SF34"};
  for (const char* known : props) if (t == known) return 1;
  return 0;
}

void drawRadarAircraftIcon(int x, int y, float track, const char* type, const char* category) {
  int kind = radarAircraftIconKind(type, category);
  float angle = (isfinite(track) ? track : 0.0f) * DEG_TO_RAD;
  float east = sinf(angle), north = -cosf(angle);
  const float iconScale = 0.6f;
  auto px = [&](float side, float forward) { return x + (int)roundf(iconScale * (side * -north + forward * east)); };
  auto py = [&](float side, float forward) { return y + (int)roundf(iconScale * (side * east + forward * north)); };
  auto paint = [&](int ox, int oy, uint16_t color) {
    auto line = [&](float s1, float f1, float s2, float f2) {
      sprRadar.drawLine(px(s1,f1)+ox,py(s1,f1)+oy,px(s2,f2)+ox,py(s2,f2)+oy,color);
    };
    line(0,-7,0,8); line(1,-3,1,5);
    if (kind == 2) {
      sprRadar.fillCircle(x+ox,y+oy,1,color);
      line(-6,2,6,2); line(0,-4,0,8);
      line(-2,-6,2,-6);
    } else if (kind == 1) {
      line(-7,1,7,1); line(-7,0,7,0);
      line(-3,-5,3,-5);
      line(-3,8,3,8);
    } else {
      sprRadar.fillTriangle(px(0,4)+ox,py(0,4)+oy,px(-7,-2)+ox,py(-7,-2)+oy,px(7,-2)+ox,py(7,-2)+oy,color);
      sprRadar.fillTriangle(px(0,-3)+ox,py(0,-3)+oy,px(-3,-7)+ox,py(-3,-7)+oy,px(3,-7)+ox,py(3,-7)+oy,color);
    }
    line(0,-4,0,7);
  };
  paint(0,0,TFT_RED);
  // Direction-unknown marker; north remains the neutral icon pose.
  if (!isfinite(track)) sprRadar.fillCircle(x+5,y+4,1,TFT_RED);
}

void drawRadarPageFull() {
  tft.fillScreen(COL_BG);
  drawTopBar("Flight Radar");
  drawNavBar();
  tft.fillRect(0, TOPBAR_H, SCREEN_W, SCREEN_H - NAV_H - TOPBAR_H, TFT_BLACK);
  if (!radarSpriteReady) initRadarWorkspace();
  pageDirty = false;
  lastDrawnPage = PAGE_RADAR;
  radarDataChanged = true;
}

void updateRadarDynamic() {
  static unsigned long lastRadarDraw = 0;
  uint32_t now = millis();
  if (!radarDataChanged && now - lastRadarDraw < 33UL) return;
  lastRadarDraw = now;
  bool contentChanged = radarDataChanged;
  radarDataChanged = false;
  if (!radarSpriteReady) {
    tft.setTextColor(COL_TEXT, TFT_BLACK);
    tft.drawString("Preparing radar...", 18, 90, 2);
    static uint32_t lastBufferTry = 0;
    if (now - lastBufferTry >= 1000) { lastBufferTry = now; pageDirty = true; }
    return;
  }

  if (!radarWorkspaceMutex || xSemaphoreTake(radarWorkspaceMutex, 0) != pdTRUE) return;
  // Coordinates are local to the sprite pushed at (10,36).
  const int cx = 110, cy = 110, R = 110;
  float outer = radarOuterKm();
  sprRadar.fillSprite(TFT_BLACK);

  // One clockwise revolution every six seconds, at up to 30 frames/second.
  float sweep = (now % 6000UL) * (360.0f / 6000.0f);
  // Half-degree spokes and a gradual fade fill the trail without coarse bands.
  for (int step = 48; step >= 0; --step) {
    float fade = 1.0f - step / 48.0f;
    float angle = (sweep - step * 0.5f - 90.0f) * DEG_TO_RAD;
    uint16_t color = tft.color565(10 + (int)(fade * 12),
                                  30 + (int)(fade * 96),
                                  24 + (int)(fade * 44));
    sprRadar.drawLine(cx, cy, cx + (int)(cosf(angle) * R),
                      cy + (int)(sinf(angle) * R), color);
  }
  drawRadarBasemap();
  // Light-green rings blend at 50% opacity over the sweep and basemap.
  const uint16_t ringColor = tft.color565(110, 255, 110);
  for (int ring = 1; ring <= 5; ++ring) {
    int radius = (int)roundf(R * ring / 5.0f);
    drawTransparentRadarRing(cx, cy, radius, ringColor);
    drawTransparentRadarRing(cx, cy, radius - 1, ringColor);
  }
  sprRadar.drawFastHLine(cx-R, cy, R*2+1, COL_STROKE);
  sprRadar.drawFastVLine(cx, cy-R, R*2+1, COL_STROKE);
  float leading = (sweep - 90.0f) * DEG_TO_RAD;
  sprRadar.drawLine(cx, cy, cx + (int)(cosf(leading) * R),
                    cy + (int)(sinf(leading) * R), COL_GREEN);
  sprRadar.fillCircle(cx, cy, 2, COL_TEXT);

  // Eight compass headings, clockwise from north, outside the outer ring.
  const char* compassHeadings[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  sprRadar.setTextColor(COL_TEXT, TFT_BLACK);
  sprRadar.setTextDatum(MC_DATUM);
  for (int heading = 0; heading < 8; ++heading) {
    float angle = (heading * 45.0f - 90.0f) * DEG_TO_RAD;
    int headingRadius = R - 9;
    int hx = cx + (int)roundf(cosf(angle) * headingRadius);
    int hy = cy + (int)roundf(sinf(angle) * headingRadius);
    sprRadar.drawString(compassHeadings[heading], hx, hy, 1);
  }
  sprRadar.setTextDatum(TL_DATUM);
  for (int ring = 1; ring <= 5; ++ring) {
    int radius = (int)roundf(R * ring / 5.0f);
    float distanceMiles = RADAR_OUTER_MILES * ring / 5.0f;
    String label = String((int)roundf(distanceMiles)) + "mi";
    int x = cx + (int)(radius * 0.7071f) - 15;
    int y = cy + (int)(radius * 0.7071f) - 4;
    sprRadar.setTextColor(ringColor, TFT_BLACK);
    sprRadar.drawString(label, x, y, 1);
  }

  RadarAircraft local[64];
  size_t n = 0;
  if (radarMutex) xSemaphoreTake(radarMutex, portMAX_DELAY);
  n = radarAircraftCount;
  if (n > 64) n = 64;
  memcpy(local, radarAircraft, n * sizeof(RadarAircraft));
  bool ok = radarFetchOK;
  bool hasData = radarHasData;
  char fetchError[40];
  snprintf(fetchError, sizeof(fetchError), "%s", radarLastError);
  unsigned long age = radarLastFetchMs;
  if (radarMutex) xSemaphoreGive(radarMutex);

  float latRad = WR_MAP_LAT * DEG_TO_RAD;
  float kmPerLon = 111.31949f * cosf(latRad);
  float centerMercator = asinhf(tanf(latRad));
  float northScale = 6378.137f * cosf(latRad);
  size_t visibleCount = 0;
  for (size_t i = 0; i < n; ++i) {
    float east = (local[i].lon - WR_MAP_LON) * kmPerLon;
    float north = northScale * (asinhf(tanf(local[i].lat * DEG_TO_RAD)) - centerMercator);
    float px = cx + east / outer * R, py = cy - north / outer * R;
    float dx = px-cx, dy = py-cy;
    if (!isfinite(dx) || !isfinite(dy)) continue;
    ++visibleCount; // Includes off-range planes represented by ring dots.
    if (dx*dx + dy*dy > R*R) continue;
    int x = (int)px, y = (int)py;
    drawRadarAircraftIcon(x, y, local[i].track, local[i].planeType, local[i].category);
    String flight = local[i].callsign[0] ? String(local[i].callsign) : "Unknown";
    String altitude = local[i].alt[0] ? String(local[i].alt) : "Alt --";
    int labelW = 4 * (int)max(flight.length(), altitude.length());
    int labelX = x + 6;
    if (labelX + labelW > 219) labelX = x - labelW - 6;
    labelX = constrain(labelX, 2, max(2, 219 - labelW));
    int labelY = constrain(y - 6, 10, 209);
    sprRadar.setTextColor(COL_TEXT, TFT_BLACK);
    drawCompactFlightText(flight, labelX, labelY, COL_TEXT);
    sprRadar.setTextColor(COL_DIM, TFT_BLACK);
    drawCompactFlightText(altitude, labelX, labelY + 6, COL_DIM);
  }
  // Show otherwise skipped planes as red bearing dots on the outer ring.
  // Draw after labels so the dots stay visible.
  for (size_t i = 0; i < n; ++i) {
    float dx = (local[i].lon - WR_MAP_LON) * kmPerLon / outer * R;
    float dy = -northScale * (asinhf(tanf(local[i].lat * DEG_TO_RAD)) - centerMercator) / outer * R;
    float distance = hypotf(dx, dy);
    if (!isfinite(distance) || distance <= R) continue;
    float scale = R / distance;
    sprRadar.fillCircle((int)roundf(cx + dx * scale),
                        (int)roundf(cy + dy * scale), 3, TFT_RED);
  }
  String stat;
  if (!wifiEnabled) stat = "WiFi off";
  else if (hasData) {
    stat = (visibleCount == 0) ? String("Sky clear") : String((unsigned)visibleCount) + " aircraft";
    if (!ok && fetchError[0]) stat = String((unsigned)visibleCount) + " ac " + fetchError;
    else if (now-age > 30000UL) stat += " stale";
  } else if (fetchError[0]) stat = fetchError;
  else stat = "Connecting...";
  // Fit diagnostics into the status area without overwriting the range value.
  while (stat.length() > 1 && sprRadar.textWidth(stat, 1) > 128) {
    stat = stat.substring(0, stat.length() - 2) + "~";
  }
  sprRadar.setTextDatum(TL_DATUM);
  sprRadar.pushSprite(10, 36);
  static String lastFooter;
  if (contentChanged || stat != lastFooter) {
    tft.fillRect(0, 264, SCREEN_W, 12, TFT_BLACK);
    tft.setTextDatum(TL_DATUM); tft.setTextColor(COL_TEXT, TFT_BLACK);
    tft.drawString("15mi N", 6, 266, 1);
    tft.setTextDatum(TR_DATUM); tft.drawString(stat, 234, 266, 1);
    tft.setTextDatum(TL_DATUM); lastFooter = stat;
  }
  xSemaphoreGive(radarWorkspaceMutex);
}

bool handleRadarTouch(int x, int y) {
  if (currentPage != PAGE_RADAR) return false;
  if (y >= 42 && y < SCREEN_H - NAV_H) return true; // Fixed 15-mile view.
  return false;
}

void drawNotesPageFull() {
  tft.fillScreen(COL_BG);
  drawTopBar("Notes");
  drawNavBar();

  drawCard(8, 42, 224, 226, true);

  pageDirty = false;
  lastDrawnPage = PAGE_RADAR;
  lastNotesText = "";
}

void updateNotesDynamic() {
  if (notesText != lastNotesText || notesDirty) {
    tft.fillRect(18, 54, 204, 196, COL_PANEL);
    drawWrappedTextLimited(18, 54, 198, notesText, 2, COL_TEXT, COL_PANEL, 12);
    lastNotesText = notesText;
    notesDirty = false;
  }
}

void drawStatusPageFull() {
  tft.fillScreen(COL_BG);
  drawTopBar("Status");
  drawStatusShortcut(true);
  drawNavBar();

  drawCard(8, PAGE_ROW1_Y, 108, PAGE_WIDGET_H, true);
  drawCard(124, PAGE_ROW1_Y, 108, PAGE_WIDGET_H, true);
  drawCard(8, PAGE_ROW2_Y, 224, PAGE_WIDGET_H, true);
  drawCard(8, PAGE_ROW3_Y, 108, PAGE_WIDGET_H, true);
  drawCard(124, PAGE_ROW3_Y, 108, PAGE_WIDGET_H, true);

  pageDirty = false;
  lastDrawnPage = PAGE_STATUS;

  lastWifiText = "";
  lastSignalText = "";
  lastIpText = "";
  lastUptimeText = "";
  lastNetworkToggleText = "";
}

void updateStatusDynamic() {
  String w = wifiStatusText();
  if (w != lastWifiText) {
    tft.fillRect(18, PAGE_ROW1_Y + 40, 88, 24, COL_PANEL);
    tft.setTextColor(COL_DIM, COL_PANEL);
    tft.drawString("WiFi", 18, PAGE_ROW1_Y + 7, 2);
    tft.drawString("Tap to scan", 18, PAGE_ROW1_Y + 26, 1);
    tft.setTextColor(statusColor(), COL_PANEL);
    tft.drawString(w, 18, PAGE_ROW1_Y + 43, 2);
    lastWifiText = w;
  }

  String s = signalText();
  if (s != lastSignalText) {
    tft.fillRect(134, PAGE_ROW1_Y + 40, 88, 24, COL_PANEL);
    tft.setTextColor(COL_DIM, COL_PANEL);
    tft.drawString("Signal", 134, PAGE_ROW1_Y + 7, 2);
    tft.drawString("Tap: calibrate", 134, PAGE_ROW1_Y + 26, 1);
    tft.setTextColor(COL_TEXT, COL_PANEL);
    tft.drawString(s, 134, PAGE_ROW1_Y + 43, 2);
    lastSignalText = s;
  }

  String ip = ipText();
  if (ip != lastIpText) {
    tft.fillRect(18, PAGE_ROW2_Y + 30, 200, 18, COL_PANEL);
    tft.setTextColor(COL_DIM, COL_PANEL);
    tft.drawString("IP address", 18, PAGE_ROW2_Y + 8, 2);
    tft.setTextColor(COL_TEXT, COL_PANEL);
    tft.drawString(ip, 18, PAGE_ROW2_Y + 30, 2);
    lastIpText = ip;
  }

  String up = uptimeText();
  String upCombined = up + "|" + lastSyncText();
  if (upCombined != lastUptimeText) {
    tft.fillRect(18, PAGE_ROW3_Y + 26, 88, 26, COL_PANEL);
    tft.setTextColor(COL_DIM, COL_PANEL);
    tft.drawString("Uptime", 18, PAGE_ROW3_Y + 10, 2);
    tft.setTextColor(COL_TEXT, COL_PANEL);
    tft.drawString(up, 18, PAGE_ROW3_Y + 26, 2);
    tft.setTextColor(COL_DIM, COL_PANEL);
    tft.drawString(lastSyncText(), 18, PAGE_ROW3_Y + 42, 1);
    lastUptimeText = upCombined;
  }

  String networkLabel = wifiEnabled ? "Enabled" : "Disabled";
  if (networkLabel != lastNetworkToggleText) {
    uint16_t btnBg = wifiEnabled ? COL_ACCENT : COL_PANEL_ALT;
    uint16_t btnFg = wifiEnabled ? TFT_BLACK : COL_TEXT;
    uint16_t btnStroke = wifiEnabled ? COL_ACCENT : COL_STROKE;

    tft.fillRect(132, PAGE_ROW3_Y + 8, 92, 42, COL_PANEL);
    tft.setTextColor(COL_DIM, COL_PANEL);
    tft.drawString("Network", 134, PAGE_ROW3_Y + 10, 2);
    tft.fillRoundRect(134, PAGE_ROW3_Y + 30, 88, 22, 8, btnBg);
    tft.drawRoundRect(134, PAGE_ROW3_Y + 30, 88, 22, 8, btnStroke);
    tft.setTextColor(btnFg, btnBg);
    tft.drawCentreString(networkLabel.c_str(), 178, PAGE_ROW3_Y + 32, 2);
    lastNetworkToggleText = networkLabel;
  }
}

#include "WeatherRadar.h"

void drawCurrentPageFull() {
  // The reserved flight canvas also supplies PNG's dictionary off-page.
  switch (currentPage) {
    case PAGE_HOME:    drawHomePageFull(); break;
    case PAGE_STOCKS: drawStocksPageFull(); break;
    case PAGE_RADAR:   drawRadarPageFull(); break;
    case PAGE_STATUS:  drawStatusPageFull(); break;
    case PAGE_WEATHER_RADAR: drawWeatherRadarPageFull(); break;
  }

  if (focusMenuOpen && currentPage == PAGE_HOME) drawFocusMenuOverlay(true);
  if (timerDoneDialogOpen) drawTimerDoneOverlay(true);
}

void updateCurrentPageDynamic() {
  if (timerDoneDialogOpen) {
    drawTimerDoneOverlay(false);
    return;
  }

  if (focusMenuOpen && currentPage == PAGE_HOME) {
    drawFocusMenuOverlay(false);
    return;
  }

  switch (currentPage) {
    case PAGE_HOME:    updateHomeDynamic(); break;
    case PAGE_STOCKS: updateStocksDynamic(); break;
    case PAGE_RADAR:   updateRadarDynamic(); break;
    case PAGE_STATUS:  updateStatusDynamic(); break;
    case PAGE_WEATHER_RADAR: updateWeatherRadarDynamic(); break;
  }
}

bool handleFocusMenuTouch(int x, int y) {
  if (!focusMenuOpen) return false;

  if (x < TIMER_MENU_X || x >= TIMER_MENU_X + TIMER_MENU_W || y < TIMER_MENU_Y || y >= TIMER_MENU_Y + TIMER_MENU_H) {
    focusMenuOpen = false;
    cacheTimerMenu = "";
    pageDirty = true;
    return true;
  }

  struct ButtonHit {
    int x;
    int y;
    int w;
    int h;
    int minutes;
  };

  const ButtonHit buttons[] = {
    {34, 124, 74, 28, timerPresetMin[0]},
    {132, 124, 74, 28, timerPresetMin[1]},
    {34, 158, 74, 28, timerPresetMin[2]},
    {132, 158, 74, 28, timerPresetMin[3]},
    {34, 192, 74, 28, timerPresetMin[4]},
    {132, 192, 74, 28, timerPresetMin[5]}
  };

  for (const ButtonHit& btn : buttons) {
    if (x >= btn.x && x < btn.x + btn.w && y >= btn.y && y < btn.y + btn.h) {
      startFocusTimer(btn.minutes);
      pageDirty = true;
      return true;
    }
  }

  if ((focusTimerRunning || focusTimerFinished) && x >= 44 && x < 196 && y >= 224 && y < 250) {
    if (focusTimerRunning || focusTimerFinished) {
      resetFocusTimer();
    } else {
      focusMenuOpen = false;
      cacheTimerMenu = "";
    }
    pageDirty = true;
    return true;
  }

  return true;
}

bool handleHomeTouch(int x, int y) {
  if (currentPage != PAGE_HOME) return false;

  if (focusMenuOpen) return handleFocusMenuTouch(x, y);

  for (int slot = 0; slot < HOME_SLOT_COUNT; slot++) {
    if (homeWidgetSlots[slot] != HOME_WIDGET_TIMER) continue;

    int slotX, slotY, slotW, slotH;
    getHomeSlotRect(slot, slotX, slotY, slotW, slotH);

    if (x >= slotX && x < slotX + slotW && y >= slotY && y < slotY + slotH) {
      if (focusTimerFinished) {
        resetFocusTimer();
      } else {
        focusMenuOpen = true;
        cacheFocusTimer = "";
        clearHomeSlotCaches();
        cacheTimerMenu = "";
      }
      pageDirty = true;
      return true;
    }
  }

  return false;
}

bool handleTimerDoneDialogTouch(int x, int y) {
  (void)x;
  (void)y;
  if (!timerDoneDialogOpen) return false;
  dismissTimerDoneDialog();
  return true;
}

// Charts have a fixed Dow/Nasdaq layout; content taps do not change the page.
bool handleStocksTouch(int x, int y) {
  return currentPage == PAGE_STOCKS && x >= 0 && x < SCREEN_W &&
    y > TOPBAR_H && y < SCREEN_H - NAV_H;
}
bool handleStatusTouch(int x, int y) {
  if (currentPage != PAGE_STATUS) return false;
  if (x >= 124 && x < 232 && y >= PAGE_ROW1_Y && y < PAGE_ROW1_Y + PAGE_WIDGET_H) {
    runTouchCalibration();
    return true;
  }
  if (x >= 8 && x < 116 && y >= PAGE_ROW1_Y && y < PAGE_ROW1_Y + PAGE_WIDGET_H) {
    openWifiSetup();
    return true;
  }

  if (x >= 124 && x < 232 && y >= 198 && y < 268) {
    setWifiEnabled(!wifiEnabled);
    return true;
  }

  return false;
}

// =========================================================
// NAVIGATION
// =========================================================
void handleNavTouch(int x, int y) {
  if (y < SCREEN_H - NAV_H) return;

  int btnW = SCREEN_W / 4;
  int idx = x / btnW;
  if (idx < 0 || idx > 3) return;

  Page newPage = idx == 3 ? PAGE_WEATHER_RADAR : (Page)idx;
  if (newPage != currentPage) {
    currentPage = newPage;
    pageDirty = true;
  }
}

// =========================================================
// WEB SERVER
// =========================================================
void handleRoot() {
  String accent = prefs.getString("accent", "cyan");
  String bg     = prefs.getString("bg", "slate");
  String txt    = prefs.getString("text", "standard");
  String units  = prefs.getString("units", "metric");
  String region = prefs.getString("region", "europe");
  String tz     = sanitizeTimezoneKey(prefs.getString("tz", "europe_central"));
  String nickname = prefs.getString("nickname", "");
  bool flashMode = prefs.getBool("flashMode", false);
  String homeSlotKeys[HOME_SLOT_COUNT];
  for (int i = 0; i < HOME_SLOT_COUNT; i++) {
    homeSlotKeys[i] = homeWidgetKey(homeWidgetSlots[i]);
  }

  String page;
  page.reserve(21000);

  page += "<!doctype html><html><head>";
  page += "<meta charset='utf-8'>";
  page += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  page += "<title>Deskbuddy</title>";
  page += "<style>";
  page += ":root{color-scheme:dark;}";
  page += "body{margin:0;background:linear-gradient(180deg,#0b1018 0%,#111827 100%);color:#edf2f7;font-family:system-ui,sans-serif;}";
  page += ".wrap{max-width:980px;margin:0 auto;padding:28px 16px 36px;}";
  page += ".hero{margin-bottom:18px;padding:18px 20px;border:1px solid #243244;border-radius:20px;background:linear-gradient(135deg,#111927 0%,#172235 100%);box-shadow:0 10px 30px rgba(0,0,0,.22);}";
  page += ".hero h1{font-size:30px;margin:0 0 8px 0;}";
  page += ".hero p{margin:0;color:#a9b7c9;font-size:14px;}";
  page += ".ip{display:inline-block;margin-top:14px;padding:8px 12px;border-radius:999px;background:#0b1220;border:1px solid #334155;color:#dbe7f5;font-size:13px;}";
  page += ".layout{display:grid;grid-template-columns:1.15fr .85fr;gap:16px;align-items:start;}";
  page += ".stack{display:grid;gap:16px;}";
  page += ".panel{background:#171b22;border:1px solid #2d3748;border-radius:18px;padding:18px;margin:0;}";
  page += ".panel-toggle{width:100%;display:flex;align-items:center;justify-content:space-between;gap:12px;background:none;border:none;color:#edf2f7;padding:0;margin:0;cursor:pointer;text-align:left;}";
  page += ".panel-toggle:hover{color:#ffffff;}";
  page += ".panel-toggle h2{flex:1;}";
  page += ".panel-chevron{font-size:18px;color:#8ea3ba;transition:transform .18s ease;}";
  page += ".panel.collapsed .panel-chevron{transform:rotate(-90deg);}";
  page += ".panel-body{margin-top:12px;}";
  page += ".panel.collapsed .panel-body{display:none;}";
  page += ".panel h2{margin:0 0 6px 0;font-size:18px;}";
  page += ".panel p{margin:0 0 14px 0;color:#94a3b8;font-size:13px;line-height:1.45;}";
  page += ".grid{display:grid;grid-template-columns:1fr 1fr;gap:14px;}";
  page += ".grid-3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:14px;}";
  page += ".label{display:block;font-size:13px;margin:0 0 8px 0;color:#a0aec0;font-weight:600;}";
  page += "textarea,input,select{width:100%;border-radius:12px;border:1px solid #334155;background:#0b1220;color:#edf2f7;padding:12px;box-sizing:border-box;font:inherit;}";
  page += "textarea{min-height:170px;resize:vertical;}";
  page += "button{margin-top:18px;background:#38bdf8;border:none;color:#001018;padding:13px 18px;border-radius:12px;font-weight:800;cursor:pointer;font:inherit;}";
  page += ".muted{font-size:13px;color:#94a3b8;line-height:1.45;}";
  page += ".footer-note{margin-top:10px;font-size:12px;color:#7f92a8;}";
  page += ".settings-block{margin-top:18px;padding-top:16px;border-top:1px solid #2b3545;}";
  page += ".settings-block:first-of-type{margin-top:0;padding-top:0;border-top:none;}";
  page += ".settings-title{display:block;margin:0 0 6px 0;font-size:14px;font-weight:700;color:#edf2f7;letter-spacing:.02em;}";
  page += ".settings-desc{margin:0 0 12px 0;font-size:12px;color:#8ea3ba;line-height:1.45;}";
  page += ".color-stack{display:grid;gap:12px;}";
  page += ".color-row{display:grid;grid-template-columns:120px 1fr;gap:12px;align-items:center;}";
  page += ".color-meta{display:flex;align-items:center;justify-content:space-between;gap:10px;}";
  page += ".color-meta .label{margin:0;color:#dbe7f5;}";
  page += ".color-value{font-size:12px;color:#8ea3ba;white-space:nowrap;}";
  page += ".swatch-row{display:flex;flex-wrap:wrap;gap:8px;}";
  page += ".swatch{width:22px;height:22px;border-radius:999px;border:1px solid rgba(255,255,255,.18);cursor:pointer;position:relative;box-sizing:border-box;}";
  page += ".swatch input{display:none;}";
  page += ".swatch.active{box-shadow:0 0 0 2px #67e8f9, 0 0 0 5px rgba(103,232,249,.18);}";
  page += ".swatch.active::after{content:'';position:absolute;inset:5px;border-radius:999px;border:1px solid rgba(0,16,24,.45);}";
  page += ".timer-slot-grid{display:grid;grid-template-columns:1fr 1fr 1fr;gap:10px;margin-top:14px;}";
  page += ".timer-slot{border:1px solid #334155;border-radius:12px;background:#0b1220;padding:10px 10px 12px 10px;}";
  page += ".timer-slot-head{font-size:12px;color:#8ea3ba;margin-bottom:8px;font-weight:600;}";
  page += ".timer-slot-input{display:flex;align-items:center;gap:8px;}";
  page += ".timer-slot input{padding:10px 12px;text-align:center;font-weight:700;}";
  page += ".timer-unit{font-size:12px;color:#8ea3ba;white-space:nowrap;}";
  page += "@media(max-width:820px){.layout{grid-template-columns:1fr;}.grid,.grid-3,.timer-slot-grid{grid-template-columns:1fr;}.color-row{grid-template-columns:1fr;}}";
  page += "</style></head><body><div class='wrap'>";
  page += "<div class='hero'>";
  page += "<h1>Deskbuddy</h1>";
  page += "<p>Shape Deskbuddy into your own desk companion with widgets, notes, colors, and smart daily tools.</p>";
  page += "<div class='ip'>ESP IP: ";
  page += WiFi.localIP().toString();
  page += "</div></div>";

  page += "<form method='POST' action='/save'>";
  page += "<div class='layout'><div class='stack'>";

  page += "<div class='panel' data-panel='notes'>";
  page += "<button type='button' class='panel-toggle' aria-expanded='true'><h2>Notes</h2><span class='panel-chevron'>&#9662;</span></button>";
  page += "<div class='panel-body'>";
  page += "<p>Short notes synced to the device.</p>";
  page += "<label class='label'>Notes</label>";
  page += "<textarea name='notes' maxlength='700'>";
  page += htmlEscape(notesText);
  page += "</textarea>";
  page += "<div class='muted'>Saved notes show up right away.</div>";
  page += "</div></div>";

  page += "<div class='panel' data-panel='theme'>";
  page += "<button type='button' class='panel-toggle' aria-expanded='true'><h2>Theme and color</h2><span class='panel-chevron'>&#9662;</span></button>";
  page += "<div class='panel-body'>";
  page += "<p>Colors and visual style for the display.</p>";
  page += "<div class='grid'>";

  page += "<div style='grid-column:1 / -1;' class='color-stack'>";

  page += "<div class='color-row'><div class='color-meta'><label class='label'>Accent</label><span class='color-value' id='accent-value'>";
  page += accent;
  page += "</span></div><div class='swatch-row'>";
  page += "<label class='swatch" + String(accent=="standard"?" active":"") + "' style='background:" + accentPreviewCss("standard") + ";'><input type='radio' name='accent' value='standard'" + String(accent=="standard"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(accent=="ice"?" active":"") + "' style='background:" + accentPreviewCss("ice") + ";'><input type='radio' name='accent' value='ice'" + String(accent=="ice"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(accent=="white"?" active":"") + "' style='background:" + accentPreviewCss("white") + ";'><input type='radio' name='accent' value='white'" + String(accent=="white"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(accent=="cyan"?" active":"") + "' style='background:" + accentPreviewCss("cyan") + ";'><input type='radio' name='accent' value='cyan'" + String(accent=="cyan"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(accent=="mint"?" active":"") + "' style='background:" + accentPreviewCss("mint") + ";'><input type='radio' name='accent' value='mint'" + String(accent=="mint"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(accent=="green"?" active":"") + "' style='background:" + accentPreviewCss("green") + ";'><input type='radio' name='accent' value='green'" + String(accent=="green"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(accent=="blue"?" active":"") + "' style='background:" + accentPreviewCss("blue") + ";'><input type='radio' name='accent' value='blue'" + String(accent=="blue"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(accent=="purple"?" active":"") + "' style='background:" + accentPreviewCss("purple") + ";'><input type='radio' name='accent' value='purple'" + String(accent=="purple"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(accent=="pink"?" active":"") + "' style='background:" + accentPreviewCss("pink") + ";'><input type='radio' name='accent' value='pink'" + String(accent=="pink"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(accent=="orange"?" active":"") + "' style='background:" + accentPreviewCss("orange") + ";'><input type='radio' name='accent' value='orange'" + String(accent=="orange"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(accent=="amber"?" active":"") + "' style='background:" + accentPreviewCss("amber") + ";'><input type='radio' name='accent' value='amber'" + String(accent=="amber"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(accent=="red"?" active":"") + "' style='background:" + accentPreviewCss("red") + ";'><input type='radio' name='accent' value='red'" + String(accent=="red"?" checked":"") + "></label>";
  page += "</div></div>";

  page += "<div class='color-row'><div class='color-meta'><label class='label'>Text</label><span class='color-value' id='text-value'>";
  page += txt;
  page += "</span></div><div class='swatch-row'>";
  page += "<label class='swatch" + String(txt=="standard"?" active":"") + "' style='background:" + accentPreviewCss("standard") + ";'><input type='radio' name='text' value='standard'" + String(txt=="standard"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(txt=="ice"?" active":"") + "' style='background:" + accentPreviewCss("ice") + ";'><input type='radio' name='text' value='ice'" + String(txt=="ice"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(txt=="white"?" active":"") + "' style='background:" + accentPreviewCss("white") + ";'><input type='radio' name='text' value='white'" + String(txt=="white"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(txt=="cyan"?" active":"") + "' style='background:" + accentPreviewCss("cyan") + ";'><input type='radio' name='text' value='cyan'" + String(txt=="cyan"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(txt=="mint"?" active":"") + "' style='background:" + accentPreviewCss("mint") + ";'><input type='radio' name='text' value='mint'" + String(txt=="mint"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(txt=="green"?" active":"") + "' style='background:" + accentPreviewCss("green") + ";'><input type='radio' name='text' value='green'" + String(txt=="green"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(txt=="blue"?" active":"") + "' style='background:" + accentPreviewCss("blue") + ";'><input type='radio' name='text' value='blue'" + String(txt=="blue"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(txt=="purple"?" active":"") + "' style='background:" + accentPreviewCss("purple") + ";'><input type='radio' name='text' value='purple'" + String(txt=="purple"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(txt=="pink"?" active":"") + "' style='background:" + accentPreviewCss("pink") + ";'><input type='radio' name='text' value='pink'" + String(txt=="pink"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(txt=="orange"?" active":"") + "' style='background:" + accentPreviewCss("orange") + ";'><input type='radio' name='text' value='orange'" + String(txt=="orange"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(txt=="amber"?" active":"") + "' style='background:" + accentPreviewCss("amber") + ";'><input type='radio' name='text' value='amber'" + String(txt=="amber"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(txt=="red"?" active":"") + "' style='background:" + accentPreviewCss("red") + ";'><input type='radio' name='text' value='red'" + String(txt=="red"?" checked":"") + "></label>";
  page += "</div></div>";

  page += "<div class='color-row'><div class='color-meta'><label class='label'>Theme</label><span class='color-value' id='bg-value'>";
  page += bg;
  page += "</span></div><div class='swatch-row'>";
  page += "<label class='swatch" + String(bg=="slate"?" active":"") + "' style='background:" + themePreviewCss("slate") + ";'><input type='radio' name='bg' value='slate'" + String(bg=="slate"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(bg=="deep"?" active":"") + "' style='background:" + themePreviewCss("deep") + ";'><input type='radio' name='bg' value='deep'" + String(bg=="deep"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(bg=="nordic"?" active":"") + "' style='background:" + themePreviewCss("nordic") + ";'><input type='radio' name='bg' value='nordic'" + String(bg=="nordic"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(bg=="forest"?" active":"") + "' style='background:" + themePreviewCss("forest") + ";'><input type='radio' name='bg' value='forest'" + String(bg=="forest"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(bg=="coffee"?" active":"") + "' style='background:" + themePreviewCss("coffee") + ";'><input type='radio' name='bg' value='coffee'" + String(bg=="coffee"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(bg=="soft"?" active":"") + "' style='background:" + themePreviewCss("soft") + ";'><input type='radio' name='bg' value='soft'" + String(bg=="soft"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(bg=="midnight"?" active":"") + "' style='background:" + themePreviewCss("midnight") + ";'><input type='radio' name='bg' value='midnight'" + String(bg=="midnight"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(bg=="graphite"?" active":"") + "' style='background:" + themePreviewCss("graphite") + ";'><input type='radio' name='bg' value='graphite'" + String(bg=="graphite"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(bg=="garnet"?" active":"") + "' style='background:" + themePreviewCss("garnet") + ";'><input type='radio' name='bg' value='garnet'" + String(bg=="garnet"?" checked":"") + "></label>";
  page += "<label class='swatch" + String(bg=="ochre"?" active":"") + "' style='background:" + themePreviewCss("ochre") + ";'><input type='radio' name='bg' value='ochre'" + String(bg=="ochre"?" checked":"") + "></label>";
  page += "</div></div>";

  page += "</div>";

  page += "</div></div></div>";

  page += "<div class='panel' data-panel='settings'>";
  page += "<button type='button' class='panel-toggle' aria-expanded='true'><h2>Settings</h2><span class='panel-chevron'>&#9662;</span></button>";
  page += "<div class='panel-body'>";
  page += "<p>Core behavior and timer setup.</p>";
  page += "<div class='settings-block'>";
  page += "<span class='settings-title'>Stocks</span>";
  page += "<div class='muted'>Dow Jones on top, Nasdaq Composite below. Charts show changes from each day's opening value and refresh every minute during the regular U.S. trading session. Time marks are Eastern Time. When the market is closed, the page shows Market Closed.</div>";
  page += "</div><div class='settings-block'>";
  page += "<span class='settings-title'>General</span>";
  page += "<div class='grid'>";
  page += "<div><label class='label'>Buddy nickname</label><input name='nickname' maxlength='24' value='" + htmlEscape(nickname) + "'></div>";
  page += "<div><label class='label'>Auto sleep interval</label><select name='sleepMin'>";
  page += "<option value='0'"  + String(sleepIntervalMin==0?" selected":"")  + ">Never</option>";
  page += "<option value='1'"  + String(sleepIntervalMin==1?" selected":"")  + ">1 minute</option>";
  page += "<option value='5'"  + String(sleepIntervalMin==5?" selected":"")  + ">5 minutes</option>";
  page += "<option value='10'" + String(sleepIntervalMin==10?" selected":"") + ">10 minutes</option>";
  page += "<option value='30'" + String(sleepIntervalMin==30?" selected":"") + ">30 minutes</option>";
  page += "<option value='60'" + String(sleepIntervalMin==60?" selected":"") + ">1 hour</option>";
  page += "</select><div class='muted' style='margin-top:8px;'>Sleep dims the screen first, then turns it fully off after 60 seconds.</div></div>";
  page += "<div><label class='label'>Measurement system</label><select name='units'>";
  page += "<option value='metric'"   + String(units=="metric"?" selected":"")   + ">Celsius / mm</option>";
  page += "<option value='imperial'" + String(units=="imperial"?" selected":"") + ">Fahrenheit / inches</option>";
  page += "</select></div>";
  page += "<div><label class='label'>Date format</label><select name='region'>";
  page += "<option value='europe'" + String(region=="europe"?" selected":"") + ">European: dd.mm.yyyy</option>";
  page += "<option value='us'" + String(region=="us"?" selected":"") + ">US: mm/dd/yyyy</option>";
  page += "</select></div>";
  page += "<div><label class='label'>Time zone</label><select name='tz'>";
  appendTimezoneOptions(page, tz);
  page += "</select></div>";
  page += "</div>";
  page += "</div>";
  page += "<div class='settings-block'><span class='settings-title'>Timer</span><div class='settings-desc'>Choose the six quick timers shown in the popup menu.</div><div class='timer-slot-grid'>";
  for (int i = 0; i < 6; i++) {
    page += "<div class='timer-slot'><div class='timer-slot-head'>Slot " + String(i + 1) + "</div><div class='timer-slot-input'><input type='number' min='1' max='180' name='timer" + String(i) + "' value='" + String(timerPresetMin[i]) + "'><span class='timer-unit'>min</span></div></div>";
  }
  page += "</div>";
  page += "<div style='margin-top:14px;'><span class='settings-title'>Alert behavior</span><label style='display:flex;align-items:center;gap:10px;color:#edf2f7;'><input type='checkbox' name='flashMode' value='1'" + String(flashMode ? " checked" : "") + " style='width:auto;'>Flash screen when timer ends</label></div></div>";
  page += "<div class='settings-block'><span class='settings-title'>Location</span><div class='settings-desc'>Used for weather data and sun times.</div><div class='grid-3'>";
  page += "<div><label class='label'>Location name</label><input name='locname' value='" + htmlEscape(locationName) + "'></div>";
  page += "<div><label class='label'>Latitude</label><input name='lat' value='" + String(LAT, 6) + "'></div>";
  page += "<div><label class='label'>Longitude</label><input name='lng' value='" + String(LNG, 6) + "'></div>";
  page += "</div><div class='footer-note'>Example Berlin: latitude 52.5200, longitude 13.4050.</div></div>";
  page += "</div></div>";

  page += "<div class='panel' data-panel='widgets'>";
  page += "<button type='button' class='panel-toggle' aria-expanded='true'><h2>Widget Customization</h2><span class='panel-chevron'>&#9662;</span></button>";
  page += "<div class='panel-body'>";
  page += "<p>Choose which widgets appear in the four Home slots below the clock card.</p>";
  page += "<div class='grid'>";
  for (int i = 0; i < HOME_SLOT_COUNT; i++) {
    page += "<div><label class='label'>";
    page += homeSlotLabel(i);
    page += "</label><select name='homeSlot";
    page += String(i);
    page += "'>";
    appendHomeWidgetOptions(page, homeSlotKeys[i]);
    page += "</select></div>";
  }
  page += "</div>";
  page += "</div></div>";

  page += "</div><div class='stack'>";

  page += "<button type='submit'>Save to Deskbuddy</button>";
  page += "</div></div></form>";
  page += "<script>";
  page += "var colorNames={accent:{standard:'Standard',ice:'Ice',white:'White',cyan:'Cyan',mint:'Mint',green:'Green',blue:'Blue',purple:'Purple',pink:'Pink',orange:'Orange',amber:'Amber',red:'Red'},text:{standard:'Standard',ice:'Ice',white:'White',cyan:'Cyan',mint:'Mint',green:'Green',blue:'Blue',purple:'Purple',pink:'Pink',orange:'Orange',amber:'Amber',red:'Red'},bg:{slate:'Slate',deep:'Deep black',nordic:'Nordic blue',forest:'Forest',coffee:'Coffee',soft:'Soft dark',midnight:'Midnight',graphite:'Graphite',garnet:'Garnet',ochre:'Ochre'}};";
  page += "var panelStorageKey='deskbuddy-panel-state-v1';";
  page += "document.querySelectorAll('.swatch input').forEach(function(input){";
  page += "input.addEventListener('change',function(){";
  page += "document.querySelectorAll('.swatch input[name=\"'+input.name+'\"]').forEach(function(peer){";
  page += "peer.closest('.swatch').classList.toggle('active', peer.checked);";
  page += "});";
  page += "var valueEl=document.getElementById(input.name+'-value');";
  page += "if(valueEl&&colorNames[input.name]&&colorNames[input.name][input.value]){valueEl.textContent=colorNames[input.name][input.value];}";
  page += "});";
  page += "});";
  page += "function readPanelState(){try{return JSON.parse(localStorage.getItem(panelStorageKey)||'{}');}catch(e){return {};}}";
  page += "function writePanelState(state){localStorage.setItem(panelStorageKey,JSON.stringify(state));}";
  page += "function applyPanelState(panel,collapsed){panel.classList.toggle('collapsed',collapsed);var btn=panel.querySelector('.panel-toggle');if(btn){btn.setAttribute('aria-expanded',collapsed?'false':'true');}}";
  page += "var savedPanelState=readPanelState();";
  page += "document.querySelectorAll('.panel[data-panel]').forEach(function(panel){";
  page += "var panelId=panel.getAttribute('data-panel');";
  page += "if(Object.prototype.hasOwnProperty.call(savedPanelState,panelId)){applyPanelState(panel,!!savedPanelState[panelId]);}";
  page += "});";
  page += "document.querySelectorAll('.panel-toggle').forEach(function(btn){";
  page += "btn.addEventListener('click',function(){";
  page += "var panel=btn.closest('.panel');";
  page += "var collapsed=!panel.classList.contains('collapsed');";
  page += "applyPanelState(panel,collapsed);";
  page += "var state=readPanelState();";
  page += "var panelId=panel.getAttribute('data-panel');";
  page += "if(panelId){state[panelId]=collapsed;writePanelState(state);}";
  page += "});";
  page += "});";
  page += "</script>";
  page += "</div></body></html>";

  server.send(200, "text/html; charset=utf-8", page);
}

void handleSave() {
  String newNotes  = server.hasArg("notes") ? server.arg("notes") : notesText;
  String newAccent = server.hasArg("accent") ? server.arg("accent") : "cyan";
  String newBg     = server.hasArg("bg") ? server.arg("bg") : "slate";
  String newText   = server.hasArg("text") ? server.arg("text") : "standard";
  String newUnits  = server.hasArg("units") ? server.arg("units") : "metric";
  String newRegion = server.hasArg("region") ? server.arg("region") : "europe";
  String newTz     = server.hasArg("tz") ? server.arg("tz") : timezoneKey;
  String newLoc    = server.hasArg("locname") ? server.arg("locname") : locationName;
  String newNickname = server.hasArg("nickname") ? server.arg("nickname") : buddyNickname;
  HomeWidgetType newHomeSlots[HOME_SLOT_COUNT];
  for (int i = 0; i < HOME_SLOT_COUNT; i++) {
    String key = String("homeSlot") + String(i);
    String currentKey = homeWidgetKey(homeWidgetSlots[i]);
    newHomeSlots[i] = homeWidgetFromKey(server.hasArg(key) ? server.arg(key) : currentKey);
  }

  float newLat = server.hasArg("lat") ? server.arg("lat").toFloat() : LAT;
  float newLng = server.hasArg("lng") ? server.arg("lng").toFloat() : LNG;

  newNotes.trim();
  newLoc.trim();
  newNickname.trim();

  if (newNotes.length() == 0) newNotes = "No notes yet.";
  if (newNotes.length() > 700) newNotes = newNotes.substring(0, 700);
  if (newLoc.length() == 0) newLoc = "Unknown";
  if (newNickname.length() > 24) newNickname = newNickname.substring(0, 24);
  if (newUnits != "metric" && newUnits != "imperial") newUnits = "metric";
  if (newRegion != "europe" && newRegion != "us") newRegion = "europe";
  newTz = sanitizeTimezoneKey(newTz);

  int newSleepMin = server.hasArg("sleepMin") ? server.arg("sleepMin").toInt() : sleepIntervalMin;
  sleepIntervalMin = constrain(newSleepMin, 0, 120);
  bool newFlashMode = server.hasArg("flashMode");

  bool locationChanged =
    (fabsf(newLat - LAT) > 0.0001f) ||
    (fabsf(newLng - LNG) > 0.0001f) ||
    (newLoc != locationName);

  notesText = newNotes;
  buddyNickname = newNickname;
  locationName = newLoc;
  LAT = newLat;
  LNG = newLng;
  unitKey = newUnits;
  regionFormatKey = newRegion;
  timezoneKey = newTz;
  flashModeEnabled = newFlashMode;
  for (int i = 0; i < HOME_SLOT_COUNT; i++) {
    homeWidgetSlots[i] = newHomeSlots[i];
  }

  for (int i = 0; i < 6; i++) {
    String key = String("timer") + String(i);
    int currentValue = timerPresetMin[i];
    int nextValue = server.hasArg(key) ? server.arg(key).toInt() : currentValue;
    timerPresetMin[i] = sanitizeTimerMinutes(nextValue);
  }

  prefs.putString("notes", notesText);
  prefs.putString("accent", newAccent);
  prefs.putString("bg", newBg);
  prefs.putString("text", newText);
  prefs.putString("units", unitKey);
  prefs.putString("region", regionFormatKey);
  prefs.putString("tz", timezoneKey);
  prefs.putString("nickname", buddyNickname);
  prefs.putString("locname", locationName);
  prefs.putFloat("lat", LAT);
  prefs.putFloat("lng", LNG);
  prefs.putInt("sleepMin", sleepIntervalMin);
  prefs.putBool("flashMode", flashModeEnabled);
  for (int i = 0; i < HOME_SLOT_COUNT; i++) {
    String key = String("homeSlot") + String(i);
    prefs.putString(key.c_str(), homeWidgetKey(homeWidgetSlots[i]));
  }
  for (int i = 0; i < 6; i++) {
    String key = String("timer") + String(i);
    prefs.putInt(key.c_str(), timerPresetMin[i]);
  }

  applyThemeByKey(newAccent, newBg);
  applyTextColorByKey(newText);
  applyDeviceTimezoneByKey(timezoneKey);
  if (!sleepDimmed && !sleepOff) setBacklight(BL_FULL);

  notesDirty = true;
  pageDirty = true;
  dataDirty = true;

  cacheClock = "";
  cacheHomeEmpty1 = "";
  cacheHomeEmpty2 = "";
  cacheFocusTimer = "";
  cacheTimerMenu = "";
  cacheTimerDone = "";
  for (int i = 0; i < HOME_SLOT_COUNT; i++) {
    cacheHomeSlots[i] = "";
  }

  lastTempText = "";
  lastRainText = "";
  lastKpText = "";
  lastKpLevelText = "";
  lastWindText = "";
  lastWindDirText = "";
  lastNextSunLabel = "";
  lastNextSunTime = "";
  lastUptimeText = "";

  if (locationChanged) resetDataCaches();

  server.sendHeader("Location", "/");
  server.send(303);
}

void setupWebServer() {
  server.on("/weather/status", HTTP_GET, []() {
    JsonDocument doc;
    doc["pressure_hpa"] = pressureHpa; doc["pressure_inhg"] = pressureHpa / 33.8638866667f;
    doc["pressure_text"] = pressureValueText(); doc["pressure_unit"] = pressureUnitText();
    doc["pressure_delta_hpa"] = pressureDeltaHpa; doc["pressure_trend"] = pressureTrendText();
    doc["pressure_trend_hours"] = 3; doc["updated"] = lastWeatherFetch;
    JsonArray slots = doc["home_widgets"].to<JsonArray>();
    for (int i = 0; i < HOME_SLOT_COUNT; ++i) slots.add(homeWidgetKey(homeWidgetSlots[i]));
    String body; serializeJson(doc, body); server.send(200, "application/json", body);
  });
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
}

// =========================================================
// SETUP / LOOP
// =========================================================
void waitForNtpTime() {
  time_t now = time(nullptr);
  unsigned long t0 = millis();
  while (now < 1700000000 && millis() - t0 < 10000) {
    delay(200);
    now = time(nullptr);
  }
}

// Touchscreen WiFi recovery. Scanning is asynchronous to keep touch responsive.
void wifiSetupButton(int x, int y, int w, int h, const String& label) {
  tft.fillRoundRect(x, y, w, h, 3, COL_PANEL_ALT);
  tft.drawRoundRect(x, y, w, h, 3, COL_STROKE);
  tft.setTextColor(COL_TEXT, COL_PANEL_ALT);
  tft.drawCentreString(label, x + w / 2, y + (h - 8) / 2, 1);
}

void openWifiSetup() {
  if (wifiScanning || wifiSetupConnecting) return;
  wifiSetupOpen = true;
  wifiPasswordEntry = false;
  pendingWifiPass = "";
  wifiNetworkOffset = 0;
  wifiSetupDirty = true;
  wifiEnabled = true;
  prefs.putBool("wifiEnabled", true);
  wifiConnectInProgress = false;
  wakeDisplay();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(false, false);
  WiFi.scanDelete();
  wifiSetupMessage = "Searching nearby networks...";
  int result = WiFi.scanNetworks(true);
  wifiScanning = result == WIFI_SCAN_RUNNING;
  if (!wifiScanning) wifiSetupMessage = result >= 0 ? "Select a network" : "Scan failed. Tap Rescan.";
}

const char* wifiKeyboardRow(int row) {
  static const char* lower[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
  static const char* upper[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
  static const char* symbols[] = {"!@#$%^&*()", "-_=+[]{};:", "'\"\\|,<.>/?", "`~"};
  return wifiKeyboardSymbols ? symbols[row] : (wifiKeyboardShift ? upper[row] : lower[row]);
}

void drawWifiSetup() {
  tft.fillScreen(COL_BG);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawCentreString("WiFi setup", 120, 6, 2);
  tft.drawString(wifiSetupMessage.substring(0, 38), 6, 30, 1);
  if (wifiPasswordEntry) {
    tft.drawString(pendingWifiSsid.substring(0, 28), 6, 48, 2);
    // Show the entire password across two lines, including long passphrases.
    tft.drawString(pendingWifiPass.substring(0, 38), 6, 73, 1);
    tft.drawString(pendingWifiPass.substring(38), 6, 85, 1);
    for (int r = 0; r < 4; ++r) {
      const char* keys = wifiKeyboardRow(r);
      for (int c = 0; keys[c]; ++c)
        wifiSetupButton(c * 24, 100 + r * 32, 23, 30, String(keys[c]));
    }
    wifiSetupButton(0, 230, 58, 32, "Shift");
    wifiSetupButton(60, 230, 58, 32, wifiKeyboardSymbols ? "ABC" : "Symbols");
    wifiSetupButton(120, 230, 58, 32, "Space");
    wifiSetupButton(180, 230, 59, 32, "Delete");
    wifiSetupButton(0, 278, 116, 36, "Back");
    wifiSetupButton(122, 278, 117, 36, wifiSetupConnecting ? "Connecting..." : "Connect");
  } else {
    int count = WiFi.scanComplete();
    if (!wifiScanning && count == 0) {
      tft.drawString("No networks found. Tap Rescan.", 6, 60, 1);
    }
    for (int r = 0; !wifiScanning && r < 5 && wifiNetworkOffset + r < count; ++r) {
      int i = wifiNetworkOffset + r;
      String name = WiFi.SSID(i);
      if (!name.length()) name = "(hidden network)";
      wifiSetupButton(4, 52 + r * 37, 232, 34,
        name.substring(0, 25) + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? " [open]" : " [lock]"));
    }
    wifiSetupButton(0, 242, 116, 30, "Previous");
    wifiSetupButton(122, 242, 117, 30, "Next");
    wifiSetupButton(0, 280, 116, 34, "Close");
    wifiSetupButton(122, 280, 117, 34, "Rescan");
  }
  wifiSetupDirty = false;
}

void handleWifiSetupTouch(int x, int y) {
  if (wifiSetupConnecting) return;
  if (wifiPasswordEntry) {
    if (y >= 100 && y < 228) {
      int r = (y - 100) / 32, c = x / 24;
      const char* keys = wifiKeyboardRow(r);
      if (c >= 0 && c < (int)strlen(keys) && pendingWifiPass.length() < 64)
        pendingWifiPass += keys[c];
    } else if (y >= 230 && y < 262) {
      if (x < 60) wifiKeyboardShift = !wifiKeyboardShift;
      else if (x < 120) wifiKeyboardSymbols = !wifiKeyboardSymbols;
      else if (x < 180 && pendingWifiPass.length() < 64) pendingWifiPass += ' ';
      else if (x >= 180 && pendingWifiPass.length()) pendingWifiPass.remove(pendingWifiPass.length() - 1);
    } else if (y >= 278) {
      if (x < 120) { openWifiSetup(); }
      else {
        WiFi.scanDelete();
        WiFi.begin(pendingWifiSsid.c_str(), pendingWifiPass.c_str());
        wifiConnectInProgress = true;
        wifiSetupConnecting = true;
        wifiConnectStartedMs = millis();
        wifiSetupMessage = "Connecting (up to 15 seconds)...";
      }
    }
  } else if (y >= 280) {
    if (x < 120) {
      if (wifiScanning) return; // Let the active scan complete before deleting results.
      WiFi.scanDelete();
      wifiSetupOpen = false;
      WiFi.setAutoReconnect(true);
      beginWiFiConnect();
      pageDirty = true;
    } else openWifiSetup();
  } else if (!wifiScanning) {
    int count = WiFi.scanComplete();
    if (y >= 242 && y < 272) {
      if (x < 120) wifiNetworkOffset = max(0, wifiNetworkOffset - 5);
      else if (wifiNetworkOffset + 5 < count) wifiNetworkOffset += 5;
    } else if (y >= 52 && y < 237) {
      int i = wifiNetworkOffset + (y - 52) / 37;
      if (i < count && WiFi.SSID(i).length()) {
        pendingWifiSsid = WiFi.SSID(i);
        pendingWifiPass = "";
        wifiPasswordEntry = true;
        wifiKeyboardShift = false;
        wifiKeyboardSymbols = false;
        wifiSetupMessage = WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "Open network: tap Connect" : "Enter password, then Connect";
      }
    }
  }
  wifiSetupDirty = true;
}

void beginWiFiConnect() {
  if (!wifiEnabled) {
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    wifiConnectInProgress = false;
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(savedWifiSsid.c_str(), savedWifiPass.c_str());
  wifiConnectInProgress = true;
  wifiConnectStartedMs = millis();
}

void connectWiFi(bool waitForConnection = true) {
  beginWiFiConnect();
  if (!waitForConnection) return;

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(200);
  }
  wifiConnectInProgress = false;
  if (wifiEnabled && WiFi.status() != WL_CONNECTED) openWifiSetup();
}

void updateWiFiConnectionState() {
  if (!wifiEnabled) return;
  if (!wifiConnectInProgress) {
    // Give automatic reconnection 15 seconds before offering recovery.
    static unsigned long offlineSince = 0;
    if (wifiSetupOpen || WiFi.status() == WL_CONNECTED) offlineSince = 0;
    else if (offlineSince == 0) offlineSince = millis();
    else if (millis() - offlineSince >= WIFI_CONNECT_TIMEOUT_MS) {
      offlineSince = 0;
      openWifiSetup();
    }
    return;
  }

  wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    wifiConnectInProgress = false;
    if (wifiSetupConnecting) {
      // Retain the previous working credentials until the new connection succeeds.
      savedWifiSsid = pendingWifiSsid;
      savedWifiPass = pendingWifiPass;
      prefs.putString("wifiSsid", savedWifiSsid);
      prefs.putString("wifiPass", savedWifiPass);
      pendingWifiPass = "";
      wifiSetupConnecting = false;
      wifiSetupOpen = false;
      WiFi.setAutoReconnect(true);
      wifiPasswordEntry = false;
    }
    ensureSunTimesForToday();
    ensureWeather();
    ensureKpIndex();
    dataDirty = true;
    pageDirty = true;
    return;
  }

  if (millis() - wifiConnectStartedMs >= WIFI_CONNECT_TIMEOUT_MS) {
    wifiConnectInProgress = false;
    if (wifiSetupConnecting) {
      wifiSetupConnecting = false;
      WiFi.disconnect(false, false);
      wifiSetupMessage = "Failed. Check password / retry.";
      wifiSetupDirty = true;
    } else {
      openWifiSetup();
    }
    pageDirty = true;
  }
}

void setWifiEnabled(bool enabled) {
  wifiEnabled = enabled;
  prefs.putBool("wifiEnabled", wifiEnabled);

  if (!wifiEnabled) {
    wifiConnectInProgress = false;
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
  } else {
    beginWiFiConnect();
  }

  dataDirty = true;
  pageDirty = true;
}

void setup() {
  networkFetchMutex = xSemaphoreCreateMutex();
  Serial.begin(115200);
  Serial.printf("Deskbuddy reset reason: %d; free heap: %u; largest block: %u\n",
                (int)esp_reset_reason(), (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMaxAllocHeap());

  pinMode(BACKLIGHT_PIN, OUTPUT);
  analogWrite(BACKLIGHT_PIN, BL_FULL);
  pinMode(27, OUTPUT);
  digitalWrite(27, HIGH);

  loadStoredSettings();
  setBacklight(BL_FULL);
  lastInteractionMs = millis();

  delay(200);

  tft.init();
  tft.setRotation(ROT);
  tft.invertDisplay(INV);
  tft.setSwapBytes(true);
  initRadarWorkspace(); // Reserve the shared 49 KiB block before Wi-Fi fragments heap.

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Booting Deskbuddy...", 10, 10, 2);

  touchSPI.begin(T_SCK, T_MISO, T_MOSI);
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);
  ts.begin(touchSPI);
  ts.setRotation(ROT);

  tft.fillScreen(COL_BG);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawCentreString("Hold screen to calibrate", SCREEN_W / 2, 130, 2);
  bool calibrationRequested = !touchCalValid;
  unsigned long calibrationWindow = millis(), heldSince = 0;
  while (touchCalValid && millis() - calibrationWindow < 2500UL) {
    if (ts.touched()) {
      if (!heldSince) heldSince = millis();
      if (millis() - heldSince >= 1000UL) { calibrationRequested = true; break; }
    } else heldSince = 0;
    delay(10);
  }
  if (calibrationRequested) runTouchCalibration();
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  tft.drawString("Connecting WiFi...", 10, 34, 2);
  connectWiFi(true);

  tft.drawString("Syncing time...", 10, 58, 2);
  configTzTime(timezonePosixByKey(timezoneKey),
               "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  if (WiFi.status() == WL_CONNECTED) waitForNtpTime();

  ensureSunTimesForToday();
  ensureWeather();
  ensureKpIndex();

  setupWebServer();
  initRadar();
  initWeatherRadar();
  if (xTaskCreatePinnedToCore(stocksNetworkTask, "StocksNetwork", 12288, nullptr, 1, &stocksTaskHandle, 0) != pdPASS) {
    Serial.println("Stock network task could not start");
  }

  pageDirty = true;
  dataDirty = true;
  notesDirty = true;

  if (wifiSetupOpen) drawWifiSetup();
  else { drawCurrentPageFull(); updateCurrentPageDynamic(); }

  lastClockTick = millis();
  lastDataTick = millis();

  Serial.print("Deskbuddy web: http://");
  Serial.println(WiFi.localIP());
}

void loop() {
  server.handleClient();
  weatherRadarService();
  updateWiFiConnectionState();
  updateFocusTimerState();
  updateTimerDoneDialogState();
  if (wifiSetupOpen) {
    lastInteractionMs = millis();
    if (wifiScanning) {
      int result = WiFi.scanComplete();
      if (result != WIFI_SCAN_RUNNING) {
        wifiScanning = false;
        wifiSetupMessage = result >= 0 ? "Select a network" : "Scan failed. Tap Rescan.";
        wifiSetupDirty = true;
      }
    }
    int wx = 0, wy = 0;
    if (touchNewPress(wx, wy)) handleWifiSetupTouch(wx, wy);
    if (wifiSetupOpen && wifiSetupDirty) drawWifiSetup();
    delay(10);
    return;
  }
  handleAutoSleep();

  int tx = 0, ty = 0;
  if (touchNewPress(tx, ty)) {
    lastInteractionMs = millis();

    if (sleepOff) {
      if (manualDimMode) {
        sleepOff = false;
        sleepDimmed = true;
        setBacklight(BL_DIM);
        pageDirty = true;
      } else {
        wakeDisplay();
      }
      return;
    }

    if (handleTimerDoneDialogTouch(tx, ty)) {
      return;
    }

    if ((currentPage == PAGE_HOME || currentPage == PAGE_STATUS) &&
        tx >= 172 && tx < 201 && ty >= 5 && ty < 29) {
      currentPage = (currentPage == PAGE_HOME) ? PAGE_STATUS : PAGE_HOME;
      pageDirty = true;
    } else if (tx >= SCREEN_W - 36 && ty <= TOPBAR_H) {
      toggleSleepMode();
      pageDirty = true;
    } else {
      if (sleepDimmed) {
        if (!manualDimMode) {
          wakeDisplay();
        } else {
          if (!handleHomeTouch(tx, ty) && !handleStocksTouch(tx, ty) && !handleStatusTouch(tx, ty) && !handleRadarTouch(tx, ty)) {
            handleNavTouch(tx, ty);
          }
        }
      } else {
        if (!handleHomeTouch(tx, ty) && !handleStocksTouch(tx, ty) && !handleStatusTouch(tx, ty) && !handleRadarTouch(tx, ty)) {
          handleNavTouch(tx, ty);
        }
      }
    }
  }

  if (wifiSetupOpen) return;

  if (millis() - lastDataTick >= DATA_TICK_MS) {
    lastDataTick = millis();
    ensureSunTimesForToday();
    ensureWeather();
    ensureKpIndex();
  }

  if (pageDirty || lastDrawnPage != currentPage) {
    drawCurrentPageFull();
    updateCurrentPageDynamic();
    pageDirty = false;
    dataDirty = false;
  }

  // Animate radar between one-second clock updates without drawing over overlays.
  if (currentPage == PAGE_RADAR && !sleepOff && !timerDoneDialogOpen && !focusMenuOpen) {
    updateRadarDynamic();
  }

  if (millis() - lastClockTick >= CLOCK_TICK_MS) {
    lastClockTick = millis();
    updateCurrentPageDynamic();
    dataDirty = false;
  }

  delay(10);
}


