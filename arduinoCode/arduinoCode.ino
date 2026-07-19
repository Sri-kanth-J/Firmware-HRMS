#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <fpm.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <DIYables_TFT_SPI.h>
#include "secrets.h" // GITHUB_PAT -- copy secrets.h.example to secrets.h and fill in a real token

/* ---------------- Factory defaults ----------------
   Used only the very first time the device boots, before anything has been
   saved to flash. After that, these are overridden by whatever was entered
   through the "Device Setup" captive portal (WiFiManager) and persisted to
   NVS. Keep these as the values for a brand-new, unconfigured unit. */
#define DEFAULT_BASE_URL "base-url"
#define DEFAULT_API_KEY "api-key"
#define DEFAULT_API_SECRET "api-secret"

/* ---------------- Firmware / OTA update config ----------------
   FIRMWARE_VERSION must match the GitHub Release tag (a leading "v" is
   stripped before comparing) for the device to consider itself up to date.
   To ship an update: bump this, push a tag matching it (e.g. "v1.0.1") to
   OTA_GITHUB_OWNER/OTA_GITHUB_REPO, and CI builds + attaches OTA_ASSET_NAME
   to the release. Devices only check when "Check Update" is selected from
   the menu (see checkForOTAUpdate()) -- there's no background polling.

   NOTE: OTA_GITHUB_REPO is private, so every device carries GITHUB_PAT
   (from secrets.h) to read it. Scope that token to read-only "Contents"
   access on ONLY this repo -- it's baked into every unit's firmware, so a
   dumped/decompiled device exposes it. */
#define FIRMWARE_VERSION "1.0.0"
#define OTA_GITHUB_OWNER "Sri-kanth-J"
#define OTA_GITHUB_REPO  "Firmware-HRMS"
#define OTA_ASSET_NAME   "firmware.bin"
#define OTA_HTTP_TIMEOUT_MS 10000

/* Runtime config -- loaded from NVS (Preferences) at boot, editable anytime via
   the "Device Setup" menu item, which opens a WiFiManager captive portal
   with two separate pages: "Configure WiFi" natively handles the SSID/
   password (WiFiManager stores those itself), while "Configure Backend"
   handles the fields below, added as custom parameters on their own portal
   page (via setParamsPage) and persisted here ourselves. */
String deviceApiKey;
String deviceApiSecret;
String baseUrl;
String serverUrl;
String commandsNextUrl;

Preferences prefs;
bool shouldSaveConfig = false;

WiFiClient secureClient;

#define HTTP_CONNECT_TIMEOUT_MS 8000
#define HTTP_RESPONSE_TIMEOUT_MS 8000

/* Remote command queue: the backend queues ENROLL/DELETE commands (from the HR/Admin
   portal) that this device polls for and executes. */
#define POLL_INTERVAL_MS 20000
unsigned long lastPollMs = 0;

#define MAX_PENDING_ENROLLS 5
long pendingEnrollCommandIds[MAX_PENDING_ENROLLS];
long pendingEnrollEmployeeIds[MAX_PENDING_ENROLLS];
uint8_t pendingEnrollCount = 0;

/* 2.8" TFT SPI pins -- same wiring as the DIYables reference sketch.
   MOSI/SCK/MISO use the ESP32's default hardware (VSPI) pins: 23/18/19. */
#define TFT_CS_PIN   5
#define TFT_DC_PIN   2
#define TFT_RST_PIN  4
#define TFT_WIDTH    240
#define TFT_HEIGHT   320

/* R503 UART pins on ESP32 */
#define RXD2 16
#define TXD2 17
#define SENSOR_BAUD 57600

/* Peripherals */
/* NOTE: moved from GPIO23 -- that pin is now hardware SPI MOSI for the TFT.
   Also moved off GPIO27 to make room for the rotary encoder's SW pin below. */
#define BUZZER_PIN 32

/* Rotary encoder: CLK/DT give quadrature menu navigation, SW is the
   push-to-select button. Replaces the old discrete up/down/select buttons. */
#define ENCODER_CLK_PIN 25
#define ENCODER_DT_PIN  26
#define ENCODER_SW_PIN  27

/* Wi-Fi connection wait */
#define WIFI_CONNECT_TIMEOUT_MS 15000

// Uncomment the line matching your panel's driver chip (same as the reference sketch)
// DIYables_ILI9341_SPI TFT_display(TFT_WIDTH, TFT_HEIGHT, TFT_CS_PIN, TFT_DC_PIN, TFT_RST_PIN);
// DIYables_ILI9488_SPI TFT_display(TFT_WIDTH, TFT_HEIGHT, TFT_CS_PIN, TFT_DC_PIN, TFT_RST_PIN);
DIYables_ST7789_SPI  TFT_display(TFT_WIDTH, TFT_HEIGHT, TFT_CS_PIN, TFT_DC_PIN, TFT_RST_PIN);

#define TFT_BLACK  DIYables_TFT_SPI::colorRGB(0, 0, 0)
#define TFT_WHITE  DIYables_TFT_SPI::colorRGB(255, 255, 255)
#define TFT_RED    DIYables_TFT_SPI::colorRGB(255, 0, 0)
#define TFT_GREEN  DIYables_TFT_SPI::colorRGB(0, 200, 0)
#define TFT_BLUE   DIYables_TFT_SPI::colorRGB(0, 0, 255)
#define TFT_CYAN   DIYables_TFT_SPI::colorRGB(64, 224, 208)
#define TFT_VIOLET DIYables_TFT_SPI::colorRGB(148, 0, 211)
#define TFT_GRAY   DIYables_TFT_SPI::colorRGB(120, 120, 120)

HardwareSerial sensorSerial(2);
FPM finger(&sensorSerial);
FPMSystemParams params;

bool sensorReady = false;
bool haveParams = false;

/* R503 Aura LED control values */
#define R503_LED_BREATHING 0x01
#define R503_LED_FLASHING  0x02
#define R503_LED_ALWAYS_ON 0x03
#define R503_LED_ALWAYS_OFF 0x04

/* Common R503 color codes */
#define R503_CLR_RED    0x01
#define R503_CLR_BLUE   0x02
#define R503_CLR_PURPLE 0x03
#define R503_CLR_GREEN  0x04
#define R503_CLR_YELLOW 0x05
#define R503_CLR_CYAN   0x06
#define R503_CLR_WHITE  0x07

/* Status "icon" kinds -- drawn as simple vector shapes on the TFT (see
   drawStatusIcon), replacing the old 16x16 monochrome OLED bitmaps. */
enum StatusIcon { STATUS_FINGER, STATUS_OK, STATUS_ERROR, STATUS_WIFI };

const char *menuItems[] = {"Log In", "Pending Enrolls", "Device Setup", "Check Update"};
const uint8_t menuCount = sizeof(menuItems) / sizeof(menuItems[0]);
uint8_t menuIndex = 0;

void setSensorAura(uint8_t control, uint8_t speed, uint8_t color, uint8_t cycles) {
  if (!sensorReady) return;
  (void)finger.ledConfigure(control, speed, color, cycles);
}

void setAuraIdle() { setSensorAura(R503_LED_BREATHING, 60, R503_CLR_BLUE, 0); }
void setAuraProcessing() { setSensorAura(R503_LED_BREATHING, 40, R503_CLR_PURPLE, 0); }
void setAuraSuccess() { setSensorAura(R503_LED_FLASHING, 25, R503_CLR_GREEN, 2); }
void setAuraError() { setSensorAura(R503_LED_FLASHING, 20, R503_CLR_RED, 3); }
void setAuraEnroll() { setSensorAura(R503_LED_BREATHING, 45, R503_CLR_YELLOW, 0); }
void setAuraWiFi() { setSensorAura(R503_LED_BREATHING, 50, R503_CLR_CYAN, 0); }

/* ---------------- UI helpers ---------------- */
void toneMs(uint16_t ms) {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(ms);
  digitalWrite(BUZZER_PIN, LOW);
}

void beepSuccess() {
  toneMs(90);
  delay(45);
  toneMs(90);
}

void beepError() { toneMs(350); }

/* Maps a status kind to its accent/banner color. */
uint16_t statusColor(StatusIcon icon) {
  switch (icon) {
    case STATUS_OK:    return TFT_GREEN;
    case STATUS_ERROR: return TFT_RED;
    case STATUS_WIFI:  return TFT_BLUE;
    case STATUS_FINGER:
    default:           return TFT_CYAN;
  }
}

/* Draws a small recognizable shape for each status kind at (x, y), roughly
   `size` px across. Replaces the old 16x16 OLED bitmaps -- simple lines,
   rects and circles instead of a monochrome sprite. */
void drawStatusIcon(StatusIcon icon, int x, int y, int size, uint16_t color) {
  switch (icon) {
    case STATUS_OK:
      TFT_display.drawLine(x - size, y, x - size / 3, y + size / 2, color);
      TFT_display.drawLine(x - size / 3, y + size / 2, x + size, y - size / 2, color);
      break;
    case STATUS_ERROR:
      TFT_display.drawLine(x - size / 2, y - size / 2, x + size / 2, y + size / 2, color);
      TFT_display.drawLine(x + size / 2, y - size / 2, x - size / 2, y + size / 2, color);
      break;
    case STATUS_WIFI:
      TFT_display.fillRect(x - size / 2, y + size / 4, size / 4, size / 4, color);
      TFT_display.fillRect(x - size / 6, y, size / 4, size / 2, color);
      TFT_display.fillRect(x + size / 6, y - size / 4, size / 4, (size * 3) / 4, color);
      break;
    case STATUS_FINGER:
    default:
      TFT_display.drawCircle(x, y, size, color);
      TFT_display.drawCircle(x, y, (size * 2) / 3, color);
      TFT_display.drawCircle(x, y, size / 3, color);
      break;
  }
}

void showStatus(StatusIcon icon, const char *line1, const char *line2) {
  uint16_t accent = statusColor(icon);

  TFT_display.fillScreen(TFT_WHITE);

  // Top banner: accent color, icon + short label
  int bannerH = 56;
  TFT_display.fillRect(0, 0, TFT_display.width(), bannerH, accent);
  drawStatusIcon(icon, 28, bannerH / 2, 16, TFT_WHITE);
  TFT_display.setTextColor(TFT_WHITE);
  TFT_display.setTextSize(2);
  TFT_display.setCursor(56, bannerH / 2 - 8);
  TFT_display.print(line1);

  // Main message, large
  TFT_display.setTextColor(TFT_BLACK);
  TFT_display.setTextSize(3);
  TFT_display.setCursor(20, bannerH + 40);
  TFT_display.print(line2);
}

void drawMenu() {
  TFT_display.fillScreen(TFT_WHITE);

  // Header banner
  int bannerH = 44;
  TFT_display.fillRect(0, 0, TFT_display.width(), bannerH, TFT_VIOLET);
  TFT_display.setTextColor(TFT_WHITE);
  TFT_display.setTextSize(2);
  TFT_display.setCursor(10, 12);
  TFT_display.print("Attendance");

  // Wi-Fi status line
  bool wifiOn = (WiFi.status() == WL_CONNECTED);
  TFT_display.setTextSize(2);
  TFT_display.setTextColor(TFT_BLACK);
  TFT_display.setCursor(10, bannerH + 12);
  TFT_display.print("WiFi: ");
  TFT_display.setTextColor(wifiOn ? TFT_GREEN : TFT_RED);
  TFT_display.print(wifiOn ? "ON" : "OFF");

  // Menu items, selected one highlighted
  int itemH = 44;
  int startY = bannerH + 46;
  for (uint8_t i = 0; i < menuCount; i++) {
    int y = startY + i * itemH;
    bool selected = (i == menuIndex);

    if (selected) {
      TFT_display.fillRect(0, y, TFT_display.width(), itemH - 6, TFT_CYAN);
    }
    TFT_display.setTextColor(selected ? TFT_BLACK : TFT_GRAY);
    TFT_display.setTextSize(2);
    TFT_display.setCursor(16, y + 10);
    TFT_display.print(menuItems[i]);
    if (i == 1) {
      TFT_display.print(" (");
      TFT_display.print(pendingEnrollCount);
      TFT_display.print(")");
    }
  }
}

/* ---------------- Rotary encoder handling ---------------- */
/* Quadrature decode table -- index = (previous AB state << 2) | current AB
   state, value = position delta for that transition. Invalid/bounce
   transitions decode to 0. */
static const int8_t QUAD_TABLE[16] = {
   0, -1, +1,  0,
  +1,  0,  0, -1,
  -1,  0,  0, +1,
   0, +1, -1,  0
};

// Variables touched inside an ISR must be volatile.
volatile int32_t encoderCount = 0;  // raw quadrature ticks
volatile uint8_t encoderState = 0;  // last 2-bit AB state
volatile bool encoderButtonPressed = false;
volatile uint32_t lastEncoderButtonMs = 0;

void IRAM_ATTR handleEncoderISR() {
  uint8_t a = digitalRead(ENCODER_CLK_PIN);
  uint8_t b = digitalRead(ENCODER_DT_PIN);
  uint8_t currentState = (a << 1) | b;
  uint8_t index = (encoderState << 2) | currentState;

  encoderCount += QUAD_TABLE[index];
  encoderState = currentState;
}

void IRAM_ATTR handleEncoderButtonISR() {
  uint32_t now = millis(); // acceptable in Arduino-ESP32 ISR for simple debounce usage
  if (now - lastEncoderButtonMs > 200) {
    encoderButtonPressed = true;
    lastEncoderButtonMs = now;
  }
}

int32_t readEncoderCount() {
  noInterrupts();
  int32_t value = encoderCount;
  interrupts();
  return value;
}

// This encoder reports 4 quadrature ticks per detent (click) -- divide down
// to whole menu steps so one click of the knob moves the menu by one item.
int32_t readEncoderDetents() {
  return readEncoderCount() / 4;
}

/* ---------------- Remote configuration ---------------- */
/* Rebuilds the derived endpoint URLs any time baseUrl changes. */
void deriveUrls() {
  serverUrl = baseUrl + "/api/attendance";
  commandsNextUrl = baseUrl + "/api/devices/commands/next";
}

/* Reads saved config from NVS, falling back to factory defaults the first
   time the device ever boots (before anything has been saved). */
void loadConfig() {
  prefs.begin("fpmcfg", true);
  deviceApiKey = prefs.getString("apikey", DEFAULT_API_KEY);
  deviceApiSecret = prefs.getString("apisecret", DEFAULT_API_SECRET);
  baseUrl = prefs.getString("baseurl", DEFAULT_BASE_URL);
  prefs.end();
  deriveUrls();
}

void saveConfig() {
  prefs.begin("fpmcfg", false);
  prefs.putString("apikey", deviceApiKey);
  prefs.putString("apisecret", deviceApiSecret);
  prefs.putString("baseurl", baseUrl);
  prefs.end();
  Serial.println("[CONFIG] Saved device config to NVS");
}

void saveConfigCallback() {
  shouldSaveConfig = true;
}

void configPortalStartedCallback(WiFiManager *wmPtr) {
  setAuraWiFi();
  showStatus(STATUS_WIFI, "Setup Mode", "Device-Setup");
}

/* Single portal, two menu entries: connecting to the "Device-Setup" access
   point opens one WiFiManager captive portal whose root page offers
   "Configure WiFi" (native SSID/password page) and "Configure Backend" (our
   Device API Key / Device API Secret / Backend Base URL fields, on their own
   page via setParamsPage so they never show up on the Wi-Fi page). Either,
   both, or neither can be filled in during the same session.

   forcePortal=false: try whatever Wi-Fi credentials WiFiManager already has
   saved; only fall back to opening the AP + portal if that connection fails.
   Used at boot.
   forcePortal=true: always open the portal immediately. Used by the "Setup"
   menu item so Wi-Fi and/or backend config can be changed on demand, not
   just on first boot. */
bool runConfigPortal(bool forcePortal) {
  WiFiManager wm;
  wm.setSaveParamsCallback(saveConfigCallback); // fires when the "Configure Backend" page is saved
  wm.setAPCallback(configPortalStartedCallback);
  wm.setConfigPortalTimeout(180); // give up and continue offline after 3 min unattended

  WiFiManagerParameter custom_apikey("apikey", "Device API Key", deviceApiKey.c_str(), 40);
  WiFiManagerParameter custom_apisecret("apisecret", "Device API Secret", deviceApiSecret.c_str(), 64);
  WiFiManagerParameter custom_baseurl("baseurl", "Backend Base URL", baseUrl.c_str(), 80);
  wm.addParameter(&custom_apikey);
  wm.addParameter(&custom_apisecret);
  wm.addParameter(&custom_baseurl);

  wm.setParamsPage(true); // keep the backend fields off the Wi-Fi page, on their own page instead
  wm.setCustomMenuHTML("<form action='/param' method='get'><button>Configure Backend</button></form><br/>\n");
  std::vector<const char *> menu = {"wifi", "custom", "info", "exit"};
  wm.setMenu(menu); // adds our "Configure Backend" button next to the native "Configure WiFi" one

  shouldSaveConfig = false;
  bool connected;
  if (forcePortal) {
    showStatus(STATUS_WIFI, "Setup Mode", "Starting AP");
    delay(400);
    connected = wm.startConfigPortal("Device-Setup");
  } else {
    connected = wm.autoConnect("Device-Setup");
  }

  // Backend fields are only in play if the "Configure Backend" page was used --
  // check that regardless of whether Wi-Fi itself was touched this session.
  if (shouldSaveConfig) {
    deviceApiKey = String(custom_apikey.getValue());
    deviceApiSecret = String(custom_apisecret.getValue());
    baseUrl = String(custom_baseurl.getValue());
    deriveUrls();
    saveConfig();
  }

  if (connected) {
    setAuraSuccess();
    showStatus(STATUS_OK, "Wi-Fi", "Connected");
    delay(700);
  } else if (shouldSaveConfig) {
    setAuraSuccess();
    showStatus(STATUS_OK, "Backend", "Saved");
    delay(700);
  } else {
    setAuraError();
    showStatus(STATUS_ERROR, "Setup", "Timed Out");
    delay(900);
  }
  setAuraIdle();
  return connected;
}

/* ---------------- OTA firmware update ---------------- */
/* Strips a leading 'v'/'V' from a GitHub release tag ("v1.2.0" -> "1.2.0")
   so it can be string-compared against FIRMWARE_VERSION. */
String stripVersionPrefix(const String &tag) {
  if (tag.length() > 0 && (tag[0] == 'v' || tag[0] == 'V')) return tag.substring(1);
  return tag;
}

/* Streams `assetApiUrl` (a GitHub release asset API URL) into the ESP32's
   OTA flash slot via the Update library, showing rough progress on the TFT.
   Returns true only once the full image has been written and verified --
   the caller is responsible for rebooting into it.

   Private-repo asset downloads respond 302 to a pre-signed, unauthenticated
   URL on a different host. That redirect is followed manually (rather than
   via HTTPClient's setFollowRedirects) specifically so GITHUB_PAT is never
   sent to that second host -- only the first request carries it. */
bool otaDownloadAndFlash(const String &assetApiUrl) {
  WiFiClientSecure metaClient;
  metaClient.setInsecure(); // no cert pinning -- trades TLS server verification for simplicity
  metaClient.setTimeout(OTA_HTTP_TIMEOUT_MS);

  HTTPClient http;
  http.setConnectTimeout(OTA_HTTP_TIMEOUT_MS);
  http.setTimeout(OTA_HTTP_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS); // redirect handled manually below
  http.begin(metaClient, assetApiUrl);
  http.addHeader("Authorization", String("token ") + GITHUB_PAT);
  http.addHeader("Accept", "application/octet-stream");
  http.addHeader("User-Agent", "FPM-Attendance-Device");

  int code = http.GET();
  String redirectUrl;
  if (code == HTTP_CODE_FOUND || code == HTTP_CODE_MOVED_PERMANENTLY || code == HTTP_CODE_TEMPORARY_REDIRECT) {
    redirectUrl = http.getLocation();
  }
  http.end();

  WiFiClientSecure dlClient;
  HTTPClient dlHttp;
  dlClient.setInsecure();
  dlClient.setTimeout(OTA_HTTP_TIMEOUT_MS);
  dlHttp.setConnectTimeout(OTA_HTTP_TIMEOUT_MS);
  dlHttp.setTimeout(OTA_HTTP_TIMEOUT_MS);

  if (redirectUrl.length() > 0) {
    dlHttp.begin(dlClient, redirectUrl); // no Authorization header -- URL is already pre-signed
    code = dlHttp.GET();
  } else {
    // No redirect (e.g. a public repo) -- the first response IS the binary,
    // but its stream was already consumed by http.end() above, so re-request.
    dlHttp.begin(dlClient, assetApiUrl);
    dlHttp.addHeader("Authorization", String("token ") + GITHUB_PAT);
    dlHttp.addHeader("Accept", "application/octet-stream");
    dlHttp.addHeader("User-Agent", "FPM-Attendance-Device");
    code = dlHttp.GET();
  }

  if (code != HTTP_CODE_OK) {
    Serial.printf("[OTA] Download request failed, HTTP %d\n", code);
    dlHttp.end();
    return false;
  }

  int contentLength = dlHttp.getSize();
  bool sizeKnown = (contentLength > 0);

  if (!Update.begin(sizeKnown ? (size_t)contentLength : UPDATE_SIZE_UNKNOWN)) {
    Serial.printf("[OTA] Update.begin() failed: %s\n", Update.errorString());
    dlHttp.end();
    return false;
  }

  WiFiClient *stream = dlHttp.getStreamPtr();
  uint8_t buf[1024];
  size_t written = 0;
  int lastProgressPct = -1;
  uint32_t lastDataMs = millis();

  while (dlHttp.connected() && (!sizeKnown || written < (size_t)contentLength)) {
    int avail = stream->available();
    if (avail <= 0) {
      if (millis() - lastDataMs > OTA_HTTP_TIMEOUT_MS) break;
      delay(5);
      continue;
    }

    size_t toRead = (avail < (int)sizeof(buf)) ? (size_t)avail : sizeof(buf);
    size_t got = stream->readBytes(buf, toRead);
    if (got == 0) break;
    if (Update.write(buf, got) != got) {
      Serial.println("[OTA] Flash write mismatch, aborting");
      break;
    }
    written += got;
    lastDataMs = millis();

    if (sizeKnown) {
      int pct = (int)((written * 100) / contentLength);
      if (pct != lastProgressPct && pct % 5 == 0) {
        lastProgressPct = pct;
        char pctLine[8];
        snprintf(pctLine, sizeof(pctLine), "%d%%", pct);
        showStatus(STATUS_WIFI, "Updating", pctLine);
      }
    }
  }
  dlHttp.end();

  bool complete = sizeKnown ? (written == (size_t)contentLength) : (written > 0);
  if (!complete) {
    Serial.printf("[OTA] Incomplete download: %u/%d bytes\n", (unsigned)written, contentLength);
    Update.abort();
    return false;
  }

  if (!Update.end(true)) {
    Serial.printf("[OTA] Update.end() failed: %s\n", Update.errorString());
    return false;
  }

  return true;
}

/* "Check Update" menu item: queries the latest release on OTA_GITHUB_REPO,
   compares its tag to FIRMWARE_VERSION, and if different, downloads +
   flashes OTA_ASSET_NAME via otaDownloadAndFlash and reboots into it. */
void checkForOTAUpdate() {
  showStatus(STATUS_WIFI, "OTA", "Checking...");

  if (WiFi.status() != WL_CONNECTED) {
    setAuraError();
    showStatus(STATUS_ERROR, "OTA", "No WiFi");
    beepError();
    delay(1200);
    setAuraIdle();
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(OTA_HTTP_TIMEOUT_MS);

  HTTPClient http;
  http.setConnectTimeout(OTA_HTTP_TIMEOUT_MS);
  http.setTimeout(OTA_HTTP_TIMEOUT_MS);

  String url = String("https://api.github.com/repos/") + OTA_GITHUB_OWNER + "/" + OTA_GITHUB_REPO + "/releases/latest";
  http.begin(client, url);
  http.addHeader("Authorization", String("token ") + GITHUB_PAT);
  http.addHeader("Accept", "application/vnd.github+json");
  http.addHeader("User-Agent", "FPM-Attendance-Device");

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[OTA] releases/latest HTTP %d\n", code);
    http.end();
    setAuraError();
    showStatus(STATUS_ERROR, "OTA", "Check Failed");
    beepError();
    delay(1200);
    setAuraIdle();
    return;
  }

  // Filter keeps only the fields we need, to bound RAM use regardless of how
  // many assets or how much release-notes text the response carries.
  StaticJsonDocument<192> filter;
  filter["tag_name"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["url"] = true;

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();

  if (err) {
    Serial.print("[OTA] JSON parse error: "); Serial.println(err.c_str());
    setAuraError();
    showStatus(STATUS_ERROR, "OTA", "Bad Reply");
    beepError();
    delay(1200);
    setAuraIdle();
    return;
  }

  String remoteVersion = stripVersionPrefix(String((const char *)(doc["tag_name"] | "")));
  if (remoteVersion.length() == 0) {
    setAuraError();
    showStatus(STATUS_ERROR, "OTA", "No Release");
    beepError();
    delay(1200);
    setAuraIdle();
    return;
  }

  if (remoteVersion == FIRMWARE_VERSION) {
    setAuraSuccess();
    showStatus(STATUS_OK, "OTA", "Up To Date");
    delay(1200);
    setAuraIdle();
    return;
  }

  String assetUrl;
  for (JsonObject asset : doc["assets"].as<JsonArray>()) {
    if (String((const char *)(asset["name"] | "")) == OTA_ASSET_NAME) {
      assetUrl = String((const char *)(asset["url"] | ""));
      break;
    }
  }

  if (assetUrl.length() == 0) {
    setAuraError();
    showStatus(STATUS_ERROR, "OTA", "No Asset");
    beepError();
    delay(1200);
    setAuraIdle();
    return;
  }

  showStatus(STATUS_WIFI, "OTA", "Downloading");
  bool ok = otaDownloadAndFlash(assetUrl);

  if (ok) {
    setAuraSuccess();
    showStatus(STATUS_OK, "OTA", "Rebooting");
    beepSuccess();
    delay(1200);
    ESP.restart();
  } else {
    setAuraError();
    showStatus(STATUS_ERROR, "OTA", "Update Failed");
    beepError();
    delay(1500);
    setAuraIdle();
  }
}

/* Lightweight reconnect for transient Wi-Fi drops during normal operation.
   Reuses whatever credentials WiFiManager already saved -- this never opens
   the setup portal, so a dropped connection can't interrupt attendance
   logging with an unexpected captive-portal prompt. Use the "Device Setup"
   menu item (runConfigPortal) to actually change credentials. */
bool quickReconnectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  setAuraWiFi();
  showStatus(STATUS_WIFI, "Network", "Reconnecting");
  WiFi.reconnect();

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT_MS) {
    delay(150);
  }

  if (WiFi.status() == WL_CONNECTED) {
    setAuraSuccess();
    showStatus(STATUS_OK, "Wi-Fi", "Connected");
    delay(700);
    setAuraIdle();
    return true;
  }

  setAuraError();
  showStatus(STATUS_ERROR, "Wi-Fi", "Failed");
  delay(900);
  setAuraIdle();
  return false;
}

bool initFingerprintSensor() {
  sensorSerial.begin(SENSOR_BAUD, SERIAL_8N1, RXD2, TXD2);
  delay(150);

  if (!finger.begin()) {
    sensorReady = false;
    return false;
  }

  FPMStatus st = finger.readParams(&params);
  haveParams = (st == FPMStatus::OK);
  sensorReady = true;
  setAuraIdle();
  return true;
}

/* ---------------- Fingerprint actions ---------------- */
bool waitForPlaceFingerAndConvert(uint8_t slot, const char *promptLine) {
  setAuraProcessing();
  showStatus(STATUS_FINGER, promptLine, "Place finger");

  uint32_t start = millis();
  while ((millis() - start) < 15000) {
    FPMStatus st = finger.getImage();
    if (st == FPMStatus::NOFINGER) {
      delay(35);
      continue;
    }

    if (st != FPMStatus::OK) {
      setAuraError();
      showStatus(STATUS_ERROR, "Sensor", "Read Err");
      beepError();
      setAuraIdle();
      return false;
    }

    st = finger.image2Tz(slot);
    if (st != FPMStatus::OK) {
      setAuraError();
      showStatus(STATUS_ERROR, "Sensor", "Convert Err");
      beepError();
      setAuraIdle();
      return false;
    }

    setAuraSuccess();
    return true;
  }

  setAuraError();
  showStatus(STATUS_ERROR, "Timeout", "No Finger");
  beepError();
  setAuraIdle();
  return false;
}

void waitFingerRemoved() {
  uint32_t start = millis();
  while ((millis() - start) < 6000) {
    if (finger.getImage() == FPMStatus::NOFINGER) return;
    delay(50);
  }
}

bool findFreeTemplateId(uint16_t *freeId) {
  uint16_t capacity = haveParams ? params.capacity : 200;
  uint16_t pages = (capacity / FPM_TEMPLATES_PER_PAGE) + 1;

  for (uint16_t page = 0; page < pages; page++) {
    int16_t id = -1;
    FPMStatus st = finger.getFreeIndex((uint8_t)page, &id);
    if (st != FPMStatus::OK) return false;

    if (id >= 0) {
      *freeId = (uint16_t)id;
      return true;
    }
  }
  return false;
}

bool captureAndStoreTemplate(uint16_t *outSlot) {
  setAuraEnroll();
  if (!findFreeTemplateId(outSlot)) {
    setAuraError();
    showStatus(STATUS_ERROR, "Enroll", "DB Full");
    beepError();
    setAuraIdle();
    return false;
  }

  char idLine[20];
  snprintf(idLine, sizeof(idLine), "Slot %u", *outSlot);
  showStatus(STATUS_FINGER, "Enroll Start", idLine);
  delay(800);

  if (!waitForPlaceFingerAndConvert(1, "Step 1/2")) return false;

  showStatus(STATUS_FINGER, "Lift finger", "and wait");
  waitFingerRemoved();
  delay(400);

  if (!waitForPlaceFingerAndConvert(2, "Step 2/2")) return false;

  FPMStatus st = finger.generateTemplate();
  if (st != FPMStatus::OK) {
    setAuraError();
    showStatus(STATUS_ERROR, "Enroll", "No Match");
    beepError();
    setAuraIdle();
    return false;
  }

  st = finger.storeTemplate(*outSlot, 1);
  if (st != FPMStatus::OK) {
    setAuraError();
    showStatus(STATUS_ERROR, "Enroll", "Store Err");
    beepError();
    setAuraIdle();
    return false;
  }

  setAuraSuccess();
  showStatus(STATUS_OK, "Enroll OK", idLine);
  beepSuccess();
  delay(1200);
  setAuraIdle();
  return true;
}

void sendAttendanceLog(uint16_t id, uint16_t score) {
  if (WiFi.status() != WL_CONNECTED) {
    quickReconnectWiFi();
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[HTTP] Cannot send payload: Wi-Fi Disconnected!");
    setAuraError();
    showStatus(STATUS_ERROR, "Network", "Offline");
    beepError();
    delay(900);
    setAuraIdle();
    return;
  }

  HTTPClient http;
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_RESPONSE_TIMEOUT_MS);

  Serial.println("\n--- [HTTP POST START] ---");
  Serial.print("[HTTP] Target URL: "); Serial.println(serverUrl);

  http.begin(secureClient, serverUrl);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", deviceApiKey);
  http.addHeader("X-Device-Secret", deviceApiSecret);

  String payload = String("{\"fingerprint_id\":") + String(id) + ",\"score\":" + String(score) + "}";
  Serial.print("[HTTP] Sending JSON Payload: "); Serial.println(payload);

  int code = http.POST(payload);
  Serial.print("[HTTP] Response Status Code: "); Serial.println(code);

  // 🌟 FIX: Only parse if the server explicitly confirms with HTTP 200 OK
  if (code == 200) {
    String serverResponse = http.getString(); 
    Serial.print("[HTTP] Saved Response Data: "); Serial.println(serverResponse);

    // 🌟 FIX: Stream string parsing optimization
    if (serverResponse.indexOf("IN") >= 0) {
      Serial.println("[UI] Match state found: Welcome User");
      showStatus(STATUS_OK, "Welcome", "Logged IN");
    } 
    else if (serverResponse.indexOf("OUT") >= 0) {
      Serial.println("[UI] Match state found: Goodbye User");
      showStatus(STATUS_OK, "Goodbye", "Logged OUT");
    } 
    else {
      Serial.println("[UI] Match state fallback: Generic Record");
      showStatus(STATUS_OK, "Success", "Recorded");
    }
    setAuraSuccess();
    beepSuccess();
  } else {
    // Catch-all block handles server errors safely without fake success screens
    char codeLine[20];
    snprintf(codeLine, sizeof(codeLine), "Code %d", code);
    setAuraError();
    showStatus(STATUS_ERROR, "Server Error", codeLine);
    beepError();
  }

  http.end();
  Serial.println("--- [HTTP POST END] ---\n");
  delay(1300);
  setAuraIdle();
}

/* ---------------- Remote command queue ---------------- */
/* Hand-rolled JSON field extraction (no ArduinoJson dependency) -- matches this
   codebase's existing style of string-searching HTTP response bodies. Only works
   against the flat, known response shapes returned by /devices/commands/next. */
long extractJsonLong(const String &json, const char *key) {
  String pattern = String("\"") + key + "\":";
  int idx = json.indexOf(pattern);
  if (idx < 0) return -1;
  idx += pattern.length();
  if (json.startsWith("null", idx)) return -1;

  int end = idx;
  while (end < (int)json.length() && (isDigit(json[end]) || json[end] == '-')) end++;
  if (end == idx) return -1;
  return json.substring(idx, end).toInt();
}

String extractJsonString(const String &json, const char *key) {
  String pattern = String("\"") + key + "\":\"";
  int idx = json.indexOf(pattern);
  if (idx < 0) return String("");
  idx += pattern.length();
  int end = json.indexOf("\"", idx);
  if (end < 0) return String("");
  return json.substring(idx, end);
}

void ackCommand(long commandId, bool success, long resultSlot) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_RESPONSE_TIMEOUT_MS);
  String url = baseUrl + "/api/devices/commands/" + String(commandId) + "/ack";
  http.begin(secureClient, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", deviceApiKey);
  http.addHeader("X-Device-Secret", deviceApiSecret);

  String payload;
  if (success) {
    payload = String("{\"status\":\"DONE\",\"result_slot_id\":") + String(resultSlot) + "}";
  } else {
    payload = "{\"status\":\"FAILED\",\"failure_reason\":\"device could not complete command\"}";
  }

  int code = http.POST(payload);
  Serial.print("[ACK] Command "); Serial.print(commandId);
  Serial.print(" -> status code "); Serial.println(code);
  http.end();
}

void queuePendingEnroll(long commandId, long employeeId) {
  for (uint8_t i = 0; i < pendingEnrollCount; i++) {
    if (pendingEnrollCommandIds[i] == commandId) return; // already queued
  }
  if (pendingEnrollCount >= MAX_PENDING_ENROLLS) return;
  pendingEnrollCommandIds[pendingEnrollCount] = commandId;
  pendingEnrollEmployeeIds[pendingEnrollCount] = employeeId;
  pendingEnrollCount++;
}

void handleRemoteDelete(long commandId, long targetSlot) {
  if (targetSlot < 0 || (!sensorReady && !initFingerprintSensor())) {
    ackCommand(commandId, false, -1);
    return;
  }

  FPMStatus st = finger.deleteTemplate((uint16_t)targetSlot, 1);
  bool ok = (st == FPMStatus::OK);

  if (ok) {
    setAuraSuccess();
    showStatus(STATUS_OK, "Deleted", "Slot removed");
    beepSuccess();
  } else {
    setAuraError();
    showStatus(STATUS_ERROR, "Delete", "Failed");
    beepError();
  }
  delay(900);
  setAuraIdle();

  ackCommand(commandId, ok, -1);
}

void pollForCommands() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_RESPONSE_TIMEOUT_MS);
  http.begin(secureClient, commandsNextUrl);
  http.addHeader("X-Device-Key", deviceApiKey);
  http.addHeader("X-Device-Secret", deviceApiSecret);

  int code = http.GET();
  if (code != 200) {
    http.end();
    return;
  }

  String body = http.getString();
  http.end();

  if (body.indexOf("\"command\":null") >= 0) return;

  long commandId = extractJsonLong(body, "id");
  if (commandId < 0) return;

  String commandType = extractJsonString(body, "command_type");
  long employeeId = extractJsonLong(body, "employee_id");
  long targetSlot = extractJsonLong(body, "target_slot_id");

  Serial.print("[COMMAND] id="); Serial.print(commandId);
  Serial.print(" type="); Serial.print(commandType);
  Serial.print(" employee="); Serial.println(employeeId);

  if (commandType == "DELETE") {
    handleRemoteDelete(commandId, targetSlot);
  } else if (commandType == "ENROLL") {
    queuePendingEnroll(commandId, employeeId);
  }
}

void doRemoteEnroll() {
  if (pendingEnrollCount == 0) return;
  if (!sensorReady && !initFingerprintSensor()) {
    setAuraError();
    showStatus(STATUS_ERROR, "Sensor", "Missing");
    beepError();
    return;
  }

  long commandId = pendingEnrollCommandIds[0];
  long employeeId = pendingEnrollEmployeeIds[0];

  char empLine[20];
  snprintf(empLine, sizeof(empLine), "Emp #%ld", employeeId);
  showStatus(STATUS_FINGER, "Enroll For", empLine);
  delay(1200);

  uint16_t newSlot = 0;
  bool ok = captureAndStoreTemplate(&newSlot);
  ackCommand(commandId, ok, ok ? (long)newSlot : -1);

  // Remove from the pending list regardless of outcome -- a failed capture can be
  // re-requested by HR from the portal, which queues a fresh command.
  for (uint8_t i = 1; i < pendingEnrollCount; i++) {
    pendingEnrollCommandIds[i - 1] = pendingEnrollCommandIds[i];
    pendingEnrollEmployeeIds[i - 1] = pendingEnrollEmployeeIds[i];
  }
  pendingEnrollCount--;
}

bool doLogin() {
  if (!sensorReady && !initFingerprintSensor()) {
    setAuraError();
    showStatus(STATUS_ERROR, "Sensor", "Missing");
    beepError();
    return false;
  }

  if (!waitForPlaceFingerAndConvert(1, "Login")) return false;

  uint16_t id = 0;
  uint16_t score = 0;
  
  // 🌟 FIX: Arguments reordered cleanly to match structural FPM framework definitions
  FPMStatus st = finger.searchDatabase(&id, &score, 1);

  if (st == FPMStatus::OK) {
    char idLine[20];
    snprintf(idLine, sizeof(idLine), "ID %u", id);
    setAuraSuccess();
    showStatus(STATUS_OK, "Match", idLine);
    beepSuccess();
    delay(350);
    sendAttendanceLog(id, score);
    waitFingerRemoved();
    return true;
  }

  if (st == FPMStatus::NOTFOUND) {
    setAuraError();
    showStatus(STATUS_ERROR, "Denied", "No Match");
  } else {
    setAuraError();
    showStatus(STATUS_ERROR, "Sensor", "Search Err");
  }
  beepError();
  waitFingerRemoved();
  delay(900);
  setAuraIdle();
  return false;
}

/* ---------------- Arduino lifecycle ---------------- */
void setup() {
  Serial.begin(115200);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  pinMode(ENCODER_CLK_PIN, INPUT_PULLUP);
  pinMode(ENCODER_DT_PIN, INPUT_PULLUP);
  pinMode(ENCODER_SW_PIN, INPUT_PULLUP);

  // Seed the starting AB state before enabling interrupts so the first
  // transition decodes correctly.
  uint8_t initA = digitalRead(ENCODER_CLK_PIN);
  uint8_t initB = digitalRead(ENCODER_DT_PIN);
  encoderState = (initA << 1) | initB;

  attachInterrupt(digitalPinToInterrupt(ENCODER_CLK_PIN), handleEncoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_DT_PIN), handleEncoderISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENCODER_SW_PIN), handleEncoderButtonISR, FALLING);

  // DIYables_TFT_SPI's begin() doesn't return a success flag (unlike the old
  // SSD1306 library), so there's no equivalent hard-stop check here -- wiring
  // problems will just show as a blank/garbled screen.
  TFT_display.begin();
  TFT_display.setRotation(1); // landscape, matches the reference sketch

  showStatus(STATUS_FINGER, "Boot", "Starting");
  delay(500);

  loadConfig();
  runConfigPortal(false); // try saved Wi-Fi; opens "Device-Setup" portal only if that fails

  if (!initFingerprintSensor()) {
    setAuraError();
    showStatus(STATUS_ERROR, "Sensor", "Not Ready");
    delay(1000);
  }

  setAuraIdle();
  drawMenu();
}

void loop() {
  static int32_t lastReportedDetent = 0;

  int32_t detents = readEncoderDetents();
  if (detents != lastReportedDetent) {
    int32_t diff = detents - lastReportedDetent;
    lastReportedDetent = detents;

    // Clockwise (diff > 0) advances to the next item, counter-clockwise
    // (diff < 0) to the previous one; wraps around either end of the menu.
    int32_t newIndex = ((int32_t)menuIndex + diff) % (int32_t)menuCount;
    if (newIndex < 0) newIndex += menuCount;
    menuIndex = (uint8_t)newIndex;
    drawMenu();
  }

  if (encoderButtonPressed) {
    noInterrupts();
    encoderButtonPressed = false;
    interrupts();

    setAuraProcessing();
    switch (menuIndex) {
      case 0: doLogin(); break;
      case 1: doRemoteEnroll(); break;
      case 2: runConfigPortal(true); break;
      case 3: checkForOTAUpdate(); break;
      default: break;
    }
    setAuraIdle();
    drawMenu();
  }

  if (millis() - lastPollMs > POLL_INTERVAL_MS) {
    pollForCommands();
    lastPollMs = millis();
    drawMenu();
  }

  delay(15);
}
