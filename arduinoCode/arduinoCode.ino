#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <fpm.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <DIYables_TFT_SPI.h>
#include <time.h>
#include "secrets.h" // GITHUB_PAT -- copy secrets.h.example to secrets.h and fill in a real token
#include "ota_types.h" // OTAFetchStatus / OTAReleaseInfo -- see that header for why these aren't defined inline here

/* ---------------- Factory defaults ----------------
   Used only the very first time the device boots, before anything has been
   saved to flash. After that, these are overridden by whatever was entered
   through the "Device Setup" captive portal (WiFiManager) and persisted to
   NVS. Keep these as the values for a brand-new, unconfigured unit. */
#define DEFAULT_BASE_URL "https://example.com/api"
#define DEFAULT_DEVICE_ID "device-id"
#define DEFAULT_KEY_SECRET "key-secret"
#define DEFAULT_TENANT_SLUG "ASDistributors"

/* WPA2 password for the "Device-Setup" AP (see runConfigPortal()) --
   prevents anyone in radio range from opening the config/reset portal, not
   just anyone standing at the physical device. Must be 8-63 characters
   (WPA2-PSK requirement). */
#define DEVICE_SETUP_AP_PASSWORD "Asperminds"

/* ---------------- Firmware / OTA update config ----------------
   FIRMWARE_VERSION must match the GitHub Release tag (a leading "v" is
   stripped before comparing) for the device to consider itself up to date.
   To ship an update: bump this, push a tag matching it (e.g. "v0.2") to
   OTA_GITHUB_OWNER/OTA_GITHUB_REPO, and CI builds + attaches OTA_ASSET_NAME
   to the release. Devices check only when "Check Update" is selected from
   the menu (see checkForOTAUpdate() / performOTAUpdate()) -- there's no
   periodic background check, and no remote-triggered update path (the old
   backend command queue that used to push an "UPDATE" command was removed
   along with the rest of that subsystem -- see ackCommand()'s comment).

   NOTE: OTA_GITHUB_REPO is private, so every device carries GITHUB_PAT
   (from secrets.h) to read it. Scope that token to read-only "Contents"
   access on ONLY this repo -- it's baked into every unit's firmware, so a
   dumped/decompiled device exposes it. */
#define FIRMWARE_VERSION "0.1"
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
String deviceId;
String keySecret;
String baseUrl; // must include the "/api" suffix -- endpoints are baseUrl + "/createEnrollment/..." etc.
String tenantSlug; // path segment after each endpoint, e.g. baseUrl + "/createEnrollment/" + tenantSlug

Preferences prefs;
bool shouldSaveConfig = false;
// Read by configPortalStartedCallback -- true only while the config portal was
// opened on demand from the "Device Setup" menu item (see runConfigPortal),
// so its "press button: back" hint is only shown when a button-poll loop is
// actually watching for that press.
bool configPortalCancellable = false;

WiFiClientSecure secureClient;

#define HTTP_CONNECT_TIMEOUT_MS 8000
#define HTTP_RESPONSE_TIMEOUT_MS 8000

/* ---------------- Backend endpoints (body-based auth: every request carries
   deviceId/keySecret in its JSON body, not headers) ----------------
   The tenant slug (e.g. "ASDistributors") used to be a compile-time
   constant here; it's now the runtime `tenantSlug` field instead (see
   "Runtime config" below), set via the "Configure Backend" portal page
   alongside Backend Base URL -- so each call site below builds its full
   URL as baseUrl + EP_* + tenantSlug rather than a single baked-in string. */
#define EP_CREATE_ENROLLMENT            "/createEnrollment/" // used for both device registration and employee-slot enrollment -- see enrollEmployee()'s comment
#define EP_EMPLOYEES_WITHOUT_ENROLLMENT "/getEmployeesWithoutEnrollment/"
#define EP_SYNC_PUNCH_ENTRIES           "/SyncDevicePunchEntries/"

/* Punch buffer: local fingerprint matches accumulate here instead of hitting
   the backend immediately (see appendPunch()/syncPunchBuffer()), POSTed in
   bulk periodically or immediately once full. Persisted to NVS (not just
   RAM) because performOTAUpdate() calls ESP.restart() on every firmware
   update, which would otherwise silently wipe any unsynced punches. */
#define PUNCH_BUFFER_CAPACITY 30
struct PunchRecord {
  uint16_t slotId;
  uint32_t epochTime; // true UTC seconds since epoch -- see getUtcEpochNow()
};
PunchRecord punchBuffer[PUNCH_BUFFER_CAPACITY];
uint8_t punchBufferCount = 0;

#define PUNCH_SYNC_INTERVAL_MS 20000
unsigned long lastPunchSyncMs = 0;

bool deviceRegistered = false; // mirrors NVS "registered" -- gates enroll-fetch and punch-sync until createEnrollment(device) succeeds

/* Employees pending fingerprint enrollment. Fetched fresh whenever the
   "Enrolls" menu item is opened (see doEnroll()), and also refreshed
   periodically in the background (see loop()) purely so the "Enrolls (N)"
   badge on the main menu (drawMenuItem()) stays reasonably current without
   requiring the menu to be opened -- refreshing on every menu redraw would
   mean a blocking network call on every encoder tick, exactly the kind of
   weak-Wi-Fi stall this whole feature exists to avoid. */
#define MAX_EMPLOYEE_LIST 40
long employeeListIds[MAX_EMPLOYEE_LIST];
String employeeListNames[MAX_EMPLOYEE_LIST];
uint8_t employeeListCount = 0;

#define ENROLL_LIST_REFRESH_INTERVAL_MS 60000 // less time-sensitive than punch sync -- just a UI count
unsigned long lastEnrollListRefreshMs = 0;

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

/* R503 WAKEUP/TOUCH pin -- the sensor drives this high while a finger rests
   on its capacitive ring, independent of (and faster than) polling over
   UART. Wired here so the clock screen can jump straight into the login
   flow on touch, without the user opening the menu first.
   NOTE: assumed active-HIGH per the R503 family's documented WAKEUP
   behavior -- verify against your module's silkscreen/datasheet. If it's
   inverted on your unit, swap RISING->FALLING and INPUT_PULLDOWN->
   INPUT_PULLUP where this pin is set up below. */
#define SENSOR_TOUCH_PIN 33

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

const char *menuItems[] = {"Log In", "Enrolls", "Device Setup", "Check Update", "Restart"};
const uint8_t menuCount = sizeof(menuItems) / sizeof(menuItems[0]);
uint8_t menuIndex = 0;

/* ---------------- App state ----------------
   Boots straight into the clock; the menu only appears once the encoder
   button is long-pressed (see LONG_PRESS_MS below), and either a second
   long press or MENU_IDLE_TIMEOUT_MS of inactivity returns to the clock. */
enum AppState { STATE_CLOCK, STATE_MENU };
AppState appState = STATE_CLOCK;
#define MENU_IDLE_TIMEOUT_MS 10000 // idle time in the menu before falling back to the clock

/* ---------------- NTP / IST clock ----------------
   The ESP32 has no battery-backed RTC: time is only known after a
   successful NTP sync while Wi-Fi is up, then free-runs off the internal
   RTC (fine for a UI clock across brief Wi-Fi drops, resets to "unsynced"
   on power loss). Synced opportunistically whenever Wi-Fi comes up (see
   runConfigPortal()/quickReconnectWiFi()) and retried from loop() while
   the clock screen is showing and still unsynced. */
#define IST_OFFSET_SEC (5 * 3600 + 30 * 60) // UTC+5:30, no DST
#define NTP_SERVER_1 "pool.ntp.org"
#define NTP_SERVER_2 "time.google.com"
bool timeSynced = false;

// Set from handleFingerTouchISR() (see the fingerprint touch interrupt
// section below); declared here, ahead of enterClockState(), so it's
// visible where it's cleared on every (re)entry to the clock screen.
volatile bool fingerTouchFlag = false;

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

#define MENU_BANNER_H 44
#define MENU_ITEM_H   44
#define MENU_START_Y  (MENU_BANNER_H + 46)

/* Header + Wi-Fi status line only -- drawn once when the menu is entered. */
void drawMenuHeader() {
  TFT_display.fillScreen(TFT_WHITE);

  TFT_display.fillRect(0, 0, TFT_display.width(), MENU_BANNER_H, TFT_VIOLET);
  TFT_display.setTextColor(TFT_WHITE);
  TFT_display.setTextSize(2);
  TFT_display.setCursor(10, 12);
  TFT_display.print("Attendance");

  bool wifiOn = (WiFi.status() == WL_CONNECTED);
  TFT_display.setTextSize(2);
  TFT_display.setTextColor(TFT_BLACK);
  TFT_display.setCursor(10, MENU_BANNER_H + 12);
  TFT_display.print("WiFi: ");
  TFT_display.setTextColor(wifiOn ? TFT_GREEN : TFT_RED);
  TFT_display.print(wifiOn ? "ON" : "OFF");
}

/* Redraws just one item row (selected or not). Used both for the initial
   full menu draw and for cheap per-row updates while scrolling. */
void drawMenuItem(uint8_t i) {
  int y = MENU_START_Y + i * MENU_ITEM_H;
  bool selected = (i == menuIndex);

  TFT_display.fillRect(0, y, TFT_display.width(), MENU_ITEM_H - 6, selected ? TFT_CYAN : TFT_WHITE);
  TFT_display.setTextColor(selected ? TFT_BLACK : TFT_GRAY);
  TFT_display.setTextSize(2);
  TFT_display.setCursor(16, y + 10);
  TFT_display.print(menuItems[i]);
  if (i == 1) {
    TFT_display.print(" (");
    TFT_display.print(employeeListCount);
    TFT_display.print(")");
  }
}

void drawMenu() {
  drawMenuHeader();
  for (uint8_t i = 0; i < menuCount; i++) drawMenuItem(i);
}

/* Repaints only the previously- and newly-selected rows instead of the
   whole screen -- a full drawMenu() redraw (fillScreen + banner + all rows)
   over SPI is slow enough to feel laggy when scrolling quickly through the
   menu; touching just the two changed rows makes rotation feel instant. */
void updateMenuSelection(uint8_t oldIndex, uint8_t newIndex) {
  if (oldIndex == newIndex) return;
  drawMenuItem(oldIndex);
  drawMenuItem(newIndex);
}

/* ---------------- Clock screen ---------------- */
#define CLOCK_TIME_Y 130
#define CLOCK_TIME_H 44
#define CLOCK_DATE_Y 182
#define CLOCK_DATE_H 26

/* Static chrome (banner, Wi-Fi line, hint) -- drawn once on entering the
   clock screen and again only when Wi-Fi status is worth refreshing. */
void drawClockStatic() {
  TFT_display.fillScreen(TFT_WHITE);

  TFT_display.fillRect(0, 0, TFT_display.width(), MENU_BANNER_H, TFT_VIOLET);
  TFT_display.setTextColor(TFT_WHITE);
  TFT_display.setTextSize(2);
  TFT_display.setCursor(10, 12);
  TFT_display.print("Attendance");

  bool wifiOn = (WiFi.status() == WL_CONNECTED);
  TFT_display.setTextSize(2);
  TFT_display.setTextColor(TFT_BLACK);
  TFT_display.setCursor(10, MENU_BANNER_H + 12);
  TFT_display.print("WiFi: ");
  TFT_display.setTextColor(wifiOn ? TFT_GREEN : TFT_RED);
  TFT_display.print(wifiOn ? "ON" : "OFF");

  // Firmware version, right-aligned on the same line as the Wi-Fi status.
  char versionLine[8];
  snprintf(versionLine, sizeof(versionLine), "V%s", FIRMWARE_VERSION);
  int versionW = (int)strlen(versionLine) * 6 * 2; // font cell is ~6px wide at textSize 1
  TFT_display.setTextColor(TFT_GRAY);
  TFT_display.setCursor(TFT_display.width() - 10 - versionW, MENU_BANNER_H + 12);
  TFT_display.print(versionLine);

  TFT_display.setTextColor(TFT_GRAY);
  TFT_display.setTextSize(2);
  TFT_display.setCursor(10, TFT_display.height() - 30);
  TFT_display.print("Hold button: Menu");
}

/* Redraws just the time/date area -- called once a second, so it must not
   touch the rest of the screen (fillScreen there would flicker every tick). */
void drawClockTime() {
  char timeBuf[12];
  if (!timeSynced) {
    snprintf(timeBuf, sizeof(timeBuf), "Syncing..");
  } else {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
      snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
      snprintf(timeBuf, sizeof(timeBuf), "--:--:--");
    }
  }

  TFT_display.fillRect(0, CLOCK_TIME_Y, TFT_display.width(), CLOCK_TIME_H, TFT_WHITE);
  TFT_display.setTextColor(TFT_BLACK);
  TFT_display.setTextSize(4);
  int textW = (int)strlen(timeBuf) * 6 * 4; // font cell is ~6px wide at textSize 1
  int textX = (TFT_display.width() - textW) / 2;
  if (textX < 0) textX = 4;
  TFT_display.setCursor(textX, CLOCK_TIME_Y + 4);
  TFT_display.print(timeBuf);

  TFT_display.fillRect(0, CLOCK_DATE_Y, TFT_display.width(), CLOCK_DATE_H, TFT_WHITE);
  if (timeSynced) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 0)) {
      static const char *weekday[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
      char dateBuf[24];
      snprintf(dateBuf, sizeof(dateBuf), "%s %02d-%02d-%04d IST",
               weekday[timeinfo.tm_wday], timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);
      int dateW = (int)strlen(dateBuf) * 6 * 2;
      int dateX = (TFT_display.width() - dateW) / 2;
      if (dateX < 0) dateX = 4;
      TFT_display.setTextColor(TFT_GRAY);
      TFT_display.setTextSize(2);
      TFT_display.setCursor(dateX, CLOCK_DATE_Y);
      TFT_display.print(dateBuf);
    }
  }
}

void enterClockState() {
  appState = STATE_CLOCK;
  // Discard any touch that landed while the menu (or an action) was
  // showing -- only a touch that happens once the clock is back up should
  // trigger a login.
  noInterrupts();
  fingerTouchFlag = false;
  interrupts();
  drawClockStatic();
  drawClockTime();
}

/* Shared by both quick ways into the login flow from the clock screen: a
   sensed finger touch (see fingerTouchFlag) and a double-tap of the
   encoder button (see loop()). */
void triggerLoginFromClock() {
  setAuraProcessing();
  doLogin();
  setAuraIdle();
  enterClockState(); // also discards any touch re-trigger generated by the scan itself
}

/* Kicks off (or retries) NTP sync. Safe to call opportunistically -- it's a
   no-op without Wi-Fi, and configTime()/getLocalTime() are cheap once
   already synced. */
void syncTimeIfNeeded() {
  if (WiFi.status() != WL_CONNECTED) return;
  configTime(IST_OFFSET_SEC, 0, NTP_SERVER_1, NTP_SERVER_2);
  struct tm timeinfo;
  timeSynced = getLocalTime(&timeinfo, 5000);
}

/* ---------------- Rotary encoder handling ----------------
   Back to the original dual-pin 4x quadrature decode (CLK+DT both on
   CHANGE, resolved via QUAD_TABLE) -- a single-edge CLK-only decode was
   tried here but proved less reliable on this hardware, so this reverts to
   the verified-working approach. Invalid/bounce transitions decode to 0 in
   the table, which is this scheme's built-in debounce. */
static const int8_t QUAD_TABLE[16] = {
   0, -1, +1,  0,
  +1,  0,  0, -1,
  -1,  0,  0, +1,
   0, +1, -1,  0
};

#define BUTTON_DEBOUNCE_MS   30  // reject SW contact bounce
#define LONG_PRESS_MS        600 // hold time that opens/closes the menu
#define DOUBLE_TAP_WINDOW_MS 400 // max gap between two short taps to count as a double-tap

// Variables touched inside an ISR must be volatile.
volatile int32_t encoderCount = 0;  // raw quadrature ticks
volatile uint8_t encoderState = 0;  // last 2-bit AB state

volatile bool encoderButtonDown = false; // debounced logical button state
volatile bool buttonDownFlag = false;    // set once per validated press edge
volatile bool buttonUpFlag = false;      // set once per validated release edge
volatile uint32_t lastButtonEdgeMs = 0;

void IRAM_ATTR handleEncoderISR() {
  uint8_t a = digitalRead(ENCODER_CLK_PIN);
  uint8_t b = digitalRead(ENCODER_DT_PIN);
  uint8_t currentState = (a << 1) | b;
  uint8_t index = (encoderState << 2) | currentState;

  encoderCount = encoderCount + QUAD_TABLE[index]; // '+=' on volatile is deprecated (C++20)
  encoderState = currentState;
}

void IRAM_ATTR handleEncoderButtonISR() {
  uint32_t now = millis();
  if (now - lastButtonEdgeMs < BUTTON_DEBOUNCE_MS) return;
  lastButtonEdgeMs = now;

  bool pressedNow = (digitalRead(ENCODER_SW_PIN) == LOW); // active low, INPUT_PULLUP
  if (pressedNow && !encoderButtonDown) {
    encoderButtonDown = true;
    buttonDownFlag = true;
  } else if (!pressedNow && encoderButtonDown) {
    encoderButtonDown = false;
    buttonUpFlag = true;
  }
}

/* ---------------- Fingerprint sensor touch interrupt ---------------- */
#define TOUCH_DEBOUNCE_MS 300 // ignore re-triggers while a finger is still settling on the ring

volatile uint32_t lastTouchEdgeMs = 0;

void IRAM_ATTR handleFingerTouchISR() {
  uint32_t now = millis();
  if (now - lastTouchEdgeMs < TOUCH_DEBOUNCE_MS) return;
  lastTouchEdgeMs = now;
  fingerTouchFlag = true;
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
/* Reads saved config from NVS, falling back to factory defaults the first
   time the device ever boots (before anything has been saved). Also
   restores the punch buffer and registration flag here, so both survive a
   reboot (e.g. an OTA update's ESP.restart()). */
void loadConfig() {
  prefs.begin("fpmcfg", true);
  deviceId = prefs.getString("deviceid", DEFAULT_DEVICE_ID);
  keySecret = prefs.getString("keysecret", DEFAULT_KEY_SECRET);
  baseUrl = prefs.getString("baseurl", DEFAULT_BASE_URL);
  tenantSlug = prefs.getString("tenant", DEFAULT_TENANT_SLUG);
  deviceRegistered = prefs.getBool("registered", false);
  punchBufferCount = prefs.getUChar("punchcnt", 0);
  prefs.getBytes("punchbuf", punchBuffer, sizeof(punchBuffer));
  prefs.end();
}

void saveConfig() {
  prefs.begin("fpmcfg", false);
  prefs.putString("deviceid", deviceId);
  prefs.putString("keysecret", keySecret);
  prefs.putString("baseurl", baseUrl);
  prefs.putString("tenant", tenantSlug);
  prefs.end();
  Serial.println("[CONFIG] Saved device config to NVS");
}

/* Persists the punch buffer as a single fixed-size blob (always the full
   PUNCH_BUFFER_CAPACITY worth, ~180 bytes) -- simpler and no worse on NVS's
   wear budget than a variably-sized write, and cheaper than one NVS entry
   per record. Called on every append and every successful sync. */
void savePunchBuffer() {
  prefs.begin("fpmcfg", false);
  prefs.putUChar("punchcnt", punchBufferCount);
  prefs.putBytes("punchbuf", punchBuffer, sizeof(punchBuffer));
  prefs.end();
}

void saveRegisteredFlag() {
  prefs.begin("fpmcfg", false);
  prefs.putBool("registered", deviceRegistered);
  prefs.end();
}

void saveConfigCallback() {
  shouldSaveConfig = true;
}

/* Dedicated setup-mode screen rather than the generic showStatus() (which
   only fits one short message) -- the AP name has to actually be read and
   joined by the user, so it gets a line of its own, plus an explicit hint
   for getting back out without waiting through the full portal timeout.
   `cancellable` is only true when the portal was opened on demand from the
   menu (see runConfigPortal): at boot there's no menu to return to and no
   button-poll loop watching for a press, so the hint would be a lie. */
void drawSetupModeScreen(bool cancellable) {
  int bannerH = 56;
  TFT_display.fillScreen(TFT_WHITE);

  TFT_display.fillRect(0, 0, TFT_display.width(), bannerH, TFT_BLUE);
  drawStatusIcon(STATUS_WIFI, 28, bannerH / 2, 16, TFT_WHITE);
  TFT_display.setTextColor(TFT_WHITE);
  TFT_display.setTextSize(2);
  TFT_display.setCursor(56, bannerH / 2 - 8);
  TFT_display.print("Setup Mode");

  TFT_display.setTextColor(TFT_BLACK);
  TFT_display.setTextSize(2);
  TFT_display.setCursor(20, bannerH + 26);
  TFT_display.print("Join WiFi to setup:");

  TFT_display.setTextColor(TFT_BLUE);
  TFT_display.setTextSize(3);
  TFT_display.setCursor(20, bannerH + 60);
  TFT_display.print("Device-Setup");

  TFT_display.setTextColor(TFT_BLACK);
  TFT_display.setTextSize(2);
  TFT_display.setCursor(20, bannerH + 96);
  TFT_display.print("Password:");
  TFT_display.setTextColor(TFT_BLUE);
  TFT_display.setCursor(20, bannerH + 120);
  TFT_display.print(DEVICE_SETUP_AP_PASSWORD);

  TFT_display.setTextColor(TFT_GRAY);
  TFT_display.setTextSize(2);
  TFT_display.setCursor(20, TFT_display.height() - 30);
  TFT_display.print(cancellable ? "Press button: Back" : "Waiting...");
}

void configPortalStartedCallback(WiFiManager *wmPtr) {
  setAuraWiFi();
  drawSetupModeScreen(configPortalCancellable);
}

/* Single portal, two menu entries: connecting to the "Device-Setup" access
   point opens one WiFiManager captive portal whose root page offers
   "Configure WiFi" (native SSID/password page) and "Configure Backend" (our
   Device ID / Device Key Secret / Backend Base URL / Backend Tenant Slug
   fields, on their own page via setParamsPage so they never show up on the
   Wi-Fi page). Either,
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

  WiFiManagerParameter custom_deviceid("deviceid", "Device ID", deviceId.c_str(), 40);
  WiFiManagerParameter custom_keysecret("keysecret", "Device Key Secret", keySecret.c_str(), 64);
  WiFiManagerParameter custom_baseurl("baseurl", "Backend Base URL (include /api)", baseUrl.c_str(), 80);
  WiFiManagerParameter custom_tenant("tenant", "Backend Tenant Slug", tenantSlug.c_str(), 40);
  wm.addParameter(&custom_deviceid);
  wm.addParameter(&custom_keysecret);
  wm.addParameter(&custom_baseurl);
  wm.addParameter(&custom_tenant);

  wm.setParamsPage(true); // keep the backend fields off the Wi-Fi page, on their own page instead
  wm.setCustomMenuHTML(
    "<form action='/param' method='get'><button>Configure Backend</button></form><br/>\n"
    "<form action='/resetdevice' method='get'><button>Reset Device</button></form><br/>\n"
  );
  std::vector<const char *> menu = {"wifi", "custom", "info", "exit"};
  wm.setMenu(menu); // adds our "Configure Backend"/"Reset Device" buttons next to the native "Configure WiFi" one

  // Registers a custom /resetdevice page on WiFiManager's own web server --
  // fires once the server exists but before WiFiManager's own routes are
  // attached, per its documented setWebServerCallback() contract, so this
  // only ADDS a route rather than needing to override anything. Deliberately
  // NOT reachable from the on-device menu -- see resetAllFingerprints().
  // GET shows a warning + confirm button; only the POST actually wipes the
  // sensor, so a stray click/prefetch of the link itself can't trigger it.
  wm.setWebServerCallback([&wm]() {
    wm.server->on("/resetdevice", HTTP_GET, [&wm]() {
      wm.server->send(200, "text/html",
        "<html><body>"
        "<h2 style='color:#c00'>Reset Device</h2>"
        "<p>This deletes <b>ALL</b> fingerprints stored on this device. "
        "Every enrolled employee will need to be re-enrolled. This cannot be undone.</p>"
        "<form action='/resetdevice' method='POST'>"
        "<button type='submit' style='background:#c00;color:#fff;padding:10px'>Yes, delete everything</button>"
        "</form>"
        "<p><a href='/'>Cancel</a></p>"
        "</body></html>");
    });
    wm.server->on("/resetdevice", HTTP_POST, [&wm]() {
      bool ok = resetAllFingerprints();
      wm.server->send(200, "text/html",
        ok
          ? "<html><body><h2>Reset complete</h2><p>All fingerprints deleted. The device is restarting.</p></body></html>"
          : "<html><body><h2 style='color:#c00'>Reset failed</h2><p>Could not reach the fingerprint sensor. Check wiring and try again.</p><p><a href='/'>Back</a></p></body></html>");
      // Restart on success so the device comes back up in a clean state
      // rather than sitting in this AP/portal session indefinitely -- send()
      // above queues the response, but the socket needs a moment to actually
      // flush before the AP drops, same pattern as the OTA-update ack sent
      // just before ESP.restart() elsewhere in this file. Only restart on
      // success -- a failure should let the operator retry (e.g. reseat the
      // sensor wiring) without an unexpected reboot cutting off the session.
      if (ok) {
        delay(1000);
        ESP.restart();
      }
    });
  });

  shouldSaveConfig = false;
  bool connected;
  bool cancelled = false;
  configPortalCancellable = forcePortal; // read by configPortalStartedCallback

  if (forcePortal) {
    showStatus(STATUS_WIFI, "Setup Mode", "Starting AP");
    delay(400);

    // Non-blocking: startConfigPortal() would otherwise block right here
    // until connected/saved/timed out, with no way for the menu button to
    // break out early. Pumping process() ourselves (same button-poll style
    // as promptOTAConfirmation()) lets a short press cancel straight back
    // to the menu instead of waiting out the full 180s timeout.
    wm.setConfigPortalBlocking(false);
    wm.startConfigPortal("Device-Setup", DEVICE_SETUP_AP_PASSWORD);
    while (wm.getConfigPortalActive()) {
      wm.process();

      noInterrupts();
      buttonDownFlag = false; // consumed here so it isn't replayed as a fresh menu press once we return
      bool upFlag = buttonUpFlag;
      buttonUpFlag = false;
      interrupts();

      if (upFlag) {
        cancelled = true;
        wm.stopConfigPortal();
        break;
      }
    }
    connected = (WiFi.status() == WL_CONNECTED);
  } else {
    connected = wm.autoConnect("Device-Setup", DEVICE_SETUP_AP_PASSWORD);
  }
  configPortalCancellable = false;

  // Backend fields are only in play if the "Configure Backend" page was used --
  // check that regardless of whether Wi-Fi itself was touched this session.
  if (shouldSaveConfig) {
    deviceId = String(custom_deviceid.getValue());
    keySecret = String(custom_keysecret.getValue());
    baseUrl = String(custom_baseurl.getValue());
    tenantSlug = String(custom_tenant.getValue());
    saveConfig();
  }

  if (connected) {
    setAuraSuccess();
    showStatus(STATUS_OK, "Wi-Fi", "Connected");
    syncTimeIfNeeded();
    delay(700);
  } else if (shouldSaveConfig) {
    setAuraSuccess();
    showStatus(STATUS_OK, "Backend", "Saved");
    delay(700);
  } else if (cancelled) {
    setAuraIdle();
    showStatus(STATUS_ERROR, "Setup", "Cancelled");
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

/* Queries the latest GitHub release. Split out of performOTAUpdate so the
   fetch/parse logic can be shared between the remote-triggered path (applies
   unconditionally) and the manual "Check Update" path (shows the version and
   asks for confirmation first). Return type defined in ota_types.h. */
OTAReleaseInfo fetchLatestRelease() {
  OTAReleaseInfo info;
  info.status = OTA_FETCH_HTTP_ERROR;

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
    // 404 here almost always means either GITHUB_PAT is a placeholder/invalid/expired
    // (GitHub hides private-repo existence behind 404 rather than 401/403), or the repo
    // has no published (non-draft, non-prerelease) release yet.
    if (code > 0) Serial.println("[OTA] Response body: " + http.getString());
    http.end();
    return info;
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
    info.status = OTA_FETCH_BAD_JSON;
    return info;
  }

  info.version = stripVersionPrefix(String((const char *)(doc["tag_name"] | "")));
  if (info.version.length() == 0) {
    info.status = OTA_FETCH_NO_RELEASE;
    return info;
  }

  for (JsonObject asset : doc["assets"].as<JsonArray>()) {
    if (String((const char *)(asset["name"] | "")) == OTA_ASSET_NAME) {
      info.assetUrl = String((const char *)(asset["url"] | ""));
      break;
    }
  }

  info.status = OTA_FETCH_OK;
  return info;
}

/* Yes/No confirmation screen shown by a manual "Check Update" when a newer
   release is found. Reuses the rotary encoder purely as a two-way toggle
   (rotate = swap the highlighted option, press = confirm whichever is
   highlighted) -- kept separate from the main menu's state machine since
   this blocks synchronously inside performOTAUpdate(), same as the
   fingerprint prompts do. Always waits for the button to be released before
   returning (rather than reacting on press, or distinguishing short/long
   press) so no stale press-in-progress state leaks back into loop()'s own
   long-press tracking once control returns to it. Defaults to "No" and
   times out to "No" after OTA_CONFIRM_TIMEOUT_MS so an unattended device
   never gets stuck here. */
#define OTA_CONFIRM_TIMEOUT_MS 20000

void drawOTAConfirm(const String &newVersion, bool yesSelected) {
  TFT_display.fillScreen(TFT_WHITE);

  TFT_display.fillRect(0, 0, TFT_display.width(), MENU_BANNER_H, TFT_VIOLET);
  TFT_display.setTextColor(TFT_WHITE);
  TFT_display.setTextSize(2);
  TFT_display.setCursor(10, 12);
  TFT_display.print("Update Available");

  TFT_display.setTextColor(TFT_BLACK);
  TFT_display.setTextSize(2);
  TFT_display.setCursor(10, MENU_BANNER_H + 16);
  TFT_display.print("Current: v");
  TFT_display.print(FIRMWARE_VERSION);
  TFT_display.setCursor(10, MENU_BANNER_H + 44);
  TFT_display.print("New:     v");
  TFT_display.print(newVersion);

  TFT_display.setTextColor(TFT_GRAY);
  TFT_display.setCursor(10, MENU_BANNER_H + 80);
  TFT_display.print("Install this update?");

  int y = MENU_BANNER_H + 118;
  TFT_display.fillRect(10, y, 100, MENU_ITEM_H - 6, yesSelected ? TFT_CYAN : TFT_WHITE);
  TFT_display.setTextColor(yesSelected ? TFT_BLACK : TFT_GRAY);
  TFT_display.setCursor(32, y + 10);
  TFT_display.print("Yes");

  TFT_display.fillRect(130, y, 100, MENU_ITEM_H - 6, !yesSelected ? TFT_CYAN : TFT_WHITE);
  TFT_display.setTextColor(!yesSelected ? TFT_BLACK : TFT_GRAY);
  TFT_display.setCursor(152, y + 10);
  TFT_display.print("No");

  TFT_display.setTextColor(TFT_GRAY);
  TFT_display.setTextSize(2);
  TFT_display.setCursor(10, TFT_display.height() - 56);
  TFT_display.print("Turn: Choose");
  TFT_display.setCursor(10, TFT_display.height() - 30);
  TFT_display.print("Press: Confirm");
}

bool promptOTAConfirmation(const String &newVersion) {
  bool yesSelected = false; // default to "No" -- an OTA flash isn't something to fall into by accident
  int32_t lastDetent = readEncoderDetents();
  drawOTAConfirm(newVersion, yesSelected);

  uint32_t start = millis();
  while (millis() - start < OTA_CONFIRM_TIMEOUT_MS) {
    int32_t detents = readEncoderDetents();
    if (detents != lastDetent) {
      lastDetent = detents;
      yesSelected = !yesSelected; // only two options -- any rotation toggles between them
      drawOTAConfirm(newVersion, yesSelected);
    }

    noInterrupts();
    buttonDownFlag = false; // consumed here so it can't be mistaken for a fresh press once we return
    bool upFlag = buttonUpFlag;
    buttonUpFlag = false;
    interrupts();

    if (upFlag) return yesSelected;

    delay(5);
  }

  return false; // timed out -> treat as "No"
}

/* Core of the "Check Update" flow: queries the latest release on
   OTA_GITHUB_REPO, compares its tag to FIRMWARE_VERSION, and if different,
   downloads + flashes OTA_ASSET_NAME via otaDownloadAndFlash and reboots
   into it.

   commandId: always -1 now -- the only remaining caller is the local/manual
   "Check Update" menu item, which shows the new version on screen via
   promptOTAConfirmation() and only proceeds if the user confirms. This used
   to also support commandId >= 0 for a remote-triggered update pushed
   through the old backend's command queue (applied unconditionally, with a
   DONE/FAILED ack sent back); that trigger path was removed along with the
   rest of the command-queue subsystem (see ackCommand()'s comment), but the
   commandId >= 0 branches below are left in place since they're harmless
   dead code and this function is otherwise unrelated to that change.
   The ack for a successful update is sent just before ESP.restart() --
   ackCommand() blocks until the POST completes, so the backend hears back
   before the reboot happens. */
void performOTAUpdate(long commandId) {
  showStatus(STATUS_WIFI, "OTA", "Checking...");

  if (WiFi.status() != WL_CONNECTED) {
    setAuraError();
    showStatus(STATUS_ERROR, "OTA", "No WiFi");
    beepError();
    delay(1200);
    setAuraIdle();
    if (commandId >= 0) ackCommand(commandId, false, -1);
    return;
  }

  OTAReleaseInfo release = fetchLatestRelease();
  if (release.status != OTA_FETCH_OK) {
    const char *failLabel = (release.status == OTA_FETCH_BAD_JSON) ? "Bad Reply"
                           : (release.status == OTA_FETCH_NO_RELEASE) ? "No Release"
                           : "Check Failed";
    setAuraError();
    showStatus(STATUS_ERROR, "OTA", failLabel);
    beepError();
    delay(1200);
    setAuraIdle();
    if (commandId >= 0) ackCommand(commandId, false, -1);
    return;
  }

  if (release.version == FIRMWARE_VERSION) {
    setAuraSuccess();
    showStatus(STATUS_OK, "OTA", "Up To Date");
    delay(1200);
    setAuraIdle();
    if (commandId >= 0) ackCommand(commandId, true, -1);
    return;
  }

  if (release.assetUrl.length() == 0) {
    setAuraError();
    showStatus(STATUS_ERROR, "OTA", "No Asset");
    beepError();
    delay(1200);
    setAuraIdle();
    if (commandId >= 0) ackCommand(commandId, false, -1);
    return;
  }

  // An update is available. Remote-triggered (commandId >= 0) commands were
  // already decided by the backend/admin, so apply unconditionally; a local
  // "Check Update" menu press (commandId == -1) shows the new version and
  // waits here for the user to confirm before touching the flash.
  if (commandId < 0 && !promptOTAConfirmation(release.version)) {
    showStatus(STATUS_WIFI, "OTA", "Skipped");
    delay(900);
    setAuraIdle();
    return;
  }

  showStatus(STATUS_WIFI, "OTA", "Downloading");
  bool ok = otaDownloadAndFlash(release.assetUrl);

  if (ok) {
    setAuraSuccess();
    showStatus(STATUS_OK, "OTA", "Rebooting");
    beepSuccess();
    delay(1200);
    if (commandId >= 0) ackCommand(commandId, true, -1);
    ESP.restart();
  } else {
    setAuraError();
    showStatus(STATUS_ERROR, "OTA", "Update Failed");
    beepError();
    delay(1500);
    setAuraIdle();
    if (commandId >= 0) ackCommand(commandId, false, -1);
  }
}

/* "Check Update" menu item: local/manual trigger, no backend ack. */
void checkForOTAUpdate() {
  performOTAUpdate(-1);
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
    syncTimeIfNeeded();
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

/* ---------------- Backend sync: registration, enrollment, punch buffer ----------------
   Auth on every call below is body-based: {"deviceId":...,"keySecret":...}
   merged into each request's JSON, no custom headers. */

/* True UTC seconds since epoch. NOTE: on ESP32 Arduino, configTime()'s
   gmtOffset_sec bakes the offset directly into the system clock itself, so
   time(nullptr) returns the already-IST-shifted value (unlike standard
   POSIX, where time() is timezone-agnostic and only localtime() applies an
   offset) -- confirmed against known ESP32 Arduino core behavior. Subtract
   the offset back out here, once, to recover true UTC for the backend.
   syncTimeIfNeeded()/configTime() and the on-screen IST clock are untouched. */
uint32_t getUtcEpochNow() {
  return (uint32_t)(time(nullptr) - IST_OFFSET_SEC);
}

/* ISO8601 UTC, e.g. "2026-08-27T17:06:24.000Z". Milliseconds are a fixed
   placeholder, not real sub-second capture -- a fingerprint scan takes well
   over a second, so same-second collisions aren't a realistic concern, and
   real ms precision would cost an extra buffer byte for no practical gain. */
String formatIso8601Utc(uint32_t utcEpoch) {
  time_t t = (time_t)utcEpoch;
  struct tm tmUtc;
  gmtime_r(&t, &tmUtc);
  char buf[25];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.000Z",
           tmUtc.tm_year + 1900, tmUtc.tm_mon + 1, tmUtc.tm_mday,
           tmUtc.tm_hour, tmUtc.tm_min, tmUtc.tm_sec);
  return String(buf);
}

/* createEnrollment and SyncDevicePunchEntries both respond HTTP 200 even on
   failure (confirmed live: a duplicate device registration came back 200
   with {"notifier":"error","message":"Enrollment Already Exists"}) -- the
   real result is only in the body's "notifier" field. Every caller that
   cares about success/failure must check this, not just the HTTP status. */
bool bodyNotifierIsSuccess(const String &body) {
  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, body)) return false;
  String notifier = String((const char *)(doc["notifier"] | ""));
  return notifier == "success";
}

/* Registers this device with the backend. Attempted once at boot and
   retried from loop() until it succeeds; result persisted so it only ever
   needs to happen once per device lifetime. */
bool registerDeviceIfNeeded() {
  if (WiFi.status() != WL_CONNECTED) return false;

  DynamicJsonDocument doc(256);
  doc["deviceId"] = deviceId;
  doc["keySecret"] = keySecret;
  doc["type"] = "device";
  String payload;
  serializeJson(doc, payload);

  HTTPClient http;
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_RESPONSE_TIMEOUT_MS);
  http.begin(secureClient, baseUrl + EP_CREATE_ENROLLMENT + tenantSlug);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);
  String body = (code == 200) ? http.getString() : String("");
  http.end();

  Serial.print("[REGISTER] status "); Serial.print(code);
  Serial.print(" body: "); Serial.println(body);

  bool ok = (code == 200) && bodyNotifierIsSuccess(body);
  // Treat "already exists" as success too, not just a literal notifier
  // "success" -- otherwise a device that's already known to the backend
  // (re-flashed firmware, NVS erased, pre-provisioned before shipping)
  // would retry forever and never reach the "registered" state that
  // enrollment-fetch/punch-sync are gated on. Tradeoff: a genuinely WRONG
  // keySecret for an existing deviceId looks identical to this from the
  // response alone, so it gets accepted here too -- the backend would need
  // to return a distinct error to tell those apart.
  if (!ok && body.indexOf("Already Exists") >= 0) ok = true;

  if (!ok) return false;

  deviceRegistered = true;
  saveRegisteredFlag();
  // Seed the "Enrolls (N)" badge immediately rather than waiting up to
  // ENROLL_LIST_REFRESH_INTERVAL_MS for the first background refresh.
  fetchEmployeesWithoutEnrollment();
  lastEnrollListRefreshMs = millis();
  return true;
}

/* Flushes the punch buffer in one all-or-nothing bulk POST -- the backend
   gives no per-entry ack, so success clears the whole buffer and any
   failure leaves it fully intact for the next attempt. */
bool syncPunchBuffer() {
  if (punchBufferCount == 0) return true;
  if (!deviceRegistered) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  // Sized generously for a full PUNCH_BUFFER_CAPACITY (30) batch -- confirmed
  // live that 30 entries serialize to ~1.6KB of raw JSON, but ArduinoJson's
  // internal per-object/array overhead runs well above raw text size for
  // many small objects like these (30 objects x 2 fields each), so building
  // toward a tight budget risks silent truncation right at the one moment
  // (a full buffer) this whole feature exists for. RAM is abundant here
  // (this is a transient allocation against ~276KB free), so there's no
  // reason to cut this close.
  DynamicJsonDocument doc(8192);
  doc["deviceId"] = deviceId;
  doc["keySecret"] = keySecret;
  JsonArray entries = doc.createNestedArray("entries");
  for (uint8_t i = 0; i < punchBufferCount; i++) {
    JsonObject e = entries.createNestedObject();
    e["slotId"] = punchBuffer[i].slotId;
    e["timestamp"] = formatIso8601Utc(punchBuffer[i].epochTime);
  }
  String payload;
  serializeJson(doc, payload);

  HTTPClient http;
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_RESPONSE_TIMEOUT_MS);
  http.begin(secureClient, baseUrl + EP_SYNC_PUNCH_ENTRIES + tenantSlug);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);
  String body = (code == 200) ? http.getString() : String("");
  http.end();

  Serial.print("[PUNCH SYNC] status code "); Serial.println(code);

  if (code != 200 || !bodyNotifierIsSuccess(body)) return false;

  punchBufferCount = 0;
  savePunchBuffer();
  return true;
}

/* Buffers a punch locally -- instant, no network wait. If the buffer is
   already full, one blocking sync is attempted right now; if that also
   fails, the OLDEST record is evicted to make room. Evict-oldest (not
   reject-newest) because rejecting the punch happening right now would
   leave the person standing at the device with zero record and no
   confirmation -- the worst outcome for the one person actually present.
   This only triggers after PUNCH_BUFFER_CAPACITY unsynced punches AND a
   live retry just failed, i.e. a sustained outage, not routine use. */
void appendPunch(uint16_t slotId, uint32_t utcEpoch) {
  if (punchBufferCount >= PUNCH_BUFFER_CAPACITY) {
    showStatus(STATUS_WIFI, "Sync", "Buffer Full");
    if (!syncPunchBuffer()) {
      for (uint8_t i = 1; i < PUNCH_BUFFER_CAPACITY; i++) punchBuffer[i - 1] = punchBuffer[i];
      punchBufferCount = PUNCH_BUFFER_CAPACITY - 1;
      Serial.println("[PUNCH] Buffer full and sync failed -- evicted oldest record");
    }
  }
  punchBuffer[punchBufferCount].slotId = slotId;
  punchBuffer[punchBufferCount].epochTime = utcEpoch;
  punchBufferCount++;
  savePunchBuffer();
}

/* Fetches the list of employees still needing fingerprint enrollment. */
bool fetchEmployeesWithoutEnrollment() {
  employeeListCount = 0;
  if (WiFi.status() != WL_CONNECTED) return false;

  DynamicJsonDocument reqDoc(256);
  reqDoc["deviceId"] = deviceId;
  reqDoc["keySecret"] = keySecret;
  String payload;
  serializeJson(reqDoc, payload);

  HTTPClient http;
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_RESPONSE_TIMEOUT_MS);
  http.begin(secureClient, baseUrl + EP_EMPLOYEES_WITHOUT_ENROLLMENT + tenantSlug);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);
  String body = (code == 200) ? http.getString() : String("");
  http.end();

  Serial.print("[ENROLL LIST] status code "); Serial.println(code);
  if (code != 200) return false;

  // Same overhead reasoning as syncPunchBuffer()'s doc -- sized generously
  // for a full MAX_EMPLOYEE_LIST (40) entries, each with a name string of
  // unknown real-world length, rather than cutting a parse budget close.
  DynamicJsonDocument doc(8192);
  if (deserializeJson(doc, body)) return false;

  for (JsonObject emp : doc.as<JsonArray>()) {
    if (employeeListCount >= MAX_EMPLOYEE_LIST) break;
    employeeListIds[employeeListCount] = emp["id"] | -1;
    employeeListNames[employeeListCount] = String((const char *)(emp["name"] | ""));
    employeeListCount++;
  }
  return true;
}

/* Registers a captured fingerprint slot against an employee, via
   createEnrollment(type=employee). This backend originally rejected this
   exact call with "Enrollment Already Exists" (employee records are
   pre-created elsewhere, e.g. an HR admin flow, which is why they show up
   in getEmployeesWithoutEnrollment in the first place) -- an
   updateEmployeeSlotId endpoint was used as a workaround for a while, but
   the backend was fixed server-side and createEnrollment(type=employee) is
   now confirmed live as the correct/current call again. No keySecret
   needed on this one (confirmed live), unlike the device-registration call.

   deviceId is REQUIRED -- confirmed live that the slot<->employee mapping
   is scoped per device, and this was confirmed again on this endpoint: a
   slot enrolled without deviceId would (per the earlier
   updateEmployeeSlotId experience) still look successful but be unusable --
   SyncDevicePunchEntries would fail with "No employees found" for that slot
   since there'd be no device association to resolve it against. */
bool enrollEmployee(long employeeId, uint16_t slotId) {
  if (WiFi.status() != WL_CONNECTED) return false;

  DynamicJsonDocument doc(128);
  doc["deviceId"] = deviceId;
  doc["employeeId"] = employeeId;
  doc["type"] = "employee";
  doc["slotId"] = slotId;
  String payload;
  serializeJson(doc, payload);

  HTTPClient http;
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_RESPONSE_TIMEOUT_MS);
  http.begin(secureClient, baseUrl + EP_CREATE_ENROLLMENT + tenantSlug);
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);
  String body = (code == 200) ? http.getString() : String("");
  http.end();

  Serial.print("[ENROLL EMPLOYEE] status "); Serial.print(code);
  Serial.print(" body: "); Serial.println(body);
  return code == 200 && bodyNotifierIsSuccess(body);
}

/* ---------------- Legacy command-ack (dead code, kept for compilation) ----------------
   The old backend's command-queue subsystem (pollForCommands(), the ENROLL/
   DELETE/LIST_SLOTS dispatch, extractJsonLong()/extractJsonString()) has
   been removed along with that backend. This function's own endpoint
   (baseUrl + "/api/devices/commands/<id>/ack") no longer exists either --
   but performOTAUpdate() (untouched, still calls this in 5 places guarded
   by `if (commandId >= 0)`) is the one caller left, and it only ever passes
   commandId == -1 now (from checkForOTAUpdate() -> performOTAUpdate(-1)),
   so those calls are permanently dead in practice. Deleting this function
   would break that untouched OTA code, so it stays, unused. */
void ackCommand(long commandId, bool success, long resultSlot) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_RESPONSE_TIMEOUT_MS);
  String url = baseUrl + "/api/devices/commands/" + String(commandId) + "/ack";
  http.begin(secureClient, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", deviceId);
  http.addHeader("X-Device-Secret", keySecret);

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

/* ---------------- "Enrolls" menu: pull-list employee enrollment ----------------
   Pull model, replacing the old push-command queue: the device now fetches
   the pending list itself (fetchEmployeesWithoutEnrollment()) rather than
   waiting for the backend to queue an ENROLL command. */
#define ENROLL_LIST_VISIBLE_ROWS 4

void drawEnrollListHeader() {
  TFT_display.fillScreen(TFT_WHITE);
  TFT_display.fillRect(0, 0, TFT_display.width(), MENU_BANNER_H, TFT_VIOLET);
  TFT_display.setTextColor(TFT_WHITE);
  TFT_display.setTextSize(2);
  TFT_display.setCursor(10, 12);
  TFT_display.print("Select Employee");
}

void drawEnrollListItem(uint8_t visibleRow, uint8_t dataIndex, bool selected) {
  int y = MENU_START_Y + visibleRow * MENU_ITEM_H;
  TFT_display.fillRect(0, y, TFT_display.width(), MENU_ITEM_H - 6, selected ? TFT_CYAN : TFT_WHITE);
  TFT_display.setTextColor(selected ? TFT_BLACK : TFT_GRAY);
  TFT_display.setTextSize(2);
  TFT_display.setCursor(16, y + 10);
  TFT_display.print(employeeListNames[dataIndex]);
  TFT_display.print("(");
  TFT_display.print(employeeListIds[dataIndex]);
  TFT_display.print(")");
}

/* scrollOffset shifts which window of ENROLL_LIST_VISIBLE_ROWS is on screen --
   needed because employeeListCount can exceed what fits, unlike the fixed
   5-item main menu. */
void drawEnrollList(uint8_t selectedIndex, uint8_t scrollOffset) {
  drawEnrollListHeader();
  uint8_t visibleCount = employeeListCount - scrollOffset;
  if (visibleCount > ENROLL_LIST_VISIBLE_ROWS) visibleCount = ENROLL_LIST_VISIBLE_ROWS;
  for (uint8_t row = 0; row < visibleCount; row++) {
    uint8_t dataIndex = scrollOffset + row;
    drawEnrollListItem(row, dataIndex, dataIndex == selectedIndex);
  }

  TFT_display.setTextColor(TFT_GRAY);
  TFT_display.setTextSize(2);
  TFT_display.setCursor(10, TFT_display.height() - 30);
  TFT_display.print("Turn: Scroll  Press: Pick");
}

/* Blocking selection loop, modeled on promptOTAConfirmation(): encoder moves
   the highlight (wrapping), a validated button-up confirms and returns the
   selected employeeId. Times out (no separate cancel gesture, same as
   promptOTAConfirmation -- the button here already means "confirm") and
   returns -1 for cancelled. */
long promptEnrollSelection() {
  int32_t selectedIndex = 0;
  uint8_t scrollOffset = 0;
  int32_t lastDetent = readEncoderDetents();
  drawEnrollList((uint8_t)selectedIndex, scrollOffset);

  uint32_t start = millis();
  while (millis() - start < OTA_CONFIRM_TIMEOUT_MS) {
    int32_t detents = readEncoderDetents();
    if (detents != lastDetent) {
      int32_t diff = detents - lastDetent;
      lastDetent = detents;

      int32_t newIndex = (selectedIndex + diff) % (int32_t)employeeListCount;
      if (newIndex < 0) newIndex += employeeListCount;
      selectedIndex = newIndex;

      if (selectedIndex < scrollOffset) {
        scrollOffset = (uint8_t)selectedIndex;
      } else if (selectedIndex >= scrollOffset + ENROLL_LIST_VISIBLE_ROWS) {
        scrollOffset = (uint8_t)(selectedIndex - ENROLL_LIST_VISIBLE_ROWS + 1);
      }

      drawEnrollList((uint8_t)selectedIndex, scrollOffset);
    }

    noInterrupts();
    buttonDownFlag = false; // consumed here so it can't be mistaken for a fresh press once we return
    bool upFlag = buttonUpFlag;
    buttonUpFlag = false;
    interrupts();

    if (upFlag) return employeeListIds[selectedIndex];

    delay(5);
  }

  return -1; // timed out -- treat as cancelled
}

void doEnroll() {
  if (!deviceRegistered) {
    setAuraError();
    showStatus(STATUS_ERROR, "Device", "Not Registered");
    beepError();
    delay(900);
    setAuraIdle();
    return;
  }

  if (WiFi.status() != WL_CONNECTED && !quickReconnectWiFi()) {
    setAuraError();
    showStatus(STATUS_ERROR, "Network", "Offline");
    beepError();
    delay(900);
    setAuraIdle();
    return;
  }

  setAuraProcessing();
  showStatus(STATUS_WIFI, "Enrolls", "Loading...");
  lastEnrollListRefreshMs = millis(); // this counts as a refresh -- no need for the background timer to repeat it right away
  if (!fetchEmployeesWithoutEnrollment()) {
    setAuraError();
    showStatus(STATUS_ERROR, "Enrolls", "Fetch Failed");
    beepError();
    delay(900);
    setAuraIdle();
    return;
  }

  if (employeeListCount == 0) {
    setAuraSuccess();
    showStatus(STATUS_OK, "Enrolls", "All Done");
    beepSuccess();
    delay(900);
    setAuraIdle();
    return;
  }

  long chosenEmployeeId = promptEnrollSelection();
  if (chosenEmployeeId < 0) {
    setAuraIdle(); // cancelled/timed out -- silent return to menu
    return;
  }

  if (!sensorReady && !initFingerprintSensor()) {
    setAuraError();
    showStatus(STATUS_ERROR, "Sensor", "Missing");
    beepError();
    delay(900);
    setAuraIdle();
    return;
  }

  uint16_t newSlot = 0;
  bool captured = captureAndStoreTemplate(&newSlot);
  if (!captured) {
    // No local cleanup needed -- the backend only drops an employee from
    // getEmployeesWithoutEnrollment once createEnrollment succeeds, so a
    // failed attempt naturally reappears next time this list is fetched.
    setAuraIdle();
    return;
  }

  bool posted = enrollEmployee(chosenEmployeeId, newSlot);
  if (posted) setAuraSuccess(); else setAuraError();
  showStatus(posted ? STATUS_OK : STATUS_ERROR, "Enroll", posted ? "Synced" : "Sync Failed");
  if (posted) beepSuccess(); else beepError();
  delay(1200);
  setAuraIdle();
}

/* Wipes every fingerprint template on the sensor via the FPM library's
   emptyDatabase() (maps to the R503's PS_Empty command). Deliberately NOT
   reachable from the on-device menu -- deleting every employee's
   fingerprint is irreversible and affects everyone who uses this device,
   so it needs to require the same access as changing Wi-Fi/backend config
   (the "Device-Setup" AP's password), not be reachable by anyone who just
   picks up the unit and scrolls the menu. Triggered from the WiFiManager
   portal's /resetdevice page instead (see runConfigPortal()), which is
   itself gated behind DEVICE_SETUP_AP_PASSWORD and its own web-side
   confirmation step. Deleting all templates also frees slot 0 again, which
   findFreeTemplateId()/reserveSlotZero() already handle automatically on
   the next enrollment -- no special-casing needed here. */
bool resetAllFingerprints() {
  if (!sensorReady && !initFingerprintSensor()) return false;

  showStatus(STATUS_WIFI, "Reset", "Deleting...");
  FPMStatus st = finger.emptyDatabase();

  if (st == FPMStatus::OK) {
    setAuraSuccess();
    showStatus(STATUS_OK, "Reset", "Complete");
    beepSuccess();
  } else {
    setAuraError();
    showStatus(STATUS_ERROR, "Reset", "Failed");
    beepError();
  }
  delay(1200);
  setAuraIdle();
  return st == FPMStatus::OK;
}

bool doLogin() {
  if (!sensorReady && !initFingerprintSensor()) {
    setAuraError();
    showStatus(STATUS_ERROR, "Sensor", "Missing");
    beepError();
    return false;
  }

  // Punches are timestamped and buffered locally, so a real synced clock is
  // required before a scan is even attempted -- better a short wait here
  // than a punch stored with a garbage epoch. Normally only true for the
  // first ~15-30s after boot (loop() retries syncTimeIfNeeded() every 15s).
  if (!timeSynced) {
    setAuraError();
    showStatus(STATUS_ERROR, "Clock", "Syncing...");
    beepError();
    delay(900);
    setAuraIdle();
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

    appendPunch(id, getUtcEpochNow()); // instant, no network wait -- bulk-synced later
    (void)score; // no longer forwarded anywhere; kept only because searchDatabase() requires the out-param

    showStatus(STATUS_OK, "Recorded", idLine);
    beepSuccess();
    delay(900);
    setAuraIdle();

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

/* "Restart" menu item: manual reboot, e.g. to recover from a stuck state
   without a power cycle. */
void doRestart() {
  showStatus(STATUS_WIFI, "Device", "Restarting");
  beepSuccess();
  delay(600);
  ESP.restart();
}

/* ---------------- Arduino lifecycle ---------------- */
void setup() {
  Serial.begin(115200);

  secureClient.setInsecure(); // no cert pinning -- backend host may not use a CA in the ESP32's trust store

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  pinMode(SENSOR_TOUCH_PIN, INPUT_PULLDOWN);
  attachInterrupt(digitalPinToInterrupt(SENSOR_TOUCH_PIN), handleFingerTouchISR, RISING);

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
  attachInterrupt(digitalPinToInterrupt(ENCODER_SW_PIN), handleEncoderButtonISR, CHANGE);

  // DIYables_TFT_SPI's begin() doesn't return a success flag (unlike the old
  // SSD1306 library), so there's no equivalent hard-stop check here -- wiring
  // problems will just show as a blank/garbled screen.
  TFT_display.begin();
  TFT_display.setRotation(2); // portrait (vertical), flipped 180 from rotation 0 for this panel's mounting

  showStatus(STATUS_FINGER, "Boot", "Starting");
  delay(500);

  loadConfig(); // also restores the punch buffer and registration flag
  runConfigPortal(false); // try saved Wi-Fi; opens "Device-Setup" portal only if that fails

  if (WiFi.status() == WL_CONNECTED && !deviceRegistered) {
    registerDeviceIfNeeded(); // one shot at boot; loop()'s timer retries it if this fails
  }

  if (!initFingerprintSensor()) {
    setAuraError();
    showStatus(STATUS_ERROR, "Sensor", "Not Ready");
    delay(1000);
  }

  setAuraIdle();
  syncTimeIfNeeded();
  enterClockState();
}

void loop() {
  static int32_t lastReportedDetent = 0;
  static uint32_t pressStartMs = 0;
  static bool longPressFired = false;
  static uint32_t lastClockUpdateMs = 0;
  static uint32_t lastSyncAttemptMs = 0;
  static uint32_t lastMenuActivityMs = 0;
  static uint32_t lastShortReleaseMs = 0;

  // ---- Encoder rotation: only meaningful while the menu is showing ----
  if (appState == STATE_MENU) {
    int32_t detents = readEncoderDetents();
    if (detents != lastReportedDetent) {
      int32_t diff = detents - lastReportedDetent;
      lastReportedDetent = detents;

      // Clockwise (diff > 0) advances to the next item, counter-clockwise
      // (diff < 0) to the previous one; wraps around either end of the menu.
      uint8_t oldIndex = menuIndex;
      int32_t newIndex = ((int32_t)menuIndex + diff) % (int32_t)menuCount;
      if (newIndex < 0) newIndex += menuCount;
      // Update menuIndex BEFORE redrawing -- drawMenuItem() decides
      // selected/unselected by comparing against the live menuIndex, so
      // redrawing first (with the old value still in place) highlighted the
      // outgoing row instead of clearing it.
      menuIndex = (uint8_t)newIndex;
      updateMenuSelection(oldIndex, menuIndex);
      lastMenuActivityMs = millis();
    }
  } else {
    // Keep the baseline current so a stray rotation on the clock screen
    // doesn't cause a jump the moment the menu opens.
    lastReportedDetent = readEncoderDetents();
  }

  // ---- Encoder button: short press = select, long press = open/close menu ----
  bool downFlag, upFlag, downNow;
  noInterrupts();
  downFlag = buttonDownFlag; buttonDownFlag = false;
  upFlag = buttonUpFlag; buttonUpFlag = false;
  downNow = encoderButtonDown;
  interrupts();

  if (downFlag) {
    pressStartMs = millis();
    longPressFired = false;
    lastMenuActivityMs = millis();
  }

  if (downNow && !longPressFired && (millis() - pressStartMs >= LONG_PRESS_MS)) {
    longPressFired = true;
    if (appState == STATE_CLOCK) {
      lastReportedDetent = readEncoderDetents(); // don't carry over stray rotation into the menu
      drawMenu();
      appState = STATE_MENU;
      lastMenuActivityMs = millis();
    } else {
      enterClockState();
    }
  }

  if (upFlag && !longPressFired) {
    if (appState == STATE_MENU) {
      setAuraProcessing();
      switch (menuIndex) {
        case 0: doLogin(); break;
        case 1: doEnroll(); break;
        case 2: runConfigPortal(true); break;
        case 3: checkForOTAUpdate(); break;
        case 4: doRestart(); break;
        default: break;
      }
      setAuraIdle();
      drawMenu();
      lastReportedDetent = readEncoderDetents(); // discard ticks that piled up during the (blocking) action
      lastMenuActivityMs = millis();
    } else {
      // Double-tap on the clock screen: two short taps within
      // DOUBLE_TAP_WINDOW_MS jump straight into login, same as a sensed
      // finger touch. A single tap alone does nothing.
      uint32_t now = millis();
      if (now - lastShortReleaseMs <= DOUBLE_TAP_WINDOW_MS) {
        lastShortReleaseMs = 0; // consumed -- don't chain a third tap into another double-tap
        triggerLoginFromClock();
      } else {
        lastShortReleaseMs = now;
      }
    }
  }

  // ---- Finger touch on the sensor: jump straight into login from the clock ----
  if (appState == STATE_CLOCK) {
    bool touched;
    noInterrupts();
    touched = fingerTouchFlag;
    fingerTouchFlag = false;
    interrupts();

    if (touched) triggerLoginFromClock();
  }

  // ---- Menu idle timeout: fall back to the clock after 10s of inactivity ----
  if (appState == STATE_MENU && millis() - lastMenuActivityMs >= MENU_IDLE_TIMEOUT_MS) {
    enterClockState();
  }

  // ---- Clock tick / opportunistic re-sync ----
  if (appState == STATE_CLOCK) {
    if (!timeSynced && millis() - lastSyncAttemptMs > 15000) {
      lastSyncAttemptMs = millis();
      syncTimeIfNeeded();
    }
    if (millis() - lastClockUpdateMs >= 1000) {
      lastClockUpdateMs = millis();
      drawClockTime();
    }
  }

  // ---- Background: device registration retry, punch-buffer flush, enroll-list refresh ----
  // At most one blocking HTTP call per tick, in priority order: registration
  // first (nothing else works until it succeeds), then punch sync (data-loss
  // risk), then the enroll-list refresh (just a UI count, lowest priority
  // and its own longer interval). Only the enroll-list refresh redraws the
  // TFT (to update the "Enrolls (N)" badge if the menu happens to be open) --
  // registration/punch-sync never touch the screen; the only screen touch
  // outside this block is appendPunch()'s "Buffer Full" message.
  if (millis() - lastPunchSyncMs > PUNCH_SYNC_INTERVAL_MS) {
    lastPunchSyncMs = millis();
    if (WiFi.status() == WL_CONNECTED) {
      if (!deviceRegistered) {
        registerDeviceIfNeeded();
      } else if (punchBufferCount > 0) {
        syncPunchBuffer();
      } else if (millis() - lastEnrollListRefreshMs > ENROLL_LIST_REFRESH_INTERVAL_MS) {
        lastEnrollListRefreshMs = millis();
        if (fetchEmployeesWithoutEnrollment() && appState == STATE_MENU) drawMenu();
      }
    }
  }

  delay(5);
}
