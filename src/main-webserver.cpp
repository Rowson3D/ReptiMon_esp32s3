#include <Arduino.h>
#include <Wire.h>
#include "version_auto.h"
#include <SHT85.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <ctype.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
// Camera support
#include "esp_camera.h"
// Hardware watchdog (TWDT)
#include <esp_task_wdt.h>

// WiFi Configuration - managed via Preferences and Web UI
String selectedSSID = "";
String selectedPassword = "";
Preferences preferences; // stores ssid/pass persistently
Preferences appPrefs;    // stores app settings persistently

// Access Point configuration (fallback / entry point mode)
String ap_ssid_dyn = "";          // e.g., ReptiMon-ABC123
String ap_password_dyn = "";      // unique per-device password derived from chip eFuse MAC
bool useAccessPoint = false;       // whether AP is currently active
bool captivePortalActive = false;  // whether DNS redirect is active
unsigned long apShutdownAt = 0;    // millis() target to tear down AP after successful connect (0 = not scheduled)
DNSServer dnsServer;               // captive portal DNS server
const byte DNS_PORT = 53;

// Create SHT30 sensor object with default address 0x44
SHT85 sht30(0x44);

// Web server on port 80
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
AsyncWebSocket camWs("/ws/cam"); // dedicated binary camera stream

// ESP32 XIAO S3 Built-in LEDs
#define LED_BUILTIN_RED   21  // Red LED
#define LED_BUILTIN_BLUE  2   // Blue/Yellow LED

// Performance optimization constants
#define SENSOR_UPDATE_INTERVAL_MS     50    // Ultra-fast 20Hz updates (50ms)
#define DISPLAY_UPDATE_INTERVAL_MS    100   // 10Hz display updates (100ms)
#define SERIAL_BAUD_RATE             921600 // Maximum reliable baud rate
#define I2C_CLOCK_SPEED              100000 // Standard I2C 100kHz (debug; SHT85 ok up to 1MHz)

// FreeRTOS task handles and synchronization
TaskHandle_t sensorTaskHandle = NULL;
TaskHandle_t displayTaskHandle = NULL;
TaskHandle_t ledTaskHandle = NULL;
TaskHandle_t webTaskHandle = NULL;
QueueHandle_t sensorDataQueue = NULL;
SemaphoreHandle_t dataMutex = NULL;

// Application settings persisted in NVS
struct AppSettings {
  String hostname = "momo"; // mDNS/DHCP hostname
  String units = "C";       // UI display units: "C" or "F"
  float tempMin = 22.0f;
  float tempMax = 32.0f;
  float tempIdeal = 26.0f;
  float humMin = 50.0f;
  float humMax = 80.0f;
  float humIdeal = 65.0f;
  // Comfort index thresholds (0-100)
  float comfortMin = 60.0f;
  float comfortMax = 100.0f;
  float comfortIdeal = 85.0f;
  int camFrameSize = 8;  // FRAMESIZE_VGA (640x480) — 30fps sweet spot for OV3660 over WiFi
  int camQuality = 15;   // JPEG quality 15 = ~8-12 KB/frame; necessary headroom for 30fps
  int camBrightness = 0;  // -2..2
  int camContrast = 1;    // -2..2
  int camSaturation = 1;  // -2..2
  int camWbMode = 0;      // 0=auto,1=sunny,2=cloudy,3=office,4=home
  int camSpecial = 0;     // 0=none..6=sepia
  bool camHmirror = false;
  bool camVflip = false;
};
AppSettings appSettings;
String mdnsHostname = "momo";
bool mdnsActive = false;

// Reptile enclosure monitoring variables
struct EnvironmentData {
  float temperature;
  float humidity;
  float dewPoint;
  float heatIndex;
  float vaporPressureDeficit;
  float absoluteHumidity;
  unsigned long timestamp;
  bool valid;
};

// Statistics tracking with lock-free updates
struct Statistics {
  volatile float tempMin, tempMax, tempAvg;
  volatile float humMin, humMax, humAvg;
  volatile float dewMin, dewMax, dewAvg;
  volatile int readingCount;
  volatile bool initialized;
} stats = {0};

// Current data with atomic access
volatile EnvironmentData currentData = {0};
EnvironmentData readings[32]; // Larger buffer for high-frequency data
volatile int readingIndex = 0;

// Reptile care thresholds (adjustable for your species)
struct ReptileThresholds {
  float tempMin = 22.0;      // Minimum safe temperature (°C)
  float tempMax = 32.0;      // Maximum safe temperature (°C)
  float tempIdeal = 26.0;    // Ideal temperature (°C)
  float humMin = 50.0;       // Minimum humidity (%)
  float humMax = 80.0;       // Maximum humidity (%)
  float humIdeal = 65.0;     // Ideal humidity (%)
  // Comfort thresholds (0-100 scale)
  float comfortMin = 60.0;
  float comfortMax = 100.0;
  float comfortIdeal = 85.0;
} thresholds;

// Performance monitoring
unsigned long lastSensorRead = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long sensorReadCount = 0;
unsigned long displayUpdateCount = 0;

// Web server variables
String lastJsonData = "";
unsigned long lastWebUpdate = 0;

// Firmware version and OTA state
#ifndef FW_VERSION
#define FW_VERSION "0.1.0"
#endif
#ifndef GIT_COMMIT
#define GIT_COMMIT "unknown"
#endif
#ifndef BUILD_TIME
#define BUILD_TIME ""
#endif
#ifndef GITHUB_OWNER
#define GITHUB_OWNER "Rowson3D"
#endif
#ifndef GITHUB_REPO
#define GITHUB_REPO  "ReptiMon"
#endif
#ifndef GITHUB_ASSET_NAME
#define GITHUB_ASSET_NAME "firmware.bin"
#endif
// Optional filesystem asset for LittleFS image OTA
#ifndef GITHUB_FS_ASSET_NAME
#define GITHUB_FS_ASSET_NAME "littlefs.bin"
#endif
// Optional GitHub token to avoid anonymous rate limits (set via -DGITHUB_TOKEN="ghp_...")
#ifndef GITHUB_TOKEN
#define GITHUB_TOKEN ""
#endif
// Optional PEM CA certificate for OTA HTTPS validation.
// Build with -DGITHUB_CA_CERT="R\"EOF(...pem...)EOF\"" to avoid setInsecure().
#ifndef GITHUB_CA_CERT
#define GITHUB_CA_CERT ""
#endif
static String fwVersion = String(FW_VERSION);
static String fwCommit  = String(GIT_COMMIT);
static String fwBuild   = String(BUILD_TIME);
// Effective installed version (from last OTA). When present, this overrides fwVersion for reporting/comparison.
static String installedFwTag = ""; // normalized like 0.2.1 (no leading 'v')
static const char* kGithubOwner = GITHUB_OWNER;  // override via -DGITHUB_OWNER=\"owner\"
static const char* kGithubRepo  = GITHUB_REPO;   // override via -DGITHUB_REPO=\"repo\"
static const char* kGithubAsset = GITHUB_ASSET_NAME; // override via -DGITHUB_ASSET_NAME=\"name.bin\"
static String otaLatestVersion = "";
static String otaLatestUrl = "";
static volatile bool otaInProgress = false;
static SemaphoreHandle_t otaLock = NULL;  // prevents concurrent OTA task starts (F9)
// OTA state for UI synchronization
static volatile int otaPct = 0;              // 0..100
static String otaPhase = "idle";            // idle|starting|fw|fw_done|fs|fs_done|rebooting|error
static unsigned long otaStartMs = 0;
static String otaErrorMsg = "";
static SemaphoreHandle_t otaStateMutex = NULL;

static void setOtaState(const char* phase, int pct = -1) {
  if (otaStateMutex) xSemaphoreTake(otaStateMutex, portMAX_DELAY);
  otaPhase = phase ? String(phase) : otaPhase;
  if (pct >= 0) otaPct = pct;
  if (otaPhase == "starting") otaStartMs = millis();
  if (otaStateMutex) xSemaphoreGive(otaStateMutex);
}
static void setOtaError(const String& msg) {
  if (otaStateMutex) xSemaphoreTake(otaStateMutex, portMAX_DELAY);
  otaPhase = "error"; otaErrorMsg = msg;
  if (otaStateMutex) xSemaphoreGive(otaStateMutex);
}

// OTA logging (rolling buffer)
static const int OTA_LOG_CAP = 200;
static String otaLogBuf[OTA_LOG_CAP];
static int otaLogStart = 0;  // index of oldest
static int otaLogCount = 0;  // number of valid entries
static SemaphoreHandle_t otaLogMutex = NULL;

static void otaLog(const String &line) {
  if (!otaLogMutex) otaLogMutex = xSemaphoreCreateMutex();
  if (otaLogMutex) xSemaphoreTake(otaLogMutex, portMAX_DELAY);
  // Timestamp (ms since boot)
  String msg = "[" + String(millis()) + "] " + line;
  int idx;
  if (otaLogCount < OTA_LOG_CAP) {
    idx = (otaLogStart + otaLogCount) % OTA_LOG_CAP;
    otaLogCount++;
  } else {
    // overwrite oldest
    idx = otaLogStart;
    otaLogStart = (otaLogStart + 1) % OTA_LOG_CAP;
  }
  otaLogBuf[idx] = msg;
  // Mirror to Serial for dev
  Serial.println(msg);
  if (otaLogMutex) xSemaphoreGive(otaLogMutex);
}

static void otaLogf(const char *fmt, ...) {
  char buf[192];
  va_list ap; va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  otaLog(String(buf));
}

// Camera state
bool cameraAvailable = false;
// Camera stream statistics (global across clients)
volatile unsigned long camStatFrames = 0;
volatile unsigned long camStatBytes = 0;
unsigned long camStatStartMs = 0;
// Protect camera operations (stream vs reconfigure)
SemaphoreHandle_t cameraMutex = NULL;

// Forward declarations for camera/settings
bool initCamera();
void loadAppSettings();
void saveAppSettings(const AppSettings &s);

// WiFi control state
volatile bool wifiConnectPending = false;
volatile bool scanScheduled    = false;  // set by handler, executed in loop()
volatile bool connectScheduled = false;  // set by handler, executed in loop()
unsigned long wifiConnectStart = 0;
String pendingSSID = "";
String pendingPass = "";

// Forward declarations
inline float calculateDewPoint(float temp, float humidity);
inline float calculateHeatIndex(float temp, float humidity);
inline float calculateVPD(float temp, float humidity);
inline float calculateAbsoluteHumidity(float temp, float humidity);
String getTemperatureStatus(float temp);
String getHumidityStatus(float humidity);
void updateStatisticsAtomic(float temp, float humidity, float dewPoint);
void updateLEDStatusFast(float temp, float humidity);
void printUltraFastReading(EnvironmentData &data);
void sensorTask(void *parameter);
void displayTask(void *parameter);
void ledTask(void *parameter);
void webTask(void *parameter);
void setupWiFi();
void setupWebServer();
String generateJsonData();
void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
             void *arg, uint8_t *data, size_t len);

// ---- Version helpers ----
static String normalizeVersion(const String& v) {
  // Trim spaces and a leading 'v' or 'V'
  int i = 0;
  while (i < (int)v.length() && isspace((int)v[i])) i++;
  if (i < (int)v.length() && (v[i] == 'v' || v[i] == 'V')) i++;
  String s = v.substring(i);
  // Strip git-describe dirty-build suffix: "0.3.1-1-g7e04d7c" -> "0.3.1" (F4)
  // Without this, dev builds compare as newer than any release and OTA never fires.
  int dash = s.indexOf('-');
  if (dash > 0) s = s.substring(0, dash);
  return s;
}
static int semverCompare(const String& aIn, const String& bIn) {
  String a = normalizeVersion(aIn), b = normalizeVersion(bIn);
  // Compare dotted numeric parts, non-numeric ignored
  int ia = 0, ib = 0;
  while (ia < (int)a.length() || ib < (int)b.length()) {
    long va = 0, vb = 0;
    // parse next int from a
    while (ia < (int)a.length() && !isdigit((int)a[ia])) ia++;
    while (ia < (int)a.length() && isdigit((int)a[ia])) { va = va * 10 + (a[ia]-'0'); ia++; }
    // skip separators
    while (ia < (int)a.length() && a[ia] != '\0' && !isdigit((int)a[ia])) ia++;
    // parse next int from b
    while (ib < (int)b.length() && !isdigit((int)b[ib])) ib++;
    while (ib < (int)b.length() && isdigit((int)b[ib])) { vb = vb * 10 + (b[ib]-'0'); ib++; }
    while (ib < (int)b.length() && b[ib] != '\0' && !isdigit((int)b[ib])) ib++;
    if (va != vb) return (va < vb) ? -1 : 1;
  }
  return 0; // equal
}

// OTA helpers (GitHub Releases)
static void configureOtaClient(WiFiClientSecure& client) {
  if (strlen(GITHUB_CA_CERT) > 0) {
    client.setCACert(GITHUB_CA_CERT);
  } else {
    client.setInsecure();
  }
}

static String httpGet(const String& url) {
  WiFiClientSecure client;
  configureOtaClient(client);
  HTTPClient https;
  if (!https.begin(client, url)) return String();
  https.addHeader("User-Agent", "ReptiMon-OTA");
  // Hint GitHub to return JSON (not strictly required, but more future-proof)
  https.addHeader("Accept", "application/vnd.github+json");
  // Optional Authorization to bypass low anonymous rate limits
  if (GITHUB_TOKEN[0] != '\0') {
    https.addHeader("Authorization", String("Bearer ") + GITHUB_TOKEN);
  }
  https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int code = https.GET();
  if (code != HTTP_CODE_OK) { https.end(); return String(); }
  String body = https.getString();
  https.end();
  return body;
}

// New: supports prereleases and returns both firmware and filesystem URLs
// Cache: GitHub check result is cached for 5 minutes to avoid hammering the API and
// to prevent concurrent HTTPS calls that trigger the non-reentrant bootloader_mmap.
struct GhCache {
  String tag, fwUrl, fsUrl, relPage, published;
  bool   valid  = false;
  bool   ok     = false;
  unsigned long expiresMs = 0;
};
static GhCache ghCache;
static SemaphoreHandle_t ghMutex = NULL; // ensure only one HTTPS call at a time

static bool getGithubLatest(String& outTag, String& outFwUrl, String& outFsUrl, String& outReleasePage, String& outPublished) {
  String base = String("https://api.github.com/repos/") + kGithubOwner + "/" + kGithubRepo + "/releases";
  auto parseRelease = [&](JsonVariant v)->bool{
    if (!v.is<JsonObject>()) return false;
    const char* tagC = v["tag_name"].is<const char*>() ? v["tag_name"].as<const char*>() : "";
    const char* pageC = v["html_url"].is<const char*>() ? v["html_url"].as<const char*>() : "";
    const char* pubC  = v["published_at"].is<const char*>() ? v["published_at"].as<const char*>() : "";
    outTag = String(tagC);
    outReleasePage = String(pageC);
    outPublished = String(pubC);
    outFwUrl = ""; outFsUrl = "";
    JsonArray assets = v["assets"].as<JsonArray>();
    if (!assets.isNull()) {
      for (JsonVariant a : assets) {
        const char* name = a["name"].is<const char*>() ? a["name"].as<const char*>() : "";
        const char* dl   = a["browser_download_url"].is<const char*>() ? a["browser_download_url"].as<const char*>() : "";
        if (!name || !dl || !*dl) continue;
        if (strcmp(name, kGithubAsset) == 0) outFwUrl = String(dl);
        if (strcmp(name, GITHUB_FS_ASSET_NAME) == 0) outFsUrl = String(dl);
      }
    }
    return outTag.length() && outFwUrl.length();
  };
  // 1) /latest (published releases)
  String latestJson = httpGet(base + "/latest");
  if (latestJson.length()) {
    DynamicJsonDocument d(32768);
    if (!deserializeJson(d, latestJson)) {
      if (parseRelease(d.as<JsonVariant>())) return true;
    }
  }
  // 2) Fallback to releases list (includes prereleases)
  String listJson = httpGet(base);
  if (!listJson.length()) return false;
  DynamicJsonDocument arr(65536);
  if (deserializeJson(arr, listJson)) return false;
  if (!arr.is<JsonArray>()) return false;
  for (JsonVariant r : arr.as<JsonArray>()) {
    if (parseRelease(r)) return true; // GitHub returns newest first
  }
  return false;
}

static bool applyOtaFromUrl(const String& url, String& outMsg) {
  otaLogf("Firmware OTA: GET %s", url.c_str());
  setOtaState("fw", 0);
  WiFiClientSecure client;
  configureOtaClient(client);
  HTTPClient https;
  if (!https.begin(client, url)) { outMsg = "begin failed"; return false; }
  // GitHub asset URLs redirect to S3 – follow redirects to get the actual payload
  https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  https.addHeader("User-Agent", "ReptiMon-OTA");
  https.addHeader("Accept", "application/octet-stream");
  if (GITHUB_TOKEN[0] != '\0') {
    https.addHeader("Authorization", String("Bearer ") + GITHUB_TOKEN);
  }
  int code = https.GET();
  if (code != HTTP_CODE_OK) { outMsg = String("HTTP ") + code; https.end(); otaLogf("Firmware OTA: HTTP %d", code); return false; }
  int len = https.getSize();
  size_t updateSize = (len > 0) ? (size_t)len : UPDATE_SIZE_UNKNOWN;
  if (!Update.begin(updateSize)) { outMsg = "Update.begin failed"; https.end(); otaLog("Firmware OTA: Update.begin failed"); setOtaError("fw: Update.begin failed"); return false; }
  otaInProgress = true;
  // Progress logging (throttled)
  int lastPct = -10;
  Update.onProgress([&](size_t pos, size_t total){
    int pct = (total ? (int)((pos * 100) / total) : (pos % 100));
    if (pct >= lastPct + 10) { lastPct = pct; otaLogf("Firmware OTA: %d%%", pct); }
    setOtaState("fw", pct);
  });
  WiFiClient* stream = https.getStreamPtr();
  size_t written = Update.writeStream(*stream);
  bool ok = Update.end();
  if (len > 0) { ok = ok && (written == (size_t)len); }
  https.end();
  otaInProgress = false;
  if (!ok) { outMsg = String("Update failed: ") + (Update.getError()); setOtaError(outMsg); return false; }
  outMsg = "OK";
  otaLog("Firmware OTA: completed");
  setOtaState("fw_done", 100);
  return true;
}

// Filesystem OTA (LittleFS image)
static bool applyFsOtaFromUrl(const String& url, String& outMsg) {
  otaLogf("FS OTA: GET %s", url.c_str());
  setOtaState("fs", 0);
  WiFiClientSecure client;
  configureOtaClient(client);
  HTTPClient https;
  if (!https.begin(client, url)) { outMsg = "begin failed"; return false; }
  // GitHub asset URLs redirect to S3 – follow redirects to get the actual payload
  https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  https.addHeader("User-Agent", "ReptiMon-OTA");
  https.addHeader("Accept", "application/octet-stream");
  if (GITHUB_TOKEN[0] != '\0') {
    https.addHeader("Authorization", String("Bearer ") + GITHUB_TOKEN);
  }
  int code = https.GET();
  if (code != HTTP_CODE_OK) { outMsg = String("HTTP ") + code; https.end(); otaLogf("FS OTA: HTTP %d", code); return false; }
  int len = https.getSize();
  size_t updateSize = (len > 0) ? (size_t)len : UPDATE_SIZE_UNKNOWN;
  // Unmount LittleFS before writing the new image to avoid filesystem corruption (F3)
  LittleFS.end();
  // Prefer U_FS if available (Arduino-ESP32 2.x); fallback to U_SPIFFS for older cores
  #ifdef U_FS
    const int FS_TARGET = U_FS;
  #else
    const int FS_TARGET = U_SPIFFS;
  #endif
  if (!Update.begin(updateSize, FS_TARGET)) { outMsg = "Update.begin(FS) failed"; https.end(); otaLog("FS OTA: Update.begin failed"); setOtaError("fs: Update.begin failed"); return false; }
  otaInProgress = true;
  int lastPct = -10;
  Update.onProgress([&](size_t pos, size_t total){
    int pct = (total ? (int)((pos * 100) / total) : (pos % 100));
    if (pct >= lastPct + 10) { lastPct = pct; otaLogf("FS OTA: %d%%", pct); }
    setOtaState("fs", pct);
  });
  WiFiClient* stream = https.getStreamPtr();
  size_t written = Update.writeStream(*stream);
  bool ok = Update.end();
  if (len > 0) { ok = ok && (written == (size_t)len); }
  https.end();
  otaInProgress = false;
  if (!ok) { outMsg = String("FS Update failed: ") + (Update.getError()); setOtaError(outMsg); return false; }
  outMsg = "OK";
  otaLog("FS OTA: completed");
  setOtaState("fs_done", 100);
  return true;
}

// XIAO ESP32S3 + OV2640 default pinout (common AI-Thinker compatible)
// Adjust if your breakout differs
#ifndef PWDN_GPIO_NUM
#define PWDN_GPIO_NUM    -1
#endif
#ifndef RESET_GPIO_NUM
#define RESET_GPIO_NUM   -1
#endif
#ifndef XCLK_GPIO_NUM
#define XCLK_GPIO_NUM    10
#endif
#ifndef SIOD_GPIO_NUM
#define SIOD_GPIO_NUM    40
#endif
#ifndef SIOC_GPIO_NUM
#define SIOC_GPIO_NUM    39
#endif

#ifndef Y9_GPIO_NUM
#define Y9_GPIO_NUM      48
#endif
#ifndef Y8_GPIO_NUM
#define Y8_GPIO_NUM      11
#endif
#ifndef Y7_GPIO_NUM
#define Y7_GPIO_NUM      12
#endif
#ifndef Y6_GPIO_NUM
#define Y6_GPIO_NUM      14
#endif
#ifndef Y5_GPIO_NUM
#define Y5_GPIO_NUM      16
#endif
#ifndef Y4_GPIO_NUM
#define Y4_GPIO_NUM      18
#endif
#ifndef Y3_GPIO_NUM
#define Y3_GPIO_NUM      17
#endif
#ifndef Y2_GPIO_NUM
#define Y2_GPIO_NUM      15
#endif
#ifndef VSYNC_GPIO_NUM
#define VSYNC_GPIO_NUM   38
#endif
#ifndef HREF_GPIO_NUM
#define HREF_GPIO_NUM    47
#endif
#ifndef PCLK_GPIO_NUM
#define PCLK_GPIO_NUM    13
#endif

bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  // Use non-deprecated SCCB pin fields
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000; // stable XCLK for OV3660 on this board
  config.pixel_format = PIXFORMAT_JPEG;

  // Frame size and quality from settings
  config.frame_size = (framesize_t)appSettings.camFrameSize;
  config.jpeg_quality = appSettings.camQuality;
  // Double buffer: one frame being sent, one being captured.
  // Let the driver pick the optimal memory location (PSRAM vs DRAM) to avoid
  // DMA coherency artifacts that occur when fb_location is forced on ESP32-S3.
  config.fb_count = 2;
  config.grab_mode = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    cameraAvailable = false;
    return false;
  }
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_framesize(s, (framesize_t)appSettings.camFrameSize);
    s->set_quality(s, appSettings.camQuality);
    // OV3660-tuned defaults
    // AWB & exposure
    s->set_whitebal(s, 1);            // auto white balance on
    s->set_awb_gain(s, 1);            // AWB gain on
    s->set_wb_mode(s, appSettings.camWbMode); // persisted
    s->set_gain_ctrl(s, 1);           // AGC on
    s->set_exposure_ctrl(s, 1);       // AEC on
    s->set_aec2(s, 1);                // AEC2 (improved exposure algorithm) — OV3660 supports this
    s->set_ae_level(s, 0);            // AE compensation -2..2
    s->set_gainceiling(s, GAINCEILING_4X); // OV3660 has good native SNR; keep gain low
    //   for dark enclosures raise to GAINCEILING_8X or GAINCEILING_16X
    // Image corrections (all supported on OV3660)
    s->set_lenc(s, 1);                // lens shading correction
    s->set_bpc(s, 1);                 // black pixel correction
    s->set_wpc(s, 1);                 // white pixel correction
    // NOTE: dcw (downscale crop) is OV2640-only; omitted here
    // Colour & sharpness
    s->set_brightness(s, appSettings.camBrightness);
    s->set_contrast(s, appSettings.camContrast);
    s->set_saturation(s, appSettings.camSaturation);
    s->set_sharpness(s, 1);           // fixed — OV3660 hardware sharpening
    s->set_denoise(s, 1);             // fixed — OV3660 noise reduction
    s->set_special_effect(s, appSettings.camSpecial);
    s->set_colorbar(s, 0);
    s->set_hmirror(s, appSettings.camHmirror ? 1 : 0);
    s->set_vflip(s, appSettings.camVflip ? 1 : 0);
  }
  cameraAvailable = true;
  Serial.println("Camera initialized successfully");
  return true;
}

// MJPEG streaming response for AsyncWebServer
class AsyncJpegStreamResponse : public AsyncAbstractResponse {
public:
  AsyncJpegStreamResponse() {
    _code = 200;
    _contentType = String("multipart/x-mixed-replace; boundary=") + _boundary;
    _sendContentLength = false;
    _chunked = true;
    _fb = nullptr;
    _index = 0;
    _state = State::HEADER;
    _locked = false;
  }
  ~AsyncJpegStreamResponse() override {
    if (_fb) { esp_camera_fb_return(_fb); _fb = nullptr; }
    if (_locked && cameraMutex) { xSemaphoreGive(cameraMutex); _locked = false; }
  }
  bool _sourceValid() const override { return true; }
  size_t _fillBuffer(uint8_t *buf, size_t maxLen) override {
    size_t len = 0;
    while (len < maxLen) {
      if (_state == State::HEADER) {
        if (_fb) { esp_camera_fb_return(_fb); _fb = nullptr; }
        if (cameraMutex && !_locked) { xSemaphoreTake(cameraMutex, portMAX_DELAY); _locked = true; }
        _fb = esp_camera_fb_get();
        if (!_fb) { return len; }
        _index = 0;
        // Prepare part header
        _header = String("--") + _boundary + "\r\n";
        _header += "Content-Type: image/jpeg\r\n";
        _header += "Content-Length: " + String(_fb->len) + "\r\n\r\n";
        _headerSent = 0;
        _state = State::SEND_HEADER;
      }
      if (_state == State::SEND_HEADER) {
        size_t remaining = _header.length() - _headerSent;
        if (remaining > 0) {
          size_t toCopy = std::min(remaining, maxLen - len);
          memcpy(buf + len, _header.c_str() + _headerSent, toCopy);
          _headerSent += toCopy;
          len += toCopy;
          if (len >= maxLen) break;
        }
        if (_headerSent >= _header.length()) {
          _state = State::SEND_FRAME;
        } else {
          break;
        }
      }
      if (_state == State::SEND_FRAME) {
        size_t remaining = _fb->len - _index;
        if (remaining > 0) {
          size_t toCopy = std::min(remaining, maxLen - len);
          memcpy(buf + len, _fb->buf + _index, toCopy);
          _index += toCopy;
          len += toCopy;
          if (len >= maxLen) break;
        }
        if (_index >= _fb->len) {
          _state = State::SEND_TAIL;
        } else {
          break;
        }
      }
      if (_state == State::SEND_TAIL) {
        const char *tail = "\r\n";
        size_t remaining = 2 - _tailSent;
        if (remaining > 0) {
          size_t toCopy = std::min(remaining, maxLen - len);
          memcpy(buf + len, tail + _tailSent, toCopy);
          _tailSent += toCopy;
          len += toCopy;
          if (len >= maxLen) break;
        }
        if (_tailSent >= 2) {
          _tailSent = 0;
          // Completed a frame; update global stream stats
          if (_fb) { camStatFrames++; camStatBytes += _fb->len; }
          if (_locked && cameraMutex) { xSemaphoreGive(cameraMutex); _locked = false; }
          _state = State::HEADER; // next frame
        } else {
          break;
        }
      }
    }
    return len;
  }
private:
  enum class State { HEADER, SEND_HEADER, SEND_FRAME, SEND_TAIL };
  const char* _boundary = "frame";
  camera_fb_t *_fb;
  size_t _index;
  State _state;
  String _header;
  size_t _headerSent = 0;
  size_t _tailSent = 0;
  bool _locked = false;
};

// Optimized calculation functions using fast math
inline float calculateDewPoint(float temp, float humidity) {
  const float a = 17.27f;
  const float b = 237.7f;
  float alpha = ((a * temp) / (b + temp)) + logf(humidity * 0.01f);
  return (b * alpha) / (a - alpha);
}

inline float calculateHeatIndex(float temp, float humidity) {
  if (temp < 27.0f) return temp;
  return 0.5f * (temp + 61.0f + ((temp - 68.0f) * 1.2f) + (humidity * 0.094f));
}

inline float calculateVPD(float temp, float humidity) {
  float es = 0.6108f * expf((17.27f * temp) / (temp + 237.3f));
  float ea = es * humidity * 0.01f;
  return es - ea;
}

inline float calculateAbsoluteHumidity(float temp, float humidity) {
  float es = 0.6108f * expf((17.27f * temp) / (temp + 237.3f));
  float ea = es * humidity * 0.01f;
  return (ea * 2.16679f) / (temp + 273.15f);
}

String getTemperatureStatus(float tempC) {
  // tempC is always in °C from the sensor.
  // Thresholds are always stored in °C (frontend converts before saving).
  if (thresholds.tempMin == 0.0f && thresholds.tempMax == 0.0f) return "Perfect"; // uninitialised
  if (tempC < thresholds.tempMin) return "Too Cold";
  if (tempC > thresholds.tempMax) return "Too Hot";
  return "Perfect";
}

String getHumidityStatus(float humidity) {
  if (humidity < thresholds.humMin) return "Too Dry";
  if (humidity > thresholds.humMax) return "Too Wet";
  // In range [min, max] is Perfect per request
  return "Perfect";
}

void loadAppSettings() {
  appPrefs.begin("app", true);
  appSettings.hostname = appPrefs.getString("host", appSettings.hostname);
  appSettings.units = appPrefs.getString("units", appSettings.units);
  appSettings.tempMin = appPrefs.getFloat("tmin", thresholds.tempMin);
  appSettings.tempMax = appPrefs.getFloat("tmax", thresholds.tempMax);
  appSettings.tempIdeal = appPrefs.getFloat("tideal", thresholds.tempIdeal);
  appSettings.humMin = appPrefs.getFloat("hmin", thresholds.humMin);
  appSettings.humMax = appPrefs.getFloat("hmax", thresholds.humMax);
  appSettings.humIdeal = appPrefs.getFloat("hideal", thresholds.humIdeal);
  appSettings.comfortMin = appPrefs.getFloat("cmin", thresholds.comfortMin);
  appSettings.comfortMax = appPrefs.getFloat("cmax", thresholds.comfortMax);
  appSettings.comfortIdeal = appPrefs.getFloat("cideal", thresholds.comfortIdeal);
  appSettings.camFrameSize  = appPrefs.getInt("camsize", appSettings.camFrameSize);
  appSettings.camQuality    = appPrefs.getInt("camq",    appSettings.camQuality);
  appSettings.camBrightness = appPrefs.getInt("cambr",   appSettings.camBrightness);
  appSettings.camContrast   = appPrefs.getInt("camco",   appSettings.camContrast);
  appSettings.camSaturation = appPrefs.getInt("camsa",   appSettings.camSaturation);
  appSettings.camWbMode     = appPrefs.getInt("camwb",   appSettings.camWbMode);
  appSettings.camSpecial    = appPrefs.getInt("camfx",   appSettings.camSpecial);
  appSettings.camHmirror    = appPrefs.getBool("camhm",  appSettings.camHmirror);
  appSettings.camVflip      = appPrefs.getBool("camvf",  appSettings.camVflip);
  // Persisted installed firmware tag (if any)
  installedFwTag = appPrefs.getString("fw_tag", "");
  appPrefs.end();

  // Apply to runtime thresholds and hostname
  thresholds.tempMin = appSettings.tempMin;
  thresholds.tempMax = appSettings.tempMax;
  thresholds.tempIdeal = appSettings.tempIdeal;
  thresholds.humMin = appSettings.humMin;
  thresholds.humMax = appSettings.humMax;
  thresholds.humIdeal = appSettings.humIdeal;
  thresholds.comfortMin = appSettings.comfortMin;
  thresholds.comfortMax = appSettings.comfortMax;
  thresholds.comfortIdeal = appSettings.comfortIdeal;
  mdnsHostname = appSettings.hostname.length() ? appSettings.hostname : String("momo");
}

void saveAppSettings(const AppSettings &s) {
  appPrefs.begin("app", false);
  appPrefs.putString("host", s.hostname);
  appPrefs.putString("units", s.units);
  appPrefs.putFloat("tmin", s.tempMin);
  appPrefs.putFloat("tmax", s.tempMax);
  appPrefs.putFloat("tideal", s.tempIdeal);
  appPrefs.putFloat("hmin", s.humMin);
  appPrefs.putFloat("hmax", s.humMax);
  appPrefs.putFloat("hideal", s.humIdeal);
  appPrefs.putFloat("cmin", s.comfortMin);
  appPrefs.putFloat("cmax", s.comfortMax);
  appPrefs.putFloat("cideal", s.comfortIdeal);
  appPrefs.putInt("camsize", s.camFrameSize);
  appPrefs.putInt("camq",    s.camQuality);
  appPrefs.putInt("cambr",   s.camBrightness);
  appPrefs.putInt("camco",   s.camContrast);
  appPrefs.putInt("camsa",   s.camSaturation);
  appPrefs.putInt("camwb",   s.camWbMode);
  appPrefs.putInt("camfx",   s.camSpecial);
  appPrefs.putBool("camhm",  s.camHmirror);
  appPrefs.putBool("camvf",  s.camVflip);
  if (installedFwTag.length()) appPrefs.putString("fw_tag", installedFwTag);
  appPrefs.end();
}

void updateStatisticsAtomic(float temp, float humidity, float dewPoint) {
  if (!stats.initialized) {
    stats.tempMin = stats.tempMax = stats.tempAvg = temp;
    stats.humMin = stats.humMax = stats.humAvg = humidity;
    stats.dewMin = stats.dewMax = stats.dewAvg = dewPoint;
    stats.readingCount = 1;
    stats.initialized = true;
    return;
  }
  
  stats.readingCount++;
  float n = stats.readingCount;
  
  // Update averages with exponential smoothing for better performance
  stats.tempAvg = ((n-1) * stats.tempAvg + temp) / n;
  stats.humAvg = ((n-1) * stats.humAvg + humidity) / n;
  stats.dewAvg = ((n-1) * stats.dewAvg + dewPoint) / n;
  
  // Update min/max
  if (temp < stats.tempMin) stats.tempMin = temp;
  if (temp > stats.tempMax) stats.tempMax = temp;
  if (humidity < stats.humMin) stats.humMin = humidity;
  if (humidity > stats.humMax) stats.humMax = humidity;
  if (dewPoint < stats.dewMin) stats.dewMin = dewPoint;
  if (dewPoint > stats.dewMax) stats.dewMax = dewPoint;
}

void updateLEDStatusFast(float temp, float humidity) {
  bool tempOK = (temp >= thresholds.tempMin && temp <= thresholds.tempMax);
  bool humOK = (humidity >= thresholds.humMin && humidity <= thresholds.humMax);
  
  if (tempOK && humOK) {
    digitalWrite(LED_BUILTIN_RED, LOW);   // Turn off red (no alert)
    digitalWrite(LED_BUILTIN_BLUE, HIGH); // Turn on blue (all good)
  } else {
    digitalWrite(LED_BUILTIN_RED, HIGH);  // Turn on red (alert)
    digitalWrite(LED_BUILTIN_BLUE, LOW);  // Turn off blue
  }
}

void printUltraFastReading(EnvironmentData &data) {
  Serial.printf("Temp %.2f°C  RH %.1f%%  DewPt %.1f°C  HeatIdx %.1f°C  VPD %.2f kPa  AbsHum %.2f g/m³\n",
                data.temperature, data.humidity, data.dewPoint,
                data.heatIndex, data.vaporPressureDeficit, data.absoluteHumidity);
}

// Build AP SSID unique per device: ReptiMon-XXYYZZ (last 3 bytes of eFuse MAC)
static String buildApSsid() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[7]; sprintf(buf, "%06X", (uint32_t)(mac & 0xFFFFFF));
  return String("ReptiMon-") + buf;
}
// Build AP password unique per device: 8 hex chars from eFuse MAC bytes 3-6
static String buildApPassword() {
  uint64_t mac = ESP.getEfuseMac();
  char buf[9]; sprintf(buf, "%08X", (uint32_t)((mac >> 8) & 0xFFFFFFFF));
  return String(buf);
}

void setupWiFi() {
  // NOTE: loadAppSettings() already called in setup(); ap_ssid_dyn / ap_password_dyn
  // are initialized in setup() from chip eFuse MAC before this function is called.

  // Load stored credentials (if any)
  preferences.begin("wifi", true);
  String storedSSID = preferences.getString("ssid", "");
  String storedPASS = preferences.getString("pass", "");
  preferences.end();

  if (storedSSID.length() == 0) {
    // No stored credentials → start AP with captive portal immediately
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
    WiFi.softAP(ap_ssid_dyn.c_str(), ap_password_dyn.c_str(), 6 /*channel*/, 0 /*hidden*/, 4 /*max conn*/);
    useAccessPoint = true;
    captivePortalActive = true;
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    Serial.println("\nAccess Point Mode Active");
    Serial.println("===================================================================");
    Serial.print("Network:    "); Serial.println(ap_ssid_dyn);
    Serial.print("Password:   "); Serial.println(ap_password_dyn);
    Serial.print("IP address: "); Serial.println(WiFi.softAPIP());
    Serial.println("Connect to this network then open: http://" + WiFi.softAPIP().toString());
    Serial.println("===================================================================");
    return;
  }

  // Use stored credentials – connect asynchronously (F10: no blocking 15-second loop)
  // loop() monitors wifiConnectPending and handles success (mDNS) or timeout (AP fallback)
  selectedSSID = storedSSID;
  selectedPassword = storedPASS;
  WiFi.mode(WIFI_STA);
  if (selectedPassword.length() == 0) {
    WiFi.begin(selectedSSID.c_str());
  } else {
    WiFi.begin(selectedSSID.c_str(), selectedPassword.c_str());
  }
  Serial.printf("Connecting to '%s' (non-blocking)...\n", selectedSSID.c_str());
  wifiConnectPending = true;
  wifiConnectStart   = millis();
}

String generateJsonData() {
  DynamicJsonDocument doc(2048);

  // Snapshot current sensor data safely under mutex to prevent cross-core data races (F2)
  EnvironmentData snap;
  if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
  memcpy(&snap, (const void*)&currentData, sizeof(EnvironmentData));
  if (dataMutex) xSemaphoreGive(dataMutex);

  // Current readings
  float tempDisplay = snap.temperature;
  if (appSettings.units == "F") {
    tempDisplay = snap.temperature * 9.0f / 5.0f + 32.0f;
  }
  doc["temperature"] = tempDisplay;
  doc["humidity"] = snap.humidity;
  float dewDisplay = snap.dewPoint;
  float heatIdxDisplay = snap.heatIndex;
  if (appSettings.units == "F") {
    dewDisplay = snap.dewPoint * 9.0f / 5.0f + 32.0f;
    heatIdxDisplay = snap.heatIndex * 9.0f / 5.0f + 32.0f;
  }
  doc["dewPoint"] = dewDisplay;
  doc["heatIndex"] = heatIdxDisplay;
  doc["vpd"] = snap.vaporPressureDeficit;
  doc["absoluteHumidity"] = snap.absoluteHumidity;
  doc["timestamp"] = snap.timestamp;
  doc["valid"] = snap.valid;

  // Status indicators
  doc["tempStatus"] = getTemperatureStatus(snap.temperature);
  doc["humStatus"] = getHumidityStatus(snap.humidity);
  doc["units"] = appSettings.units;
  
  // Thresholds
  JsonObject thresh = doc.createNestedObject("thresholds");
  thresh["tempMin"] = thresholds.tempMin;
  thresh["tempMax"] = thresholds.tempMax;
  thresh["tempIdeal"] = thresholds.tempIdeal;
  thresh["humMin"] = thresholds.humMin;
  thresh["humMax"] = thresholds.humMax;
  thresh["humIdeal"] = thresholds.humIdeal;
  thresh["comfortMin"] = thresholds.comfortMin;
  thresh["comfortMax"] = thresholds.comfortMax;
  thresh["comfortIdeal"] = thresholds.comfortIdeal;
  
  // Statistics
  if (stats.initialized) {
    JsonObject st = doc.createNestedObject("stats");
    st["tempMin"] = stats.tempMin;
    st["tempMax"] = stats.tempMax;
    st["tempAvg"] = stats.tempAvg;
    st["humMin"] = stats.humMin;
    st["humMax"] = stats.humMax;
    st["humAvg"] = stats.humAvg;
    st["dewMin"] = stats.dewMin;
    st["dewMax"] = stats.dewMax;
    st["dewAvg"] = stats.dewAvg;
    st["readingCount"] = stats.readingCount;
  }
  
  // System info
  JsonObject sys = doc.createNestedObject("system");
  sys["uptime"] = millis();
  sys["freeHeap"] = ESP.getFreeHeap();
  // Total heap size (bytes)
  #ifdef ESP_ARDUINO_VERSION
  sys["heapSize"] = ESP.getHeapSize();
  #endif
  sys["cpuFreq"] = getCpuFrequencyMhz();
  sys["sensorHz"] = sensorReadCount * 1000.0f / millis();
  sys["displayHz"] = displayUpdateCount * 1000.0f / millis();
  sys["camera"] = cameraAvailable;
  sys["rssi"] = (WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  sys["ip"] = (WiFi.getMode() == WIFI_AP) ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  sys["hostname"] = mdnsHostname;
  sys["mdns"] = mdnsActive;
  // WiFi mode/connection flags
  wifi_mode_t mode = WiFi.getMode();
  const char* modeStr = (mode == WIFI_OFF) ? "off" : (mode == WIFI_STA) ? "sta" : (mode == WIFI_AP) ? "ap" : (mode == WIFI_AP_STA) ? "ap+sta" : "unknown";
  sys["wifiMode"] = modeStr;
  sys["sta"] = (WiFi.status() == WL_CONNECTED);
  sys["ap"] = (mode == WIFI_AP) || (mode == WIFI_AP_STA);
  // PSRAM (bytes)
  sys["psramSize"] = ESP.getPsramSize();
  sys["freePsram"] = ESP.getFreePsram();
  // Flash & sketch (bytes)
  sys["flashSize"] = ESP.getFlashChipSize();
  sys["flashSpeed"] = ESP.getFlashChipSpeed();
  sys["sketchSize"] = ESP.getSketchSize();
  sys["freeSketch"] = ESP.getFreeSketchSpace();
  // LittleFS usage (bytes)
  // Report effective version: prefer installedFwTag when set
  sys["fwVersion"] = (installedFwTag.length() ? installedFwTag : fwVersion);
  sys["fwCommit"] = fwCommit;
  sys["fwBuilt"] = fwBuild;
  sys["fsTotal"] = LittleFS.totalBytes();
  sys["fsUsed"] = LittleFS.usedBytes();
  // Identity
  sys["sdk"] = String(ESP.getSdkVersion());
  #ifdef ARDUINO_ARCH_ESP32
  sys["chipModel"] = String(ESP.getChipModel());
  sys["chipRev"] = ESP.getChipRevision();
  #endif
  // MAC addresses
  sys["macSta"] = WiFi.macAddress();
  sys["macAp"] = WiFi.softAPmacAddress();
  // Convenience fields: prefer STA SSID when connected; else show AP SSID if AP mode is active
  {
    wl_status_t st = WiFi.status();
    wifi_mode_t mode = WiFi.getMode();
    bool apActive = (mode == WIFI_AP) || (mode == WIFI_AP_STA);
    if (st == WL_CONNECTED) {
      doc["ssid"] = selectedSSID;
    } else if (apActive) {
      doc["ssid"] = ap_ssid_dyn;
    } else {
      doc["ssid"] = "";
    }
  }
  
  String jsonString;
  serializeJson(doc, jsonString);
  return jsonString;
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type,
             void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
  Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
      // Send current data immediately
      client->text(generateJsonData());
      break;
      
    case WS_EVT_DISCONNECT:
  Serial.printf("WebSocket client #%u disconnected\n", client->id());
      break;
      
    case WS_EVT_ERROR:
  Serial.printf("WebSocket error from client #%u\n", client->id());
      break;
      
    case WS_EVT_DATA: {
      // Handle incoming data if needed (e.g., threshold updates)
      AwsFrameInfo *info = (AwsFrameInfo*)arg;
      if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
        // Handle text messages here if needed
        String message = "";
        for (size_t i = 0; i < info->len; i++) {
          message += (char) data[i];
        }
  Serial.printf("WebSocket message: %s\n", message.c_str());
      }
      break;
    }
  }
}

void setupWebServer() {
  // Initialize LittleFS for serving static files
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS initialization failed!");
    return;
  }
  Serial.println("LittleFS initialized successfully");
  
  // WebSocket handlers
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  // Camera WebSocket — no event handler needed; camPushTask manages it
  server.addHandler(&camWs);
  
  // Serve static files from data folder
  // Route: if captive portal is active, default to portal.html at root; otherwise index.html
  auto defaultFileSelector = [](AsyncWebServerRequest *request){
    if (captivePortalActive) return String("/portal.html");
    return String("/index.html");
  };
  server.on("/", HTTP_GET, [defaultFileSelector](AsyncWebServerRequest *request){
    String path = defaultFileSelector(request);
    request->send(LittleFS, path, String(), false);
  });
  // Also expose explicit /portal and /index
  server.on("/portal", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(LittleFS, "/portal.html", String(), false); });
  server.on("/index", HTTP_GET, [](AsyncWebServerRequest *request){ request->send(LittleFS, "/index.html", String(), false); });

  // Setup-complete confirmation page — captive WebView navigates here after WiFi connect.
  // A fresh 200 response to a non-portal URL signals iOS/Android that the portal is resolved,
  // triggering the system "Done" button without any redirect to the main dashboard.
  server.on("/done", HTTP_GET, [](AsyncWebServerRequest *request) {
    String staIp = WiFi.localIP().toString();
    String html = R"rawliteral(
<!doctype html><html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,shrink-to-fit=no,viewport-fit=cover">
<title>ReptiMon Connected</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
html,body{background:#080e09;color:#e2ebe0;font:15px/1.5 ui-sans-serif,system-ui,-apple-system,sans-serif;
  min-height:100dvh;display:flex;align-items:center;justify-content:center;}
.wrap{text-align:center;padding:40px 24px;max-width:340px;width:100%}
.check{width:64px;height:64px;border-radius:50%;background:rgba(74,222,128,.12);
  display:flex;align-items:center;justify-content:center;margin:0 auto 20px}
.title{font-size:22px;font-weight:700;color:#4ade80;margin-bottom:8px}
.sub{color:#728c6e;font-size:14px;margin-bottom:24px;line-height:1.6}
.ip{display:inline-block;font-family:ui-monospace,monospace;font-size:15px;font-weight:600;
  background:#131d14;border:1px solid #263a28;border-radius:10px;padding:9px 18px;color:#e2ebe0}
.note{margin-top:20px;font-size:12px;color:#4a6047;line-height:1.6}
</style></head><body>
<div class="wrap">
  <div class="check">
    <svg width="32" height="32" viewBox="0 0 24 24" fill="none" stroke="#4ade80" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round"><polyline points="20 6 9 17 4 12"/></svg>
  </div>
  <div class="title">Connected!</div>
  <div class="sub">ReptiMon is live on your network.<br>Tap <strong style="color:#e2ebe0">Done</strong> to close this screen.</div>
  <div class="ip">)rawliteral";
    html += staIp;
    html += R"rawliteral(</div>
  <div class="note">You can reach the dashboard at the IP above<br>once you reconnect to your home network.</div>
</div>
</body></html>)rawliteral";
    request->send(200, "text/html", html);
  });
  // Static assets under root
  // IMPORTANT: Disable cache for core SPA assets to avoid client-side mismatches after updates
  server.serveStatic("/index.html", LittleFS, "/index.html").setCacheControl("no-cache, no-store, must-revalidate");
  server.serveStatic("/script.js", LittleFS, "/script.js").setCacheControl("no-cache, no-store, must-revalidate");
  server.serveStatic("/style.css", LittleFS, "/style.css").setCacheControl("no-cache, no-store, must-revalidate");
  server.serveStatic("/components", LittleFS, "/components/").setCacheControl("no-cache, no-store, must-revalidate");
  server.serveStatic("/partials", LittleFS, "/partials/").setCacheControl("no-cache, no-store, must-revalidate");
  // Vendor assets — long cache is fine
  server.serveStatic("/vendor", LittleFS, "/vendor/").setCacheControl("public, max-age=86400");
  // NOTE: The root catch-all serveStatic("/") is registered LAST (below, just before server.begin())
  // so that all /api/* routes take precedence over static file lookup.

  // API endpoint for JSON data
  server.on("/api/data", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", generateJsonData());
  });

  // WiFi status endpoint
  server.on("/api/wifi/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    DynamicJsonDocument doc(512);
    bool ap = (WiFi.getMode() == WIFI_AP);
    wl_status_t st = WiFi.status();
    const char* state = ap ? "ap" : (st == WL_CONNECTED ? "connected" : (wifiConnectPending ? "connecting" : "disconnected"));
    doc["state"] = state;
    doc["ssid"] = (st == WL_CONNECTED) ? selectedSSID : "";
    doc["ip"] = ap ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
    doc["ap_ip"]  = WiFi.softAPIP().toString();
    doc["sta_ip"] = WiFi.localIP().toString();
    doc["rssi"] = (st == WL_CONNECTED) ? WiFi.RSSI() : 0;
    doc["hostname"] = mdnsHostname;
    doc["ap"] = ap;
    doc["connecting"] = wifiConnectPending;
    doc["sta_status"] = (int)st;
    if (ap) {
      doc["ap_ssid"] = ap_ssid_dyn;
      doc["captive"] = captivePortalActive;
    }
    // Scan status
    int sc = WiFi.scanComplete();
    if (sc == -1) doc["scan"] = "scanning"; else if (sc >= 0) doc["scan"] = "done"; else doc["scan"] = "idle";
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  // Detailed WiFi info (STA/AP details, MACs, DNS, gateway, subnet, BSSID, channel, tx power, sleep)
  server.on("/api/wifi/info", HTTP_GET, [](AsyncWebServerRequest *request) {
    DynamicJsonDocument doc(1024);
    bool ap = (WiFi.getMode() == WIFI_AP) || (WiFi.getMode() == WIFI_AP_STA);
    bool staEnabled = (WiFi.getMode() == WIFI_STA) || (WiFi.getMode() == WIFI_AP_STA);
    wl_status_t st = WiFi.status();
    doc["mode"] = (WiFi.getMode() == WIFI_AP) ? "ap" : (WiFi.getMode() == WIFI_STA) ? "sta" : (WiFi.getMode() == WIFI_AP_STA) ? "ap+sta" : "off";
    doc["state"] = (st == WL_CONNECTED) ? "connected" : (wifiConnectPending ? "connecting" : (ap ? "ap" : "disconnected"));
    doc["hostname"] = mdnsHostname;
  doc["mdns"] = mdnsActive;
    // STA details
    JsonObject sta = doc.createNestedObject("sta");
    sta["enabled"] = staEnabled;
    sta["ssid"] = (st == WL_CONNECTED) ? selectedSSID : "";
    sta["bssid"] = (st == WL_CONNECTED) ? WiFi.BSSIDstr() : "";
    sta["rssi"] = (st == WL_CONNECTED) ? WiFi.RSSI() : 0;
    sta["channel"] = (st == WL_CONNECTED) ? WiFi.channel() : 0;
    sta["ip"] = WiFi.localIP().toString();
    sta["gateway"] = WiFi.gatewayIP().toString();
    sta["subnet"] = WiFi.subnetMask().toString();
    sta["dns"] = WiFi.dnsIP().toString();
    sta["mac"] = WiFi.macAddress();
    // AP details
    JsonObject apj = doc.createNestedObject("ap");
    apj["enabled"] = ap;
    apj["ssid"] = useAccessPoint ? ap_ssid_dyn : WiFi.softAPSSID();
    apj["ip"] = WiFi.softAPIP().toString();
    apj["mac"] = WiFi.softAPmacAddress();
    apj["clients"] = WiFi.softAPgetStationNum();
    apj["captive"] = captivePortalActive;
    // Radio
    JsonObject radio = doc.createNestedObject("radio");
    auto txEnum = WiFi.getTxPower();
    auto enumToDbm = [](wifi_power_t p)->int{
      switch(p){
        case WIFI_POWER_19_5dBm: return 20;
        case WIFI_POWER_19dBm: return 19;
        case WIFI_POWER_18_5dBm: return 18;
        case WIFI_POWER_17dBm: return 17;
        case WIFI_POWER_15dBm: return 15;
        case WIFI_POWER_13dBm: return 13;
        case WIFI_POWER_11dBm: return 11;
        case WIFI_POWER_8_5dBm: return 9;
        case WIFI_POWER_7dBm: return 7;
        case WIFI_POWER_5dBm: return 5;
        case WIFI_POWER_2dBm: return 2;
        default: return 0;
      }
    };
    radio["tx_power_dbm"] = enumToDbm(txEnum);
    radio["sleep"] = WiFi.getSleep();
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  // WiFi scan start (non-blocking)
  server.on("/api/wifi/scan/start", HTTP_GET, [](AsyncWebServerRequest *request) {
    // Schedule the actual WiFi API calls for loop() so no WiFi stack calls
    // block this async handler (avoids mode-switch timing issues causing WIFI_SCAN_FAILED).
    scanScheduled = true;
    request->send(202, "application/json", "{\"status\":\"started\"}");
  });

  // WiFi scan results
  server.on("/api/wifi/scan/results", HTTP_GET, [](AsyncWebServerRequest *request) {
    DynamicJsonDocument doc(4096);
    int sc = WiFi.scanComplete();
    if (sc == -1) {
      doc["status"] = "scanning";
    } else if (sc == -2) {
      doc["status"] = "failed";
    } else {
      doc["status"] = "done";
      JsonArray arr = doc.createNestedArray("networks");
      for (int i = 0; i < sc; i++) {
        JsonObject o = arr.createNestedObject();
        o["ssid"] = WiFi.SSID(i);
        o["rssi"] = WiFi.RSSI(i);
        o["security"] = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "open" : "secured";
        o["channel"] = WiFi.channel(i);
      }
      WiFi.scanDelete();
    }
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  // WiFi connect endpoint (non-blocking start)
  server.on("/api/wifi/connect", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    DynamicJsonDocument body(512);
    DeserializationError err = deserializeJson(body, data, len);
    if (err) { request->send(400, "application/json", "{\"error\":\"invalid_json\"}"); return; }
    String ssid = body["ssid"] | "";
    String pass = body["password"] | "";
    if (ssid.length() == 0) { request->send(400, "application/json", "{\"error\":\"missing_ssid\"}"); return; }
    // Save credentials and schedule the actual WiFi work for loop() so this response
    // is delivered BEFORE any WiFi stack manipulation (avoids blocking the response).
    pendingSSID = ssid;
    pendingPass = pass;
    connectScheduled = true;
    request->send(202, "application/json", "{\"status\":\"connecting\"}");
  });

  // WiFi reconnect
  server.on("/api/wifi/reconnect", HTTP_POST, [](AsyncWebServerRequest *request){
    bool ok = WiFi.reconnect();
    request->send(200, "application/json", String("{\"status\":\"") + (ok ? "ok" : "failed") + "\"}");
  });

  // WiFi disconnect
  server.on("/api/wifi/disconnect", HTTP_POST, [](AsyncWebServerRequest *request){
    WiFi.disconnect(true, true);
    request->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  // Toggle AP mode (enable/disable AP alongside STA)
  server.on("/api/wifi/toggle_ap", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
    DynamicJsonDocument body(512);
    if (deserializeJson(body, data, len)) { request->send(400, "application/json", "{\"error\":\"invalid_json\"}"); return; }
    bool enable = body["enable"].as<bool>();
    int channel = body.containsKey("channel") ? body["channel"].as<int>() : 6;
    String pass = body.containsKey("password") ? (const char*)body["password"] : ap_password_dyn;
    if (enable) {
      if (WiFi.getMode() == WIFI_STA) WiFi.mode(WIFI_AP_STA); else WiFi.mode(WIFI_AP);
      if (ap_ssid_dyn.length() == 0) {
        uint64_t chipid = ESP.getEfuseMac(); char idbuf[7]; sprintf(idbuf, "%06X", (uint32_t)(chipid & 0xFFFFFF));
        ap_ssid_dyn = String("ReptiMon-") + idbuf;
      }
      WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
      WiFi.softAP(ap_ssid_dyn.c_str(), pass.c_str(), channel, 0, 4);
      useAccessPoint = true;
    } else {
      WiFi.softAPdisconnect(true);
      useAccessPoint = false;
    }
    DynamicJsonDocument resp(256); resp["status"] = "ok"; String out; serializeJson(resp, out); request->send(200, "application/json", out);
  });

  // TX power get
  server.on("/api/wifi/txpower/get", HTTP_GET, [](AsyncWebServerRequest *request){
    auto txEnum = WiFi.getTxPower();
    auto enumToDbm = [](wifi_power_t p)->int{
      switch(p){
        case WIFI_POWER_19_5dBm: return 20;
        case WIFI_POWER_19dBm: return 19;
        case WIFI_POWER_18_5dBm: return 18;
        case WIFI_POWER_17dBm: return 17;
        case WIFI_POWER_15dBm: return 15;
        case WIFI_POWER_13dBm: return 13;
        case WIFI_POWER_11dBm: return 11;
        case WIFI_POWER_8_5dBm: return 9;
        case WIFI_POWER_7dBm: return 7;
        case WIFI_POWER_5dBm: return 5;
        case WIFI_POWER_2dBm: return 2;
        default: return 0;
      }
    };
    DynamicJsonDocument doc(128); doc["tx_power_dbm"] = enumToDbm(txEnum); String out; serializeJson(doc, out); request->send(200, "application/json", out);
  });

  // TX power set (expects integer enum value)
  server.on("/api/wifi/txpower/set", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
    DynamicJsonDocument body(128); if (deserializeJson(body, data, len)) { request->send(400, "application/json", "{\"error\":\"invalid_json\"}"); return; }
    int dbm = body["value"].as<int>();
    auto dbmToEnum = [](int d)->wifi_power_t{
      if (d >= 20) return WIFI_POWER_19_5dBm;
      else if (d >= 19) return WIFI_POWER_19dBm;
      else if (d >= 18) return WIFI_POWER_18_5dBm;
      else if (d >= 17) return WIFI_POWER_17dBm;
      else if (d >= 15) return WIFI_POWER_15dBm;
      else if (d >= 13) return WIFI_POWER_13dBm;
      else if (d >= 11) return WIFI_POWER_11dBm;
      else if (d >= 9) return WIFI_POWER_8_5dBm;
      else if (d >= 7) return WIFI_POWER_7dBm;
      else if (d >= 5) return WIFI_POWER_5dBm;
      else if (d >= 2) return WIFI_POWER_2dBm;
      else return WIFI_POWER_2dBm;
    };
    WiFi.setTxPower(dbmToEnum(dbm));
    request->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  // Sleep get/set
  server.on("/api/wifi/sleep/get", HTTP_GET, [](AsyncWebServerRequest *request){ DynamicJsonDocument d(64); d["sleep"] = WiFi.getSleep(); String o; serializeJson(d,o); request->send(200, "application/json", o); });
  server.on("/api/wifi/sleep/set", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
    DynamicJsonDocument b(64); if (deserializeJson(b,data,len)) { request->send(400, "application/json", "{\"error\":\"invalid_json\"}"); return; }
    bool en = b["enable"].as<bool>(); WiFi.setSleep(en); request->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  // Hostname set (also restarts mDNS)
  server.on("/api/wifi/hostname/set", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
    DynamicJsonDocument b(256); if (deserializeJson(b,data,len)) { request->send(400, "application/json", "{\"error\":\"invalid_json\"}"); return; }
    String hn = b["hostname"] | ""; if (hn.length()==0) { request->send(400, "application/json", "{\"error\":\"missing_hostname\"}"); return; }
    AppSettings ns = appSettings; ns.hostname = hn; saveAppSettings(ns); appSettings = ns; loadAppSettings();
  if (mdnsActive) { MDNS.end(); mdnsActive = false; }
  mdnsActive = MDNS.begin(mdnsHostname.c_str()); if (mdnsActive) { MDNS.addService("http","tcp",80); }
    request->send(200, "application/json", "{\"status\":\"ok\"}");
  });

  // mDNS restart
  server.on("/api/wifi/mdns/restart", HTTP_POST, [](AsyncWebServerRequest *request){
  if (mdnsActive) { MDNS.end(); mdnsActive = false; }
  bool ok = MDNS.begin(mdnsHostname.c_str()); if (ok) { MDNS.addService("http","tcp",80); mdnsActive = true; }
    request->send(200, "application/json", String("{\"status\":\"") + (ok?"ok":"failed") + "\"}");
  });

  // WiFi forget endpoint (clears saved credentials and restarts)
  server.on("/api/wifi/forget", HTTP_POST, [](AsyncWebServerRequest *request){
    preferences.begin("wifi", false);
    preferences.remove("ssid");
    preferences.remove("pass");
    preferences.end();
    request->send(200, "application/json", "{\"status\":\"ok\"}");
    Serial.println("Forget WiFi requested - restarting into AP mode...");
    // Deferred restart so the HTTP response can flush (B7.2)
    xTaskCreate([](void*){ vTaskDelay(pdMS_TO_TICKS(300)); ESP.restart(); vTaskDelete(NULL); },
                "reboot_t", 1024, nullptr, 1, nullptr);
  });

  // Captive portal helpers: respond to common OS connectivity checks.
  // While captive portal is active  → redirect to the portal so the OS shows the login UI.
  // After WiFi connects (captivePortalActive = false) → return the OS-standard "success"
  //   response for each probe so the OS changes its button from "Cancel" to "Done" and
  //   the user can cleanly dismiss the captive browser on their own terms.

  // iOS / macOS probe — expects exact "<HTML>...<BODY>Success</BODY>" to clear captive state
  server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *request){
    if (captivePortalActive) {
      request->send(200, "text/html", "<html><head><meta http-equiv='refresh' content='0; url=/'/></head><body>Redirecting...</body></html>");
    } else {
      request->send(200, "text/html", "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>");
    }
  });

  // Android probe — expects HTTP 204 to signal no captive portal
  auto androidProbe = [](AsyncWebServerRequest *request){
    if (captivePortalActive) {
      request->send(200, "text/html", "<html><head><meta http-equiv='refresh' content='0; url=/'/></head><body>Redirecting...</body></html>");
    } else {
      request->send(204);
    }
  };
  server.on("/generate_204", HTTP_GET, androidProbe);
  server.on("/gen_204",      HTTP_GET, androidProbe);

  // Windows probes
  server.on("/ncsi.txt", HTTP_GET, [](AsyncWebServerRequest *request){
    if (captivePortalActive) {
      request->send(200, "text/html", "<html><head><meta http-equiv='refresh' content='0; url=/'/></head><body>Redirecting...</body></html>");
    } else {
      request->send(200, "text/plain", "Microsoft NCSI");
    }
  });
  server.on("/connecttest.txt", HTTP_GET, [](AsyncWebServerRequest *request){
    if (captivePortalActive) {
      request->send(200, "text/html", "<html><head><meta http-equiv='refresh' content='0; url=/'/></head><body>Redirecting...</body></html>");
    } else {
      request->send(200, "text/plain", "Microsoft Connect Test");
    }
  });
  server.on("/check_network_status.txt", HTTP_GET, [](AsyncWebServerRequest *request){
    if (captivePortalActive) {
      request->send(200, "text/html", "<html><head><meta http-equiv='refresh' content='0; url=/'/></head><body>Redirecting...</body></html>");
    } else {
      request->send(200, "text/plain", "success");
    }
  });
  
  // Handle 404: during captive portal, redirect everything to root (portal)
  server.onNotFound([](AsyncWebServerRequest *request) {
    if (captivePortalActive) {
      request->redirect("/");
    } else {
      request->send(404, "text/plain", "File not found");
    }
  });

  // Settings endpoints
  server.on("/api/settings/get", HTTP_GET, [](AsyncWebServerRequest *request) {
    DynamicJsonDocument doc(1024);
    doc["hostname"] = appSettings.hostname;
    doc["units"] = appSettings.units;
    JsonObject th = doc.createNestedObject("thresholds");
    th["tempMin"] = appSettings.tempMin;
    th["tempMax"] = appSettings.tempMax;
    th["tempIdeal"] = appSettings.tempIdeal;
    th["humMin"] = appSettings.humMin;
    th["humMax"] = appSettings.humMax;
    th["humIdeal"] = appSettings.humIdeal;
    th["comfortMin"] = appSettings.comfortMin;
    th["comfortMax"] = appSettings.comfortMax;
    th["comfortIdeal"] = appSettings.comfortIdeal;
    JsonObject cam = doc.createNestedObject("camera");
    cam["available"]   = cameraAvailable;
    cam["frameSize"]   = appSettings.camFrameSize;
    cam["quality"]     = appSettings.camQuality;
    cam["brightness"]  = appSettings.camBrightness;
    cam["contrast"]    = appSettings.camContrast;
    cam["saturation"]  = appSettings.camSaturation;
    cam["wbMode"]      = appSettings.camWbMode;
    cam["special"]     = appSettings.camSpecial;
    cam["hmirror"]     = appSettings.camHmirror;
    cam["vflip"]       = appSettings.camVflip;
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server.on("/api/settings/save", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
    DynamicJsonDocument body(1024);
    DeserializationError err = deserializeJson(body, data, len);
    if (err) { request->send(400, "application/json", "{\"error\":\"invalid_json\"}"); return; }
    AppSettings ns = appSettings;
    if (body.containsKey("hostname")) ns.hostname = (const char*)body["hostname"];
    if (body.containsKey("units")) ns.units = (const char*)body["units"];
    if (body.containsKey("thresholds")) {
      JsonObject th = body["thresholds"].as<JsonObject>();
      if (th.containsKey("tempMin")) ns.tempMin = th["tempMin"].as<float>();
      if (th.containsKey("tempMax")) ns.tempMax = th["tempMax"].as<float>();
      if (th.containsKey("tempIdeal")) ns.tempIdeal = th["tempIdeal"].as<float>();
      if (th.containsKey("humMin")) ns.humMin = th["humMin"].as<float>();
      if (th.containsKey("humMax")) ns.humMax = th["humMax"].as<float>();
      if (th.containsKey("humIdeal")) ns.humIdeal = th["humIdeal"].as<float>();
      if (th.containsKey("comfortMin")) ns.comfortMin = th["comfortMin"].as<float>();
      if (th.containsKey("comfortMax")) ns.comfortMax = th["comfortMax"].as<float>();
      if (th.containsKey("comfortIdeal")) ns.comfortIdeal = th["comfortIdeal"].as<float>();
    }
    if (body.containsKey("camera")) {
      JsonObject cam = body["camera"].as<JsonObject>();
      // Clamp frame size (0=QCIF .. 13=QSXGA) and JPEG quality (10=best .. 63=worst)
      if (cam.containsKey("frameSize"))  ns.camFrameSize  = constrain(cam["frameSize"].as<int>(), 0, 13);
      if (cam.containsKey("quality"))    ns.camQuality    = constrain(cam["quality"].as<int>(), 10, 63);
      if (cam.containsKey("brightness")) ns.camBrightness = constrain(cam["brightness"].as<int>(), -2, 2);
      if (cam.containsKey("contrast"))   ns.camContrast   = constrain(cam["contrast"].as<int>(), -2, 2);
      if (cam.containsKey("saturation")) ns.camSaturation = constrain(cam["saturation"].as<int>(), -2, 2);
      if (cam.containsKey("wbMode"))     ns.camWbMode     = constrain(cam["wbMode"].as<int>(), 0, 4);
      if (cam.containsKey("special"))    ns.camSpecial    = constrain(cam["special"].as<int>(), 0, 6);
      if (cam.containsKey("hmirror"))    ns.camHmirror    = cam["hmirror"].as<bool>();
      if (cam.containsKey("vflip"))      ns.camVflip      = cam["vflip"].as<bool>();
    }
    // Validate hostname: mDNS-safe characters only (alphanumeric + hyphen), 1-32 chars
    if (ns.hostname.length() == 0 || ns.hostname.length() > 32) ns.hostname = "momo";
    for (int ci = 0; ci < (int)ns.hostname.length(); ci++) {
      char ch = ns.hostname[ci];
      if (!isalnum((unsigned char)ch) && ch != '-') { ns.hostname = "momo"; break; }
    }
    // Clamp thresholds to physically plausible ranges to reject NaN / out-of-bounds values (F18)
    auto clampT = [](float v){ return isnan(v) ? 22.0f : constrain(v, -10.0f, 80.0f); };
    auto clampH = [](float v){ return isnan(v) ? 50.0f : constrain(v,   0.0f, 100.0f); };
    ns.tempMin   = clampT(ns.tempMin);   ns.tempMax   = clampT(ns.tempMax);
    ns.tempIdeal = clampT(ns.tempIdeal);
    ns.humMin    = clampH(ns.humMin);    ns.humMax    = clampH(ns.humMax);
    ns.humIdeal  = clampH(ns.humIdeal);
    // Persist and apply
    saveAppSettings(ns);
    appSettings = ns;
    loadAppSettings();
    // If camera already up and settings changed, try to apply
    if (cameraAvailable) {
      sensor_t *s = esp_camera_sensor_get();
      if (s) {
        s->set_framesize(s,    (framesize_t)appSettings.camFrameSize);
        s->set_quality(s,      appSettings.camQuality);
        s->set_brightness(s,   appSettings.camBrightness);
        s->set_contrast(s,     appSettings.camContrast);
        s->set_saturation(s,   appSettings.camSaturation);
        s->set_wb_mode(s,      appSettings.camWbMode);
        s->set_special_effect(s, appSettings.camSpecial);
        s->set_hmirror(s,      appSettings.camHmirror ? 1 : 0);
        s->set_vflip(s,        appSettings.camVflip   ? 1 : 0);
      }
    }
    DynamicJsonDocument resp(256);
    resp["status"] = "ok";
    String out; serializeJson(resp, out);
    request->send(200, "application/json", out);
  });

  // System operations
  server.on("/api/system/reboot", HTTP_POST, [](AsyncWebServerRequest *request){
    request->send(200, "application/json", "{\"status\":\"rebooting\"}");
    Serial.println("Reboot requested via API");
    // Deferred restart so the HTTP response can flush before the device resets (B7.2)
    xTaskCreate([](void*){ vTaskDelay(pdMS_TO_TICKS(300)); ESP.restart(); vTaskDelete(NULL); },
                "reboot_t", 1024, nullptr, 1, nullptr);
  });

  // Camera endpoints
  server.on("/api/camera/status", HTTP_GET, [](AsyncWebServerRequest *request){
    DynamicJsonDocument doc(256);
    doc["available"] = cameraAvailable;
    String out; serializeJson(doc, out);
    request->send(200, "application/json", out);
  });

  server.on("/api/camera/snapshot", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!cameraAvailable) { request->send(503, "application/json", "{\"error\":\"camera_unavailable\"}"); return; }
    if (cameraMutex) xSemaphoreTake(cameraMutex, portMAX_DELAY);
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      if (cameraMutex) xSemaphoreGive(cameraMutex); // F1: release mutex before error return
      request->send(500, "application/json", "{\"error\":\"capture_failed\"}"); return;
    }
  // Use non-deprecated response API
  AsyncWebServerResponse *response = request->beginResponse(200, String("image/jpeg"), fb->buf, fb->len);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
    esp_camera_fb_return(fb);
    if (cameraMutex) xSemaphoreGive(cameraMutex);
  });

  server.on("/api/camera/stream", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!cameraAvailable) { request->send(503, "application/json", "{\"error\":\"camera_unavailable\"}"); return; }
    AsyncJpegStreamResponse *response = new AsyncJpegStreamResponse();
    response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    response->addHeader("Pragma", "no-cache");
    request->send(response);
  });

  server.on("/api/camera/restart", HTTP_POST, [](AsyncWebServerRequest *request){
    bool ok = true;
    if (cameraAvailable) {
      if (cameraMutex) xSemaphoreTake(cameraMutex, portMAX_DELAY);
      esp_camera_deinit();
      cameraAvailable = false;
      delay(100);
    }
    ok = initCamera();
    if (cameraMutex) xSemaphoreGive(cameraMutex);
    request->send(200, "application/json", String("{\"status\":\"") + (ok?"ok":"failed") + "\"}");
  });

  // Camera stream stats (global)
  server.on("/api/camera/stream_stats", HTTP_GET, [](AsyncWebServerRequest *request){
    DynamicJsonDocument doc(256);
    doc["frames"] = camStatFrames;
    doc["bytes"] = camStatBytes;
    doc["since"] = camStatStartMs;
    doc["now"] = millis();
    String out; serializeJson(doc, out); request->send(200, "application/json", out);
  });

  // Camera controls: adjust common OV3660 parameters at runtime
  server.on("/api/camera/ctrl", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total){
    DynamicJsonDocument body(512);
    if (deserializeJson(body, data, len)) { request->send(400, "application/json", "{\"error\":\"invalid_json\"}"); return; }
    if (cameraMutex) xSemaphoreTake(cameraMutex, portMAX_DELAY);
    sensor_t *s = esp_camera_sensor_get();
    bool ok = (s != nullptr);
    if (ok) {
      if (body.containsKey("wb_mode")) { s->set_wb_mode(s, body["wb_mode"].as<int>()); }
      if (body.containsKey("whitebal")) { s->set_whitebal(s, body["whitebal"].as<bool>()); }
      if (body.containsKey("awb_gain")) { s->set_awb_gain(s, body["awb_gain"].as<bool>()); }
      if (body.containsKey("quality"))  { s->set_quality(s, constrain(body["quality"].as<int>(), 4, 63)); }
      if (body.containsKey("brightness")) { s->set_brightness(s, body["brightness"].as<int>()); }
      if (body.containsKey("contrast")) { s->set_contrast(s, body["contrast"].as<int>()); }
      if (body.containsKey("saturation")) { s->set_saturation(s, body["saturation"].as<int>()); }
      if (body.containsKey("ae_level")) { s->set_ae_level(s, body["ae_level"].as<int>()); }
      if (body.containsKey("aec2")) { s->set_aec2(s, body["aec2"].as<bool>()); }
      if (body.containsKey("gainceiling")) {
        int gc = body["gainceiling"].as<int>();
        switch(gc){
          case 2: s->set_gainceiling(s, GAINCEILING_2X); break;
          case 4: s->set_gainceiling(s, GAINCEILING_4X); break;
          case 8: s->set_gainceiling(s, GAINCEILING_8X); break;
          case 16: s->set_gainceiling(s, GAINCEILING_16X); break;
          case 32: s->set_gainceiling(s, GAINCEILING_32X); break;
          case 64: s->set_gainceiling(s, GAINCEILING_64X); break;
          case 128: s->set_gainceiling(s, GAINCEILING_128X); break;
          default: break;
        }
      }
      if (body.containsKey("lenc")) { s->set_lenc(s, body["lenc"].as<bool>()); }
      if (body.containsKey("bpc")) { s->set_bpc(s, body["bpc"].as<bool>()); }
      if (body.containsKey("wpc")) { s->set_wpc(s, body["wpc"].as<bool>()); }
      if (body.containsKey("dcw")) { s->set_dcw(s, body["dcw"].as<bool>()); }
      if (body.containsKey("hmirror")) { s->set_hmirror(s, body["hmirror"].as<bool>()); }
      if (body.containsKey("vflip")) { s->set_vflip(s, body["vflip"].as<bool>()); }
      if (body.containsKey("special")) { s->set_special_effect(s, body["special"].as<int>()); }
      if (body.containsKey("colorbar")) { s->set_colorbar(s, body["colorbar"].as<bool>()); }
    }
    if (cameraMutex) xSemaphoreGive(cameraMutex);
    request->send(200, "application/json", ok ? "{\"status\":\"ok\"}" : "{\"status\":\"failed\"}");
  });

  // OTA: check latest release on GitHub (supports prereleases, returns FS availability)
  server.on("/api/ota/check", HTTP_GET, [](AsyncWebServerRequest *request){
    String tag, fwUrl, fsUrl, relPage, publishedAt;
    bool ok = getGithubLatest(tag, fwUrl, fsUrl, relPage, publishedAt);
    DynamicJsonDocument d(512);
  String effective = (installedFwTag.length() ? installedFwTag : fwVersion);
  d["current"] = effective;
    d["ok"] = ok;
    d["latest"] = ok ? tag : "";
  d["hasUpdate"] = ok ? (semverCompare(effective, tag) < 0) : false;
    d["hasFs"] = ok ? (fsUrl.length() > 0) : false;
    String repo = String(kGithubOwner) + "/" + String(kGithubRepo);
    d["repo"] = repo;
    d["releaseUrl"] = relPage.length() ? relPage : String("https://github.com/") + repo + "/releases";
    d["publishedAt"] = publishedAt;
    String o; serializeJson(d, o);
    request->send(200, "application/json", o);
  });
  // OTA: apply update from latest release asset (firmware.bin)
  server.on("/api/ota/update", HTTP_POST, [](AsyncWebServerRequest *request){
    request->send(202, "application/json", "{\"status\":\"starting\"}");
    // Run OTA in a separate task to avoid blocking; start even for empty POST bodies
    xTaskCreate([](void*){
      // Atomic OTA lock: non-blocking take; if already taken, another OTA is running (F9)
      if (!otaLock || xSemaphoreTake(otaLock, 0) != pdTRUE) { otaLog("Firmware OTA: already in progress"); vTaskDelete(NULL); return; }
      if (!otaLogMutex) otaLogMutex = xSemaphoreCreateMutex();
      if (otaLogMutex) { xSemaphoreTake(otaLogMutex, portMAX_DELAY); otaLogStart=0; otaLogCount=0; xSemaphoreGive(otaLogMutex); }
      otaLog("Firmware OTA: starting");
      setOtaState("starting", 0);
      String tag, fwUrl, fsUrl, relPage, publishedAt, msg;
      bool ok = getGithubLatest(tag, fwUrl, fsUrl, relPage, publishedAt);
      if (!ok) { otaLog("Firmware OTA: failed to query GitHub"); xSemaphoreGive(otaLock); vTaskDelete(NULL); return; }
      if (semverCompare(fwVersion, tag) < 0 && fwUrl.length()) {
        if (applyOtaFromUrl(fwUrl, msg)) {
          // Persist effective installed version (normalize to drop leading 'v')
          installedFwTag = normalizeVersion(tag);
          appPrefs.begin("app", false); appPrefs.putString("fw_tag", installedFwTag); appPrefs.end();
          otaLog("Firmware OTA: rebooting");
          setOtaState("rebooting", 100);
          vTaskDelay(pdMS_TO_TICKS(300));
          ESP.restart();
        } else {
          otaLog(String("Firmware OTA: ") + msg);
        }
      } else {
        otaLog("Firmware OTA: already up to date");
      }
      xSemaphoreGive(otaLock);
      vTaskDelete(NULL);
    }, "ota_task", 8192, nullptr, 1, nullptr);
  });

  // OTA: update filesystem (LittleFS) from latest release asset when available
  server.on("/api/ota/updatefs", HTTP_POST, [](AsyncWebServerRequest *request){
    request->send(202, "application/json", "{\"status\":\"starting\"}");
    xTaskCreate([](void*){
      if (!otaLock || xSemaphoreTake(otaLock, 0) != pdTRUE) { otaLog("FS OTA: already in progress"); vTaskDelete(NULL); return; }
      if (!otaLogMutex) otaLogMutex = xSemaphoreCreateMutex();
      if (otaLogMutex) { xSemaphoreTake(otaLogMutex, portMAX_DELAY); otaLogStart=0; otaLogCount=0; xSemaphoreGive(otaLogMutex); }
      otaLog("FS OTA: starting");
      setOtaState("starting", 0);
      String tag, fwUrl, fsUrl, relPage, publishedAt, msg;
      bool ok = getGithubLatest(tag, fwUrl, fsUrl, relPage, publishedAt);
      if (!ok) { otaLog("FS OTA: failed to query GitHub"); xSemaphoreGive(otaLock); vTaskDelete(NULL); return; }
      if (fsUrl.length()) {
        if (applyFsOtaFromUrl(fsUrl, msg)) {
          otaLog("FS OTA: rebooting");
          setOtaState("rebooting", 100);
          vTaskDelay(pdMS_TO_TICKS(300));
          ESP.restart();
        } else {
          otaLog(String("FS OTA: ") + msg);
        }
      } else {
        otaLog("FS OTA: no filesystem asset in latest release");
      }
      xSemaphoreGive(otaLock);
      vTaskDelete(NULL);
    }, "ota_fs_task", 8192, nullptr, 1, nullptr);
  });

  // OTA: apply firmware first, then filesystem if available, then restart once
  server.on("/api/ota/update_all", HTTP_POST, [](AsyncWebServerRequest *request){
    request->send(202, "application/json", "{\"status\":\"starting\"}");
    xTaskCreate([](void*){
      if (!otaLock || xSemaphoreTake(otaLock, 0) != pdTRUE) { otaLog("UpdateAll: already in progress"); vTaskDelete(NULL); return; }
      if (!otaLogMutex) otaLogMutex = xSemaphoreCreateMutex();
      if (otaLogMutex) { xSemaphoreTake(otaLogMutex, portMAX_DELAY); otaLogStart=0; otaLogCount=0; xSemaphoreGive(otaLogMutex); }
      otaLog("UpdateAll: starting");
      setOtaState("starting", 0);
      String tag, fwUrl, fsUrl, relPage, publishedAt, msg;
      bool ok = getGithubLatest(tag, fwUrl, fsUrl, relPage, publishedAt);
      if (!ok) { otaLog("UpdateAll: failed to query GitHub"); xSemaphoreGive(otaLock); vTaskDelete(NULL); return; }
      bool didSomething = false;
      // 1) Firmware update if newer
      if (semverCompare(fwVersion, tag) < 0 && fwUrl.length()) {
        otaLogf("UpdateAll: firmware %s -> %s", fwVersion.c_str(), tag.c_str());
        if (applyOtaFromUrl(fwUrl, msg)) {
          installedFwTag = normalizeVersion(tag);
          appPrefs.begin("app", false); appPrefs.putString("fw_tag", installedFwTag); appPrefs.end();
          didSomething = true;
        } else {
          otaLog(String("UpdateAll: firmware: ") + msg);
        }
      } else {
        otaLog("UpdateAll: firmware already up to date");
      }
      // 2) Filesystem update if asset exists
      if (fsUrl.length()) {
        String msg2;
        otaLog("UpdateAll: filesystem asset found");
        if (applyFsOtaFromUrl(fsUrl, msg2)) {
          didSomething = true;
        } else {
          otaLog(String("UpdateAll: filesystem: ") + msg2);
        }
      } else {
        otaLog("UpdateAll: no filesystem asset in latest release");
      }
      if (didSomething) {
        otaLog("UpdateAll: rebooting");
        setOtaState("rebooting", 100);
        vTaskDelay(pdMS_TO_TICKS(300));
        ESP.restart();
      } else {
        otaLog("UpdateAll: nothing to do");
      }
      xSemaphoreGive(otaLock);
      vTaskDelete(NULL);
    }, "ota_all_task", 12288, nullptr, 1, nullptr);
  });

  // OTA logs endpoint: returns recent log lines
  server.on("/api/ota/log", HTTP_GET, [](AsyncWebServerRequest *request){
    DynamicJsonDocument d(4096);
    JsonArray a = d.createNestedArray("lines");
    if (otaLogMutex) xSemaphoreTake(otaLogMutex, portMAX_DELAY);
    for (int i = 0; i < otaLogCount; i++) {
      int idx = (otaLogStart + i) % OTA_LOG_CAP;
      a.add(otaLogBuf[idx]);
    }
    if (otaLogMutex) xSemaphoreGive(otaLogMutex);
    String out; serializeJson(d, out);
    request->send(200, "application/json", out);
  });

  // OTA state endpoint for driving UI progress/messaging
  server.on("/api/ota/state", HTTP_GET, [](AsyncWebServerRequest *request){
    DynamicJsonDocument d(256);
    if (otaStateMutex) xSemaphoreTake(otaStateMutex, portMAX_DELAY);
    d["phase"] = otaPhase;
    d["percent"] = otaPct;
    d["since"] = otaStartMs;
    d["inProgress"] = otaInProgress;
    if (otaPhase == "error" && otaErrorMsg.length()) d["error"] = otaErrorMsg;
    if (otaStateMutex) xSemaphoreGive(otaStateMutex);
    String out; serializeJson(d, out);
    request->send(200, "application/json", out);
  });

  // Full-resolution snapshot with graceful fallback and restoration
  server.on("/api/camera/snapshot_full", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!cameraAvailable) { request->send(503, "application/json", "{\"error\":\"camera_unavailable\"}"); return; }
    int q = 12; // high quality (lower is better)
    if (request->hasParam("q")) {
      q = constrain(request->getParam("q")->value().toInt(), 10, 63);
    }
    // Optional explicit size override via query
    int target = -1;
    if (request->hasParam("size")) { target = request->getParam("size")->value().toInt(); }

    if (cameraMutex) xSemaphoreTake(cameraMutex, portMAX_DELAY);
    sensor_t *s = esp_camera_sensor_get();
    framesize_t prevFS = (framesize_t)appSettings.camFrameSize;
    int prevQ = appSettings.camQuality;
    if (s) {
      // Try UXGA -> SXGA -> XGA unless explicit size is provided
      framesize_t tries[3] = { FRAMESIZE_UXGA, FRAMESIZE_SXGA, FRAMESIZE_XGA };
      bool ok = false;
      camera_fb_t *fb = nullptr;
      if (target >= 0) {
        s->set_framesize(s, (framesize_t)target);
        s->set_quality(s, q);
        fb = esp_camera_fb_get();
        ok = (fb != nullptr);
      } else {
        for (int i = 0; i < 3 && !ok; i++) {
          s->set_framesize(s, tries[i]);
          s->set_quality(s, q);
          fb = esp_camera_fb_get();
          if (fb) ok = true;
        }
      }
      if (ok && fb) {
        // Build filename with resolution
        String fname = "snapshot_" + String(fb->width) + "x" + String(fb->height) + ".jpg";
        auto *resp = request->beginResponse(200, String("image/jpeg"), fb->buf, fb->len);
        resp->addHeader("Cache-Control", "no-store");
        resp->addHeader("Content-Disposition", String("inline; filename=") + fname);
        request->send(resp);
        esp_camera_fb_return(fb);
      } else {
        // Failure at all sizes
        request->send(500, "application/json", "{\"error\":\"fullres_failed\"}");
      }
      // Restore previous settings
      s->set_framesize(s, prevFS);
      s->set_quality(s, prevQ);
    } else {
      request->send(500, "application/json", "{\"error\":\"no_sensor\"}");
    }
    if (cameraMutex) xSemaphoreGive(cameraMutex);
  });

  // Catch-all static handler — MUST be registered after ALL server.on() routes.
  // ESPAsyncWebServer checks handlers in registration order; placing serveStatic("/")
  // before any server.on() would cause those routes to be shadowed and LittleFS
  // would be queried for every /api/* path, spamming vfs errors.
  server.serveStatic("/", LittleFS, "/").setCacheControl("public, max-age=86400");

  server.begin();
  Serial.println("Web server started.");
  Serial.println("Serving static files from LittleFS.");
}

// Web task for sending periodic updates
void webTask(void *parameter) {
  const TickType_t xDelay = pdMS_TO_TICKS(1000); // 1 second updates

  for(;;) {
    // Broadcast in STA mode AND in AP / AP+STA mode so the dashboard stays live
    // even before a WiFi network is joined (F5)
    wifi_mode_t mode = WiFi.getMode();
    bool netReady = (WiFi.status() == WL_CONNECTED)
                 || (mode == WIFI_AP)
                 || (mode == WIFI_AP_STA);
    if (netReady) {
      // Send data to all connected WebSocket clients
      String jsonData = generateJsonData();
      if (jsonData != lastJsonData && ws.count() > 0) {
        ws.textAll(jsonData);
        lastJsonData = jsonData;
        lastWebUpdate = millis();
      }
    }

    vTaskDelay(xDelay);
  }
}

// Sensor reading task - Ultra-high frequency with optimized I2C
void sensorTask(void *parameter) {
  const TickType_t xDelay = pdMS_TO_TICKS(SENSOR_UPDATE_INTERVAL_MS);
  
  for(;;) {
    unsigned long startTime = micros();
    
    if (sht30.read()) {
      EnvironmentData newData;
      newData.temperature = sht30.getTemperature();
      newData.humidity = sht30.getHumidity();
      newData.dewPoint = calculateDewPoint(newData.temperature, newData.humidity);
      newData.heatIndex = calculateHeatIndex(newData.temperature, newData.humidity);
      newData.vaporPressureDeficit = calculateVPD(newData.temperature, newData.humidity);
      newData.absoluteHumidity = calculateAbsoluteHumidity(newData.temperature, newData.humidity);
      newData.timestamp = millis();
      newData.valid = true;
      
      // Mutex-guarded write so readers on other cores see a consistent snapshot (F2)
      if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
      memcpy((void*)&currentData, &newData, sizeof(EnvironmentData));
      if (dataMutex) xSemaphoreGive(dataMutex);
      
      // Store in ring buffer for history
      readings[readingIndex] = newData;
      readingIndex = (readingIndex + 1) % 32;
      
      // Update statistics atomically
      updateStatisticsAtomic(newData.temperature, newData.humidity, newData.dewPoint);
      
      sensorReadCount++;
      lastSensorRead = millis();
      
      // Send to queue for display task
      if (sensorDataQueue != NULL) {
        xQueueSend(sensorDataQueue, &newData, 0);
      }
    }
    
    vTaskDelay(xDelay);
  }
}

// Display task - Moderate frequency for human-readable output
void displayTask(void *parameter) {
  EnvironmentData data;
  const TickType_t xDelay = pdMS_TO_TICKS(DISPLAY_UPDATE_INTERVAL_MS);

  for(;;) {
    // xQueueReceive already blocks for up to xDelay; no second vTaskDelay needed (F11)
    if (xQueueReceive(sensorDataQueue, &data, xDelay) == pdTRUE) {
      if (data.valid) {
        // printUltraFastReading(data); // suppressed — use 'pins' command to read on demand
        displayUpdateCount++;
        lastDisplayUpdate = millis();
      }
    }
  }
}

// LED status task - Fast visual feedback
void ledTask(void *parameter) {
  const TickType_t xDelay = pdMS_TO_TICKS(100); // 10Hz LED updates

  for(;;) {
    // Snapshot under mutex to avoid torn reads across cores (F2)
    bool   valid = false;
    float  temp  = 0.0f;
    float  hum   = 0.0f;
    if (dataMutex) xSemaphoreTake(dataMutex, portMAX_DELAY);
    valid = currentData.valid;
    temp  = currentData.temperature;
    hum   = currentData.humidity;
    if (dataMutex) xSemaphoreGive(dataMutex);

    if (valid) {
      updateLEDStatusFast(temp, hum);
    }

    vTaskDelay(xDelay);
  }
}

// Camera WebSocket push task — grabs JPEG frames and sends them as binary WS messages.
// Running on Core 0 at priority 2; uses vTaskDelayUntil to pace at ~30fps.
// Clients receive raw JPEG bytes and render via URL.createObjectURL for zero-overhead display.
void camPushTask(void *parameter) {
  TickType_t lastWake = xTaskGetTickCount();
  const TickType_t FRAME_TICKS = pdMS_TO_TICKS(33); // ~30 fps target

  for (;;) {
    vTaskDelayUntil(&lastWake, FRAME_TICKS);

    // Nothing to do if no clients or camera unavailable
    if (camWs.count() == 0 || !cameraAvailable) continue;

    // Periodically clean up stale/disconnected clients
    camWs.cleanupClients();

    if (cameraMutex) xSemaphoreTake(cameraMutex, portMAX_DELAY);
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
      if (cameraMutex) xSemaphoreGive(cameraMutex);
      continue;
    }

    // Validate JPEG integrity: must start with SOI (0xFF 0xD8) and end with EOI (0xFF 0xD9).
    // A partial capture or DMA glitch produces frames without these markers — drop them.
    bool validJpeg = (fb->len >= 4)
                  && (fb->buf[0] == 0xFF) && (fb->buf[1] == 0xD8)
                  && (fb->buf[fb->len-2] == 0xFF) && (fb->buf[fb->len-1] == 0xD9);

    if (validJpeg) {
      camWs.binaryAll(fb->buf, fb->len);
      camStatFrames++;
      camStatBytes += fb->len;
    }

    esp_camera_fb_return(fb);
    if (cameraMutex) xSemaphoreGive(cameraMutex);
  }
}

void setup() {
  // Initialize serial with maximum baud rate for data throughput
  Serial.begin(SERIAL_BAUD_RATE);
  Serial.println("\nESP32 XIAO S3 Reptile Monitor with Web Server");
  Serial.println("===================================================================");

  // Load persisted settings early
  loadAppSettings();
  // If no persisted installed tag yet, initialize it from compiled FW_VERSION once
  if (!installedFwTag.length()) {
    installedFwTag = normalizeVersion(fwVersion);
    appPrefs.begin("app", false); appPrefs.putString("fw_tag", installedFwTag); appPrefs.end();
  }

  // Initialize built-in LEDs
  pinMode(LED_BUILTIN_RED, OUTPUT);
  pinMode(LED_BUILTIN_BLUE, OUTPUT);
  digitalWrite(LED_BUILTIN_RED, LOW);
  digitalWrite(LED_BUILTIN_BLUE, LOW);

  // ── Create ALL synchronization primitives before any task or handler can run ──
  // This must happen before setupWiFi() / setupWebServer() so ISR-context handlers
  // and OTA lambdas always find valid semaphore handles. (B1.5 fix)
  sensorDataQueue  = xQueueCreate(16, sizeof(EnvironmentData));
  dataMutex        = xSemaphoreCreateMutex();
  cameraMutex      = xSemaphoreCreateMutex();
  otaStateMutex    = xSemaphoreCreateMutex();
  otaLock          = xSemaphoreCreateMutex();

  if (!sensorDataQueue || !dataMutex || !cameraMutex || !otaStateMutex || !otaLock) {
    Serial.println("FATAL: Failed to create FreeRTOS objects!");
    while (1);
  }

  // Build unique AP credentials from eFuse MAC (F6)
  ap_ssid_dyn     = buildApSsid();
  ap_password_dyn = buildApPassword();
  Serial.printf("AP SSID: %s  Password: %s\n", ap_ssid_dyn.c_str(), ap_password_dyn.c_str());

  // Initialize I2C — auto-detect pin orientation by scanning both combinations.
  // User wiring: yellow=D5(GPIO6), white=D4(GPIO5).
  // Try SDA=5,SCL=6 first; if no device found at 0x44, swap to SDA=6,SCL=5.
  {
    struct { uint8_t sda; uint8_t scl; const char* label; } configs[] = {
      { 5, 6, "SDA=D4(GPIO5) SCL=D5(GPIO6)" },
      { 6, 5, "SDA=D5(GPIO6) SCL=D4(GPIO5) [swapped]" },
    };

    bool busFound = false;
    for (auto& cfg : configs) {
      Wire.end();
      delay(10);
      Wire.begin(cfg.sda, cfg.scl);
      Wire.setClock(I2C_CLOCK_SPEED);
      delay(50); // settle

      Serial.printf("I2C trying: %s\n", cfg.label);
      Wire.beginTransmission(0x44);
      if (Wire.endTransmission() == 0) {
        Serial.printf("  SHT85 found at 0x44 with %s\n", cfg.label);
        busFound = true;
        break;
      } else {
        Serial.println("  0x44 not found on this orientation");
      }
    }

    // Full bus scan after orientation is resolved
    Serial.println("I2C full scan:");
    bool anyFound = false;
    for (uint8_t addr = 1; addr < 127; addr++) {
      Wire.beginTransmission(addr);
      if (Wire.endTransmission() == 0) {
        Serial.printf("  Device at 0x%02X%s\n", addr, addr == 0x44 ? " <- SHT85" : "");
        anyFound = true;
      }
    }
    if (!anyFound) {
      Serial.println("  No I2C devices found on either pin orientation.");
      Serial.println("  -> Check sensor power (3.3V/GND) and add 4.7k pull-ups on SDA+SCL.");
    }
  }

  // Initialize SHT85 sensor — retry up to 5 times before giving up
  {
    bool sensorOk = false;
    for (int attempt = 0; attempt < 5 && !sensorOk; attempt++) {
      if (sht30.begin()) {
        sensorOk = true;
      } else {
        Serial.printf("SHT85 init attempt %d/5 failed, retrying...\n", attempt + 1);
        digitalWrite(LED_BUILTIN_RED, HIGH);
        delay(500);
        digitalWrite(LED_BUILTIN_RED, LOW);
        delay(500);
      }
    }
    if (sensorOk) {
      Serial.println("SHT85 sensor initialized successfully");
    } else {
      Serial.println("SHT85 init failed after 5 attempts — continuing without sensor");
      // Device still functions (AP + web) — sensor data will be marked invalid
    }
  }

  // Setup WiFi (non-blocking)
  WiFi.persistent(false);
  WiFi.setSleep(false);
  setupWiFi();

  // Initialize camera (best-effort; cameraMutex already created above)
  initCamera();
  camStatFrames = 0; camStatBytes = 0; camStatStartMs = millis();

  // Setup web server
  setupWebServer();

  // Create high-priority tasks with optimized stack sizes
  // Camera WebSocket push task — Core 0, priority 2, dedicated to streaming frames
  xTaskCreatePinnedToCore(
    camPushTask,
    "CamPushTask",
    4096,
    NULL,
    2,
    NULL,
    0
  );

  xTaskCreatePinnedToCore(
    sensorTask,           // Task function
    "SensorTask",         // Task name
    4096,                 // Stack size
    NULL,                 // Parameters
    3,                    // Priority (high)
    &sensorTaskHandle,    // Task handle
    1                     // Core 1
  );

  xTaskCreatePinnedToCore(
    displayTask,          // Task function
    "DisplayTask",        // Task name
    4096,                 // Stack size
    NULL,                 // Parameters
    2,                    // Priority (medium)
    &displayTaskHandle,   // Task handle
    0                     // Core 0
  );

  xTaskCreatePinnedToCore(
    ledTask,              // Task function
    "LEDTask",            // Task name
    2048,                 // Stack size
    NULL,                 // Parameters
    1,                    // Priority (low)
    &ledTaskHandle,       // Task handle
    0                     // Core 0
  );

  xTaskCreatePinnedToCore(
    webTask,              // Task function
    "WebTask",            // Task name
    8192,                 // Stack size (larger for JSON)
    NULL,                 // Parameters
    2,                    // Priority (medium)
    &webTaskHandle,       // Task handle
    0                     // Core 0
  );

  // Enable hardware watchdog — 30-second timeout resets the chip if loop() stalls (F8)
  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL); // watch the Arduino loop() task

  Serial.printf("CPU Frequency: %d MHz\n", getCpuFrequencyMhz());
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("Target sensor rate: %.1f Hz\n", 1000.0f / SENSOR_UPDATE_INTERVAL_MS);
  Serial.printf("Target display rate: %.1f Hz\n", 1000.0f / DISPLAY_UPDATE_INTERVAL_MS);
  Serial.println("===================================================================");
  Serial.println("Web interface ready. Open your browser and navigate to the IP address shown above.");
  Serial.println("Access from any device on your network.");
  Serial.println("Monitoring started - real-time reptile environment data");
  Serial.println("===================================================================\n");
}

void loop() {
  // Reset hardware watchdog at the top of every loop iteration (F8)
  esp_task_wdt_reset();

  // Handle serial commands for system control
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    command.trim();
    String rawCommand = command;  // preserve original case for ssid/password extraction
    command.toLowerCase();
    
    if (command == "stats") {
  Serial.println("\nSYSTEM PERFORMANCE STATISTICS");
      Serial.println("===================================================================");
      Serial.printf("Sensor readings: %lu (%.1fHz)\n", sensorReadCount,
                    sensorReadCount * 1000.0f / millis());
      Serial.printf("Display updates: %lu (%.1fHz)\n", displayUpdateCount,
                    displayUpdateCount * 1000.0f / millis());
      Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
      Serial.printf("Min free heap: %d bytes\n", ESP.getMinFreeHeap());
      Serial.printf("CPU frequency: %d MHz\n", getCpuFrequencyMhz());
      Serial.printf("Uptime: %.1f minutes\n", millis() / 60000.0f);
      Serial.printf("WiFi status: %s\n", WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
      Serial.printf("WebSocket clients: %d\n", ws.count());
      Serial.println("===================================================================\n");
    }
    else if (command == "reset") {
      ESP.restart();
    }
    else if (command == "wifi") {
      if (WiFi.status() == WL_CONNECTED) {
  Serial.printf("WiFi: %s\n", selectedSSID.c_str());
  Serial.printf("IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("RSSI: %d dBm\n", WiFi.RSSI());
      } else {
  Serial.println("WiFi not connected");
      }
    }
    else if (command == "ap") {
      Serial.println("\nACCESS POINT CREDENTIALS:");
      Serial.println("===================================================================");
      Serial.printf("SSID:     %s\n", ap_ssid_dyn.c_str());
      Serial.printf("Password: %s\n", ap_password_dyn.c_str());
      Serial.printf("AP IP:    %s\n", WiFi.softAPIP().toString().c_str());
      Serial.printf("AP active: %s\n", useAccessPoint ? "Yes" : "No");
      Serial.println("===================================================================\n");
    }
    else if (command == "pins") {
      Serial.println("\nI2C / SENSOR DIAGNOSTICS");
      Serial.println("===================================================================");
      Serial.printf("I2C config   : SDA=GPIO5 (D4, yellow)  SCL=GPIO6 (D5, white)\n");
      Serial.printf("I2C clock    : %d Hz\n", I2C_CLOCK_SPEED);
      Serial.printf("Sensor addr  : 0x44 (SHT85)\n");

      // Live GPIO read
      Serial.printf("D4 (SDA) raw : %d\n", digitalRead(5));
      Serial.printf("D5 (SCL) raw : %d\n", digitalRead(6));

      // Live I2C bus scan
      Serial.println("I2C bus scan :");
      bool anyFound = false;
      for (uint8_t addr = 1; addr < 127; addr++) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0) {
          Serial.printf("  0x%02X detected%s\n", addr, addr == 0x44 ? " <- SHT85 OK" : "");
          anyFound = true;
        }
      }
      if (!anyFound) Serial.println("  No devices found — check wiring and pull-up resistors");

      // Current sensor state
      Serial.println("Sensor data  :");
      if (currentData.valid) {
        Serial.printf("  Temperature : %.2f C\n", currentData.temperature);
        Serial.printf("  Humidity    : %.2f %%\n", currentData.humidity);
        Serial.printf("  Dew point   : %.2f C\n", currentData.dewPoint);
        Serial.printf("  Heat index  : %.2f C\n", currentData.heatIndex);
        Serial.printf("  VPD         : %.3f kPa\n", currentData.vaporPressureDeficit);
        Serial.printf("  Abs. hum.   : %.2f g/m3\n", currentData.absoluteHumidity);
        Serial.printf("  Read count  : %lu\n", sensorReadCount);
        Serial.printf("  Last read   : %lu ms ago\n", millis() - lastSensorRead);
      } else {
        Serial.println("  No valid reading — sensor may not have initialised");
      }
      Serial.println("===================================================================\n");
    }
    else if (command.startsWith("ssid ")) {
      // Format: ssid <network-name> <password>
      // Password may be omitted for open networks.
      String rest = rawCommand.substring(5); // strip "ssid "
      rest.trim();
      int spaceIdx = rest.indexOf(' ');
      String newSsid, newPass;
      if (spaceIdx < 0) {
        // No password supplied
        newSsid = rest;
        newPass  = "";
      } else {
        newSsid = rest.substring(0, spaceIdx);
        newPass  = rest.substring(spaceIdx + 1);
        newPass.trim();
      }
      if (newSsid.length() == 0) {
        Serial.println("Usage: ssid <network-name> <password>");
      } else {
        Serial.printf("[WiFi] Queuing connection to '%s'...\n", newSsid.c_str());
        pendingSSID     = newSsid;
        pendingPass     = newPass;
        connectScheduled = true;
      }
    }
    else if (command == "help") {
      Serial.println("\nAVAILABLE COMMANDS:");
      Serial.println("ap                    - Show AP SSID and password");
      Serial.println("pins                  - I2C pin state, bus scan, live sensor reading");
      Serial.println("ssid <name> <pass>    - Connect to a WiFi network (omit pass for open)");
      Serial.println("stats                 - Show performance statistics");
      Serial.println("wifi                  - Show WiFi information");
      Serial.println("scan                  - Scan for WiFi networks");
      Serial.println("reset                 - Restart the system");
      Serial.println("help                  - Show this help\n");
    }
    else if (command == "scan") {
  Serial.println("Restarting to scan networks...");
      delay(1000);
      ESP.restart();
    }
  }
  
  // Handle deferred WiFi scan (avoids mode-switch timing issue in async handler)
  if (scanScheduled) {
    scanScheduled = false;
    if (useAccessPoint) {
      if (WiFi.getMode() != WIFI_AP_STA) {
        WiFi.mode(WIFI_AP_STA);
        vTaskDelay(pdMS_TO_TICKS(150));
      }
    } else if (WiFi.getMode() != WIFI_STA) {
      WiFi.mode(WIFI_STA);
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    WiFi.scanDelete();
    WiFi.scanNetworks(true /* async */, true /* show_hidden */);
    Serial.println("[WiFi] Scan started from loop()");
  }

  // Handle deferred WiFi connect (avoids blocking response delivery in async handler)
  if (connectScheduled) {
    connectScheduled = false;
    if (useAccessPoint) {
      if (WiFi.getMode() != WIFI_AP_STA) {
        WiFi.mode(WIFI_AP_STA);
        vTaskDelay(pdMS_TO_TICKS(100));
      }
    } else {
      WiFi.mode(WIFI_STA);
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    WiFi.disconnect(true, true);
    vTaskDelay(pdMS_TO_TICKS(200));
    if (pendingPass.length() == 0) {
      WiFi.begin(pendingSSID.c_str());
    } else {
      WiFi.begin(pendingSSID.c_str(), pendingPass.c_str());
    }
    wifiConnectPending = true;
    wifiConnectStart = millis();
    Serial.printf("[WiFi] Connecting to '%s' from loop()\n", pendingSSID.c_str());
  }

  // Finalize non-blocking WiFi connect
  if (wifiConnectPending) {
    wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
      // Save credentials
      preferences.begin("wifi", false);
      preferences.putString("ssid", pendingSSID);
      preferences.putString("pass", pendingPass);
      preferences.end();
      selectedSSID = pendingSSID;
      selectedPassword = pendingPass;
      wifiConnectPending = false;
      // Notify portal clients BEFORE stopping AP so the message can be received
      {
        DynamicJsonDocument doc(256);
        doc["event"] = "wifi_connected";
        doc["ip"]    = WiFi.localIP().toString();
        doc["ap_ip"] = WiFi.softAPIP().toString(); // AP IP so portal can navigate back to it
        String msg; serializeJson(doc, msg);
        ws.textAll(msg);
      }
      // Stop captive DNS redirect so the OS stops treating this as a captive portal,
      // but KEEP the AP running so the captive browser stays connected and can
      // receive the WebSocket event and display the Connected view.
      // Schedule AP teardown after 90s — enough time for the user to open the dashboard.
      if (captivePortalActive) {
        dnsServer.stop();
        captivePortalActive = false;
        apShutdownAt = millis() + 12000UL; // shut AP down in 12s — after portal auto-redirects
      }
      // Start mDNS for LAN discovery
      mdnsActive = MDNS.begin(mdnsHostname.c_str());
      if (!mdnsActive) {
        Serial.println("mDNS failed to start after connect");
      } else {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("mDNS active: http://%s.local\n", mdnsHostname.c_str());
      }
      Serial.println("WiFi connected (non-blocking flow)");
  } else if (millis() - wifiConnectStart > 25000) { // timeout
      wifiConnectPending = false;
      // Revert to AP — ap_ssid_dyn and ap_password_dyn were set in setup() (B3.2)
      // Give the AP a dedicated gateway IP so captive-portal clients have a predictable target
      WiFi.softAPConfig(
        IPAddress(192, 168, 4, 1),   // AP IP / gateway
        IPAddress(192, 168, 4, 1),   // gateway
        IPAddress(255, 255, 255, 0)  // subnet
      );
      WiFi.softAP(ap_ssid_dyn.c_str(), ap_password_dyn.c_str());
      useAccessPoint = true;
      captivePortalActive = true;
      dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
      DynamicJsonDocument doc(256);
      doc["event"] = "wifi_failed";
      doc["reason"] = "timeout";
      doc["ap_ip"] = WiFi.softAPIP().toString();
      String msg; serializeJson(doc, msg);
      ws.textAll(msg);
  Serial.println("WiFi connect timeout; reverted to AP");
    }
  }

  // Process captive portal DNS if active
  if (captivePortalActive) {
    dnsServer.processNextRequest();
  }

  // Tear down AP after setup-complete grace period
  if (apShutdownAt > 0 && millis() >= apShutdownAt) {
    apShutdownAt = 0;
    if (useAccessPoint) {
      WiFi.softAPdisconnect(true);
      useAccessPoint = false;
      Serial.println("Setup AP shut down after grace period.");
    }
  }

  // System health monitoring
  static unsigned long lastHealthCheck = 0;
  if (millis() - lastHealthCheck > 30000) { // Every 30 seconds
    lastHealthCheck = millis();
    
    // Check if tasks are running
    if (sensorTaskHandle == NULL || displayTaskHandle == NULL || ledTaskHandle == NULL || webTaskHandle == NULL) {
  Serial.println("Task failure detected!");
    }
    
    // Memory check
    if (ESP.getFreeHeap() < 50000) { // Less than 50KB free
  Serial.printf("Low memory: %d bytes free\n", ESP.getFreeHeap());
    }
    
    // WiFi check — only attempt reconnect when we're in STA mode (not in intentional AP mode)
    wifi_mode_t wmode = WiFi.getMode();
    bool staMode = (wmode == WIFI_STA) || (wmode == WIFI_AP_STA);
    if (staMode && !wifiConnectPending && WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi disconnected, attempting reconnection...");
      WiFi.reconnect();
    }
  }
  
  // Clean WebSocket connections
  ws.cleanupClients();

  // Small yield to prevent the Arduino task from starving lower-priority IDLE hooks
  delay(10);
}
