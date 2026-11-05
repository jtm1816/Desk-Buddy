#pragma once
#include <PNGdec.h>
#include <new>
#include "WeatherRadarMap.h"
#include "WeatherRadarPalette.h"
#include "WeatherRadarCertificates.h"

// This map is deliberately fixed to Thomson; the other dashboard location
// settings must never move live precipitation onto a mismatched offline map.
static constexpr float WR_LAT = WR_MAP_LAT, WR_LON = WR_MAP_LON;
static constexpr int WR_SIZE = 192, WR_X = 24, WR_Y = 45;
// Exact source-pixel crop used by the 15-mile-radius zoom-7 view.
static constexpr int WR_CROP_START = 104, WR_CROP_SIZE = 48;
static constexpr size_t WR_MAX_PNG = 65536;
static constexpr uint32_t WR_INTERVAL = 30000;
static const uint16_t WR_COLORS[] = {
  0, 0x0400, 0x07E0, 0xAFE0, 0xFFE0, 0xFD40, 0xF800,
  0x9EFF, 0x7DFF, 0x93FF, 0xA21F, 0x7014
};
static SemaphoreHandle_t wrMutex = nullptr;
static TaskHandle_t wrTask = nullptr;
static constexpr uint8_t WR_HISTORY = 7;
struct WrCachedFrame { time_t stamp; uint8_t *pixels; };
struct WrRemoteFrame { time_t stamp; String path; };
static WrCachedFrame wrFrames[WR_HISTORY] = {};
static uint8_t wrFrameCount = 0, wrFrameIndex = 0, wrLastLoadedFrames = 0;
static time_t wrLastHistory[WR_HISTORY] = {};
static uint32_t wrLastAnimationMs = 0, wrAnimationSteps = 0;
static uint8_t *wrOverlay = nullptr; // Non-owning alias of the displayed cached crop.
static void updateWeatherRadarDynamic(bool force);
static bool wrBusy = false, wrValid = false, wrChanged = true;
static uint32_t wrLastTry = 0, wrLastSuccess = 0;
static time_t wrFrameTime = 0;
static char wrError[44] = "";

struct WrDecodeContext { PNG *png; uint8_t *pixels; };

static uint8_t wrColorIndex(uint8_t r, uint8_t g, uint8_t b) {
  uint32_t best = UINT32_MAX;
  uint16_t target = 0;
  for (const auto &entry : RADAR_COLORS) {
    int dr = int(r) - int((entry.source >> 16) & 255);
    int dg = int(g) - int((entry.source >> 8) & 255);
    int db = int(b) - int(entry.source & 255);
    uint32_t distance = dr * dr + dg * dg + db * db;
    if (distance < best) { best = distance; target = entry.target; }
  }
  for (uint8_t i = 1; i < sizeof(WR_COLORS) / sizeof(WR_COLORS[0]); ++i)
    if (WR_COLORS[i] == target) return i;
  return 0;
}

static int wrPngLine(PNGDRAW *line) {
  auto *context = static_cast<WrDecodeContext *>(line->pUser);
  if (!context || line->iWidth != 256 || line->y >= 256) return 0;
  // Reject unsupported PNG types rather than guessing at alpha or colors.
  if (line->iPixelType != PNG_PIXEL_TRUECOLOR_ALPHA || line->iBpp != 8) return 0;
  if (line->y < WR_CROP_START || line->y >= WR_CROP_START + WR_CROP_SIZE) return 1;
  int y = line->y - WR_CROP_START;
  for (int x = 0; x < WR_CROP_SIZE; ++x) {
    const uint8_t *pixel = line->pPixels + (x + WR_CROP_START) * 4;
    uint8_t alpha = (uint16_t(pixel[3]) * 15 + 127) / 255;
    if (alpha) context->pixels[y * WR_CROP_SIZE + x] =
        (wrColorIndex(pixel[0], pixel[1], pixel[2]) << 4) | alpha;
  }
  return 1;
}

// Store compressed PNG bytes in small independent blocks, preserving the
// large contiguous allocation required by PNGdec on the classic ESP32.
class WrPagedBuffer : public Stream {
 public:
  static constexpr size_t PAGE_SIZE = 2048, PAGE_COUNT = WR_MAX_PNG / PAGE_SIZE;
  uint8_t *pages[PAGE_COUNT] = {};
  size_t used = 0; bool overflow = false, memoryError = false;
  // The sprite's unused tail holds the first 14 KiB of compressed input.
  // Only unusually large images require extra heap pages.
  static constexpr size_t SHARED_PAGES = 7;
  explicit WrPagedBuffer(uint8_t *workspace) {
    for (size_t i = 0; i < SHARED_PAGES; ++i) pages[i] = workspace + 32768 + i * PAGE_SIZE;
  }
  ~WrPagedBuffer() { for (size_t i = SHARED_PAGES; i < PAGE_COUNT; ++i) free(pages[i]); }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t *data, size_t length) override {
    if (length > WR_MAX_PNG - used) { overflow = true; return 0; }
    size_t done = 0;
    while (done < length) {
      size_t page = used / PAGE_SIZE, offset = used % PAGE_SIZE;
      if (!pages[page]) pages[page] = static_cast<uint8_t *>(malloc(PAGE_SIZE));
      if (!pages[page]) { memoryError = true; return done; }
      size_t amount = min(length - done, PAGE_SIZE - offset);
      memcpy(pages[page] + offset, data + done, amount); used += amount; done += amount;
    }
    return done;
  }
  int32_t readAt(int32_t position, uint8_t *out, int32_t length) {
    if (position < 0 || length <= 0 || size_t(position) >= used) return 0;
    size_t wanted = min(size_t(length), used - size_t(position)), done = 0;
    while (done < wanted) {
      size_t at = size_t(position) + done, page = at / PAGE_SIZE, offset = at % PAGE_SIZE;
      size_t amount = min(wanted - done, PAGE_SIZE - offset);
      memcpy(out + done, pages[page] + offset, amount); done += amount;
    }
    return int32_t(done);
  }
};
static WrPagedBuffer *wrPngSource = nullptr;
static uint32_t wrDownloadCount = 0;
static void *wrOpenPaged(const char *, int32_t *size) {
  if (!wrPngSource) return nullptr;
  *size = int32_t(wrPngSource->used); return wrPngSource;
}
static void wrClosePaged(void *) {}
static int32_t wrReadPaged(PNGFILE *file, uint8_t *out, int32_t length) {
  auto *source = static_cast<WrPagedBuffer *>(file->fHandle);
  int32_t amount = source->readAt(file->iPos, out, length); file->iPos += amount; return amount;
}
static int32_t wrSeekPaged(PNGFILE *file, int32_t position) {
  auto *source = static_cast<WrPagedBuffer *>(file->fHandle);
  if (position < 0) position = 0;
  if (size_t(position) > source->used) position = int32_t(source->used);
  file->iPos = position; return position;
}

static bool wrFetchFrames(WrRemoteFrame *frames, uint8_t &count, char *error, size_t errorSize) {
  count = 0;
  WiFiClientSecure tls; tls.setCACert(ROOT_CAS); tls.setHandshakeTimeout(10);
  HTTPClient http; http.useHTTP10(true); http.setConnectTimeout(8000); http.setTimeout(10000);
  if (!http.begin(tls, "https://api.rainviewer.com/public/weather-maps.json")) {
    snprintf(error, errorSize, "Radar connection failed"); return false;
  }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    snprintf(error, errorSize, "Radar metadata HTTP %d", code); http.end(); return false;
  }
  JsonDocument doc;
  DeserializationError jsonError = deserializeJson(doc, http.getStream()); http.end();
  if (jsonError) { snprintf(error, errorSize, "Radar metadata invalid"); return false; }
  JsonArray past = doc["radar"]["past"].as<JsonArray>();
  if (past.size() == 0 || doc["host"].as<String>() != "https://tilecache.rainviewer.com") {
    snprintf(error, errorSize, "No radar frames available"); return false;
  }
  size_t start = past.size() > WR_HISTORY ? past.size() - WR_HISTORY : 0;
  time_t previous = 0;
  for (size_t i = start; i < past.size(); ++i) {
    String path = past[i]["path"].as<String>(); time_t stamp = past[i]["time"].as<time_t>();
    if (!path.startsWith("/v2/radar/") || path.indexOf("..") >= 0 || stamp < 1700000000 || stamp <= previous) {
      snprintf(error, errorSize, "Radar history invalid"); return false;
    }
    frames[count].stamp = stamp; frames[count].path = path; ++count; previous = stamp;
  }
  return count > 0;
}

// All cache operations hold wrMutex. Only these entries own their crop buffers.
static void wrSelectFrame(uint8_t index) {
  wrFrameIndex = index; wrOverlay = wrFrames[index].pixels; wrValid = true;
  wrFrameTime = wrFrames[wrFrameCount - 1].stamp;
}
static void wrInsertFrame(time_t stamp, uint8_t *pixels) {
  time_t selected = wrFrameCount ? wrFrames[wrFrameIndex].stamp : 0;
  if (wrFrameCount == WR_HISTORY) {
    free(wrFrames[0].pixels);
    for (uint8_t i = 1; i < wrFrameCount; ++i) wrFrames[i - 1] = wrFrames[i];
    --wrFrameCount;
  }
  uint8_t at = wrFrameCount;
  while (at > 0 && wrFrames[at - 1].stamp > stamp) { wrFrames[at] = wrFrames[at - 1]; --at; }
  wrFrames[at] = {stamp, pixels}; ++wrFrameCount;
  uint8_t index = selected ? 0 : wrFrameCount - 1;
  for (uint8_t i = 0; i < wrFrameCount; ++i) if (wrFrames[i].stamp == selected) index = i;
  wrSelectFrame(index); wrChanged = true;
  if (wrFrameCount == 1) wrLastAnimationMs = millis();
}
static void wrClearFrames() {
  for (uint8_t i = 0; i < wrFrameCount; ++i) { free(wrFrames[i].pixels); wrFrames[i] = {}; }
  wrFrameCount = 0; wrFrameIndex = 0; wrOverlay = nullptr; wrValid = false; wrChanged = true;
}

static bool wrDownload(const String &path, WrPagedBuffer &buffer, char *error, size_t errorSize) {
  String url = "https://tilecache.rainviewer.com" + path + "/256/7/33.47060/-82.50090/2/1_1.png";
  WiFiClientSecure tls; tls.setCACert(ROOT_CAS); tls.setHandshakeTimeout(10);
  HTTPClient http; http.useHTTP10(true); http.setConnectTimeout(8000); http.setTimeout(10000);
  if (!http.begin(tls, url)) { snprintf(error, errorSize, "Image connection failed"); return false; }
  int code = http.GET(); int contentLength = http.getSize();
  if (code != HTTP_CODE_OK || contentLength > int(WR_MAX_PNG)) {
    snprintf(error, errorSize, "Radar image HTTP %d", code); http.end(); return false;
  }
  int received = http.writeToStream(&buffer); http.end();
  if (received <= 0 || buffer.overflow || buffer.memoryError ||
      (contentLength > 0 && buffer.used != size_t(contentLength))) {
    snprintf(error, errorSize, "%s", buffer.memoryError ? "Radar page allocation failed" : "Radar image incomplete");
    return false;
  }
  if (buffer.used > 0) { ++wrDownloadCount; return true; }
  return false;
}

static void wrWorker(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    char error[44] = ""; bool ok = false;
    WrRemoteFrame requested[WR_HISTORY]; uint8_t requestedCount = 0;
    if (!wifiEnabled || WiFi.status() != WL_CONNECTED) snprintf(error, sizeof(error), "Wi-Fi unavailable");
    else if (time(nullptr) < 1700000000) snprintf(error, sizeof(error), "Waiting for clock sync");
    else {
      bool metadataOK;
      { NetworkFetchGuard guard; metadataOK = wrFetchFrames(requested, requestedCount, error, sizeof(error)); }
      // Load newest first for a useful image immediately, then fill history.
      if (metadataOK) for (int frame = int(requestedCount) - 1; frame >= 0; --frame) {
        xSemaphoreTake(wrMutex, portMAX_DELAY);
        bool cached = false;
        for (uint8_t i = 0; i < wrFrameCount; ++i) if (wrFrames[i].stamp == requested[frame].stamp) cached = true;
        xSemaphoreGive(wrMutex);
        if (cached) { ok = true; continue; }
        uint8_t *pixels = nullptr; bool decoded = false;
        {
          // Release the network mutex between frames so aircraft and stocks
          // can update while the history loads. HTTPS buffers precede decode.
          NetworkFetchGuard guard;
          // Reserve only the 32 KiB dictionary during TLS; allocate the much
          // smaller decoder state after HTTPS buffers have been released.
          uint8_t *dictionary = radarWorkspace;
          if (!dictionary || !radarWorkspaceMutex) snprintf(error, sizeof(error), "Radar workspace unavailable");
          else {
            // Keep the shared input pages intact through HTTPS and decode.
            // Flight drawing uses a nonblocking lock and touch keeps running.
            xSemaphoreTake(radarWorkspaceMutex, portMAX_DELAY);
            {
            WrPagedBuffer download(dictionary);
            if (wrDownload(requested[frame].path, download, error, sizeof(error))) {
              PNG *decoder = new (std::nothrow) PNG;
              if (!decoder) snprintf(error, sizeof(error), "Radar state allocation failed");
              else {
              wrPngSource = &download;
              int opened = decoder->open("radar", wrOpenPaged, wrClosePaged, wrReadPaged, wrSeekPaged, wrPngLine);
              decoder->setDictionary(dictionary);
              if (opened == PNG_SUCCESS && decoder->getWidth() == 256 && decoder->getHeight() == 256 &&
                  decoder->getPixelType() == PNG_PIXEL_TRUECOLOR_ALPHA && decoder->getBpp() == 8 && !decoder->isInterlaced()) {
                pixels = static_cast<uint8_t *>(calloc(WR_CROP_SIZE * WR_CROP_SIZE, 1));
                if (pixels) {
                  WrDecodeContext context{decoder, pixels};
                  int result = decoder->decode(&context, PNG_CHECK_CRC);
                  decoded = result == PNG_SUCCESS;
                  if (!decoded) snprintf(error, sizeof(error), "PNG decode error %d", result);
                } else snprintf(error, sizeof(error), "Radar buffer allocation failed");
              } else snprintf(error, sizeof(error), "Unsupported radar image");
              if (opened == PNG_SUCCESS) decoder->close();
              wrPngSource = nullptr;
              delete decoder;
              }
            }
            } // Release overflow pages before allowing flight drawing again.
            xSemaphoreGive(radarWorkspaceMutex);
          }
        }
        if (decoded) {
          xSemaphoreTake(wrMutex, portMAX_DELAY); wrInsertFrame(requested[frame].stamp, pixels);
          xSemaphoreGive(wrMutex); pixels = nullptr; ok = true;
        }
        free(pixels); vTaskDelay(pdMS_TO_TICKS(1));
      }
    }
    xSemaphoreTake(wrMutex, portMAX_DELAY);
    // A successful refresh includes every requested timestamp, even when some
    // frames were already cached. Partial history must not report success.
    bool complete = requestedCount > 0;
    for (uint8_t i = 0; i < requestedCount; ++i) {
      bool found = false;
      for (uint8_t j = 0; j < wrFrameCount; ++j)
        if (wrFrames[j].stamp == requested[i].stamp) found = true;
      if (!found) complete = false;
    }
    ok = complete;
    if (complete) { error[0] = 0; wrLastSuccess = millis(); }
    else if (!error[0]) snprintf(error, sizeof(error), "Radar history incomplete");
    wrLastLoadedFrames = wrFrameCount;
    for (uint8_t i = 0; i < WR_HISTORY; ++i) wrLastHistory[i] = i < wrFrameCount ? wrFrames[i].stamp : 0;
    snprintf(wrError, sizeof(wrError), "%s", error); wrBusy = false; wrChanged = true;
    uint8_t loaded = wrFrameCount; xSemaphoreGive(wrMutex);
    Serial.printf("weather-radar: %s; history: %u; heap: %u; largest: %u\n",
                  ok ? "OK" : error, loaded, unsigned(ESP.getFreeHeap()), unsigned(ESP.getMaxAllocHeap()));
  }
}

static void weatherRadarRequest() {
  if (!wrMutex || !wrTask) return;
  xSemaphoreTake(wrMutex, portMAX_DELAY);
  if (!wrBusy) {
    wrBusy = true; wrChanged = true; wrLastTry = millis(); xTaskNotifyGive(wrTask);
  }
  xSemaphoreGive(wrMutex);
}

static void initWeatherRadar() {
  wrMutex = xSemaphoreCreateMutex();
  if (!wrMutex || xTaskCreate(wrWorker, "WeatherRadar", 8192, nullptr, 1, &wrTask) != pdPASS)
    snprintf(wrError, sizeof(wrError), "Radar task unavailable");
  // Diagnostic endpoints provide read-only status and an explicit refresh;
  // they do not change navigation or any saved settings.
  server.on("/weather-radar/status", HTTP_GET, []() {
    JsonDocument doc;
    if (wrMutex) xSemaphoreTake(wrMutex, portMAX_DELAY);
    doc["center"] = "Thomson, GA"; doc["latitude"] = WR_LAT; doc["longitude"] = WR_LON;
    doc["radius_miles"] = 15; doc["valid"] = wrValid; doc["busy"] = wrBusy;
    doc["frame_time"] = wrFrameTime; doc["last_success_ms"] = wrLastSuccess; doc["error"] = wrError;
    doc["overlay_bytes"] = wrFrameCount * WR_CROP_SIZE * WR_CROP_SIZE;
    doc["downloads"] = wrDownloadCount; doc["shared_workspace_bytes"] = radarWorkspace ? 221 * 221 : 0;
    doc["flight_buffer_ready"] = radarSpriteReady;
    doc["visible"] = currentPage == PAGE_WEATHER_RADAR && !wifiSetupOpen && !sleepOff;
    doc["frame_count"] = wrFrameCount; doc["display_frame_time"] = wrFrameCount ? wrFrames[wrFrameIndex].stamp : 0;
    doc["frame_index"] = wrFrameIndex; doc["animation_steps"] = wrAnimationSteps;
    doc["last_loaded_frames"] = wrLastLoadedFrames;
    JsonArray history = doc["last_history_times"].to<JsonArray>();
    for (uint8_t i = 0; i < wrLastLoadedFrames; ++i) history.add(wrLastHistory[i]);
    if (wrMutex) xSemaphoreGive(wrMutex);
    doc["free_heap"] = ESP.getFreeHeap(); doc["largest_block"] = ESP.getMaxAllocHeap();
    String response; serializeJson(doc, response); server.send(200, "application/json", response);
  });
  server.on("/weather-radar/refresh", HTTP_POST, []() {
    weatherRadarRequest(); server.send(202, "text/plain", "Radar refresh requested");
  });
}

static void weatherRadarService() {
  if (!wrMutex) return;
  bool visible = currentPage == PAGE_WEATHER_RADAR && !wifiSetupOpen && !sleepOff;
  if (visible && (!wrLastTry || millis() - wrLastTry >= WR_INTERVAL)) weatherRadarRequest();
  if (xSemaphoreTake(wrMutex, 0) == pdTRUE) {
    if (visible && wrFrameCount > 1) {
      uint32_t hold = wrFrameIndex == wrFrameCount - 1 ? 1500 : 750;
      if (millis() - wrLastAnimationMs >= hold) {
        wrSelectFrame((wrFrameIndex + 1) % wrFrameCount); wrLastAnimationMs = millis();
        ++wrAnimationSteps; wrChanged = true;
      }
    } // Keep decoded history when switching tabs; refresh only when visible.
    xSemaphoreGive(wrMutex);
  }
  // Render changing frames independently of the dashboard's one-second clock.
  if (visible) updateWeatherRadarDynamic(false);
}

static uint16_t wrBlend(uint16_t foreground, uint16_t base, uint8_t alpha) {
  // 40% opacity, matching the supplied reference; alpha retains PNG transparency.
  unsigned weight = unsigned(alpha) * 102 / 15, bg = 255 - weight;
  return ((((foreground >> 11) * weight + (base >> 11) * bg) / 255) << 11) |
      (((((foreground >> 5) & 63) * weight + ((base >> 5) & 63) * bg) / 255) << 5) |
      (((foreground & 31) * weight + (base & 31) * bg) / 255);
}

static void updateWeatherRadarDynamic(bool force = false) {
  if (!wrMutex || xSemaphoreTake(wrMutex, 0) != pdTRUE) return;
  static uint32_t lastStatus = 0;
  if (!force && !wrChanged && millis() - lastStatus < 30000) { xSemaphoreGive(wrMutex); return; }
  wrChanged = false; lastStatus = millis();
  if (force) tft.fillRect(0, TOPBAR_H, SCREEN_W, SCREEN_H - NAV_H - TOPBAR_H, COL_BG);
  else tft.fillRect(0, TOPBAR_H, SCREEN_W, WR_Y - TOPBAR_H, COL_BG);
  tft.setTextDatum(TL_DATUM); tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString("Thomson, GA | 15mi N", 8, 36, 1);
  char frame[20] = "Loading...";
  if (wrValid && wrFrameTime) {
    time_t shown = wrFrames[wrFrameIndex].stamp;
    struct tm frameLocalTime; localtime_r(&shown, &frameLocalTime);
    char clockText[12]; strftime(clockText, sizeof(clockText), "%I:%M %p", &frameLocalTime);
    snprintf(frame, sizeof(frame), "%u/%u %s", unsigned(wrFrameIndex + 1), unsigned(wrFrameCount), clockText);
  } else if (!wrBusy) snprintf(frame, sizeof(frame), "Unavailable");
  tft.drawRightString(frame, 232, 36, 1);
  const float span = 30.0f * 1609.344f /
      (40075016.686f * cosf(WR_LAT * PI / 180.0f) / 32768.0f);
  const float origin = 128.0f - span / 2;
  uint16_t row[WR_SIZE];
  for (int y = 0; y < WR_SIZE; ++y) {
    for (int x = 0; x < WR_SIZE; ++x) {
      size_t index = y * WR_SIZE + x;
      uint16_t base = pgm_read_word(WR_BASEMAP_PALETTE + pgm_read_byte(WR_BASEMAP_INDEX + index));
      int sx = int(origin + (x + 0.5f) * span / WR_SIZE) - WR_CROP_START;
      int sy = int(origin + (y + 0.5f) * span / WR_SIZE) - WR_CROP_START;
      bool inside = (x - 95.5f) * (x - 95.5f) + (y - 95.5f) * (y - 95.5f) <= 96 * 96;
      uint8_t overlay = wrValid && wrOverlay && inside && sx >= 0 && sx < WR_CROP_SIZE && sy >= 0 && sy < WR_CROP_SIZE ?
          wrOverlay[sy * WR_CROP_SIZE + sx] : 0;
      uint8_t color = overlay >> 4;
      bool label = pgm_read_byte(WR_LABEL_MASK + index / 8) & (0x80 >> (index & 7));
      row[x] = color && color < sizeof(WR_COLORS) / sizeof(WR_COLORS[0]) && !label ?
          wrBlend(WR_COLORS[color], base, overlay & 15) : base;
    }
    tft.pushImage(WR_X, WR_Y + y, WR_SIZE, 1, row);
  }
  tft.drawCircle(WR_X + 96, WR_Y + 96, 4, TFT_BLACK);
  tft.drawCircle(WR_X + 96, WR_Y + 96, 3, COL_ACCENT);
  bool stale = wrValid && time(nullptr) - wrFrameTime > 1800;
  if (!wrValid || stale || wrError[0]) {
    const char *message = !wrValid ? (wrBusy ? "Loading radar..." : "Radar unavailable / retrying") :
        stale ? "STALE RADAR" : "Update failed / retrying";
    tft.fillRoundRect(31, 52, 178, 14, 3, TFT_BLACK);
    tft.setTextColor(stale || wrError[0] ? TFT_YELLOW : COL_TEXT, TFT_BLACK);
    tft.drawCentreString(message, 120, 55, 1);
  }
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString("RAIN", 8, 240, 1); tft.drawString("SNOW", 124, 240, 1);
  for (int i = 0; i < 6; ++i) tft.fillRect(8 + i * 17, 249, 16, 5, WR_COLORS[i + 1]);
  for (int i = 0; i < 5; ++i) tft.fillRect(124 + i * 21, 249, 20, 5, WR_COLORS[i + 7]);
  tft.setTextColor(COL_DIM, COL_BG);
  tft.drawString("Light", 8, 256, 1); tft.drawRightString("Heavy", 109, 256, 1);
  tft.drawString("Light", 124, 256, 1); tft.drawRightString("Heavy", 228, 256, 1);
  tft.drawCentreString("RainViewer | (c) OSM contributors", 120, 267, 1);
  xSemaphoreGive(wrMutex);
}

static void drawWeatherRadarPageFull() {
  tft.fillScreen(COL_BG); drawTopBar("Weather Radar"); drawNavBar();
  weatherRadarRequest(); updateWeatherRadarDynamic(true);
  lastDrawnPage = PAGE_WEATHER_RADAR; pageDirty = false;
}





