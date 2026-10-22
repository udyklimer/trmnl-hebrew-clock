#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <GxEPD2_BW.h>
#include <PNGdec.h>
#include <Preferences.h>
#include <time.h>
#include <driver/rtc_io.h>
#include "firmware_update.h"
#include "wifi_networks.h"
#include "image_batch.h"
#include <esp_sntp.h>
#include <sys/time.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <qrcode.h>

// KEY1 on the TRMNL OG DIY Kit (D1, active low). KEY2 is D2 and KEY3 is D4.
// One press wakes the device from deep sleep (handy before a firmware upload);
// a second press within SETUP_PRESS_WINDOW_MS opens the config portal.
#define SETUP_BUTTON D1
#define SETUP_PRESS_WINDOW_MS 3000

// KEY2 (D2, active low): one press wakes the device and refreshes the clock
// right away, e.g. after changing the settings on the server.
#define REFRESH_BUTTON D2

// Official pin setup for Seeed Studio / TRMNL OG DIY Kit
#define EPD_BUSY 4   // GPIO 4
#define EPD_RST  38  // GPIO 38
#define EPD_DC   10  // GPIO 10
#define EPD_CS   44  // GPIO 44

#define EPD_SCK  7   // GPIO 7
#define EPD_MISO -1  
#define EPD_MOSI 9   // GPIO 9

GxEPD2_BW<GxEPD2_750_GDEY075T7, GxEPD2_750_GDEY075T7::HEIGHT> display(
  GxEPD2_750_GDEY075T7(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

PNG png;

const char* DEFAULT_SERVER = "https://clock.udyklimer.com";
#define SLEEP_FALLBACK_SEC 60       // Fallback if NTP sync fails
#define DISPLAY_UPDATE_SEC 2        // Buffer: seconds the display update takes

#define WIFI_CONNECT_TIMEOUT_MS 10000   // Per network, on each connection attempt
#define SETUP_PORTAL_TIMEOUT_SEC 180    // The portal closes after this long without use
#define PORTAL_RETRY_INTERVAL_MS 30000  // While the portal is open after Wi-Fi was lost
#define NTP_TIMEOUT_MS 5000             // How long a fetch wake waits for the time sync
#define MAX_BATCH_MINUTES 15            // Upper limit for the server's X-Batch-Minutes

// Time of the last NTP sync, to report how far the internal clock drifts between syncs
RTC_DATA_ATTR time_t lastClockSync = 0;

// Per-wake measurements, printed in one [TIMING] line just before deep sleep
static const char* wakeType = "fetch";
static long wifiMs = -1;
static long ntpMs = -1;
static long displayMs = -1;
static String downloadMs;  // Comma-separated, one entry per image download

// Time zone for the clock the device draws itself when the server can't be
// reached (POSIX TZ format). Defaults to Israel; the server can override it
// with an X-Clock-Timezone header, which is remembered across deep sleep.
#define DEFAULT_TIMEZONE "IST-2IDT,M3.4.4/26,M10.5.0"
RTC_DATA_ATTR char clockTimezone[48] = DEFAULT_TIMEZONE;

// Always use full refresh with the GDEY075T7's fast OTP waveform (~1.2s).
// Partial refresh on this panel accumulates ghosting that only a full refresh clears.

// Battery measurement on the TRMNL OG DIY Kit: voltage on GPIO 1 (D0/A0),
// with the measuring circuit switched on by driving GPIO 6 (A5) HIGH.
#define BATTERY_ADC_PIN 1
#define BATTERY_ENABLE_PIN 6
#define BATTERY_SAMPLES 16
#define BATTERY_MIN_MV 2500         // Readings outside this range are treated as failed
#define BATTERY_MAX_MV 4500
// USB power lifts the reading by about 50 mV on the very next wake, while the
// wake-to-wake noise is under 10 mV, so a step larger than this means USB was
// plugged in (rise) or unplugged (drop).
#define CHARGING_STEP_MV 30
// Firmware updates are installed only on USB power or above this battery level
#define OTA_MIN_BATTERY_MV 3700

// Kept across deep sleep to compare each wake with the previous one
#define CHARGING_UNKNOWN -1
RTC_DATA_ATTR int previousBatteryMv = 0;              // 0 = no previous reading
RTC_DATA_ATTR int chargingState = CHARGING_UNKNOWN;   // -1 unknown, 0 on battery, 1 on USB

// Read the battery voltage in millivolts, or 0 if the reading is implausible
int readBatteryMv() {
  pinMode(BATTERY_ENABLE_PIN, OUTPUT);
  digitalWrite(BATTERY_ENABLE_PIN, HIGH);
  analogReadResolution(12);
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
  delay(10);

  uint32_t sum = 0;
  for (int i = 0; i < BATTERY_SAMPLES; i++) {
    sum += analogRead(BATTERY_ADC_PIN);
    delay(2);
  }
  digitalWrite(BATTERY_ENABLE_PIN, LOW);

  // Conversion from Seeed's Arduino guide for this kit
  float raw = (float)sum / BATTERY_SAMPLES;
  int mv = (int)((raw / 4095.0f) * 3.6f * 2.0f * 0.968f * 1000.0f + 0.5f);
  if (mv < BATTERY_MIN_MV || mv > BATTERY_MAX_MV) {
    return 0;
  }
  return mv;
}

// Update the charging state from the voltage step since the previous wake
void updateChargingState(int batteryMv) {
  if (batteryMv > 0 && previousBatteryMv > 0) {
    int step = batteryMv - previousBatteryMv;
    if (step > CHARGING_STEP_MV) {
      chargingState = 1;
    } else if (step < -CHARGING_STEP_MV) {
      chargingState = 0;
    }
  }
  previousBatteryMv = batteryMv;
}

// Read setting from ESP32 NVS
String loadPreference(const char* key, const char* defaultVal) {
  Preferences p;
  p.begin("clock_cfg", true);
  String val = p.getString(key, defaultVal);
  p.end();
  return val;
}

// Save settings to ESP32 NVS
void savePreferences(const String& user, const String& srv) {
  Preferences p;
  p.begin("clock_cfg", false);
  p.putString("username", user);
  p.putString("server", srv);
  p.end();
  Serial.printf("Saved to NVS: username='%s', server='%s'\n", user.c_str(), srv.c_str());
}

// Normalize the configured server address into a base URL without trailing slash
String normalizeServerUrl(String srv) {
  srv.trim();
  if (srv.length() == 0) {
    srv = DEFAULT_SERVER;
  }
  // Default to https if no protocol specified
  if (!srv.startsWith("http://") && !srv.startsWith("https://")) {
    srv = "https://" + srv;
  }
  // Strip trailing slashes
  while (srv.endsWith("/")) {
    srv.remove(srv.length() - 1);
  }
  return srv;
}

// Build final image URL based on server, username, firmware version and battery status.
// batteryMv <= 0 leaves out both battery parameters; an unknown charging
// state leaves out only the charging parameter. at != 0 asks for the minute
// containing that Unix time instead of the current one.
String buildImageUrl(const String& srv, const String& user, int batteryMv, int charging, time_t at) {
  String url = normalizeServerUrl(srv) + "/clock.png";
  char sep = '?';
  if (user.length() > 0) {
    url += sep;
    url += "user=" + user;
    sep = '&';
  }
  url += sep;
  url += "fw=" FW_VERSION;
  sep = '&';
  if (batteryMv > 0) {
    url += sep;
    url += "battery_mv=" + String(batteryMv);
    sep = '&';
    if (charging != CHARGING_UNKNOWN) {
      url += sep;
      url += "charging=" + String(charging);
    }
  }
  if (at != 0) {
    url += "&at=" + String((unsigned long)at);
  }
  return url;
}

// Deep sleep for the given time, waking early if either button is pressed
void deepSleepMicros(uint64_t micros) {
  Serial.printf("[TIMING] wake=%s wifi=%ldms ntp=%ldms downloads=[%s]ms display=%ldms total=%lums\n",
                wakeType, wifiMs, ntpMs, downloadMs.c_str(), displayMs, millis());
  Serial.flush();
  // Keep the buttons pulled up while sleeping so only a press pulls them low
  rtc_gpio_pullup_en((gpio_num_t)SETUP_BUTTON);
  rtc_gpio_pulldown_dis((gpio_num_t)SETUP_BUTTON);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)SETUP_BUTTON, 0);
  rtc_gpio_pullup_en((gpio_num_t)REFRESH_BUTTON);
  rtc_gpio_pulldown_dis((gpio_num_t)REFRESH_BUTTON);
  esp_sleep_enable_ext1_wakeup(1ULL << REFRESH_BUTTON, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_sleep_enable_timer_wakeup(micros);
  esp_deep_sleep_start();
}

void enterDeepSleep(uint32_t seconds) {
  deepSleepMicros((uint64_t)seconds * 1000000ULL);
}

// Sleep until DISPLAY_UPDATE_SEC before the next minute starts, so the next
// image is on screen as the minute changes
void sleepUntilNextMinute() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1700000000) {
    Serial.printf("Time unknown. Falling back to %u sec sleep.\n", SLEEP_FALLBACK_SEC);
    enterDeepSleep(SLEEP_FALLBACK_SEC);
  }
  int64_t nowUs = (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
  int64_t targetSec = (tv.tv_sec / 60 + 1) * 60 - DISPLAY_UPDATE_SEC;
  if ((int64_t)targetSec * 1000000LL - nowUs < 500000LL) targetSec += 60;  // Too close, take the next one
  int64_t sleepUs = (int64_t)targetSec * 1000000LL - nowUs;

  struct tm t;
  localtime_r(&tv.tv_sec, &t);
  Serial.printf("Time: %02d:%02d:%02d | Sleeping %.1f sec until just before the next minute\n",
                t.tm_hour, t.tm_min, t.tm_sec, sleepUs / 1e6);
  deepSleepMicros((uint64_t)sleepUs);
}

// Start of the minute that is about to begin (we wake ~2 s before it), as
// Unix time, or 0 if the time is not known
time_t upcomingMinute() {
  time_t now = time(nullptr);
  if (now < 1700000000) return 0;
  return (now + DISPLAY_UPDATE_SEC + 3) / 60 * 60;
}

// Sync the clock over NTP, first logging how far the internal clock had
// drifted since the previous sync. Keeps the internal clock if NTP fails.
void syncClock() {
  struct timeval before;
  gettimeofday(&before, nullptr);
  bool hadTime = before.tv_sec > 1700000000;
  unsigned long start = millis();

  sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
  configTzTime(clockTimezone, "pool.ntp.org", "time.google.com");
  while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && millis() - start < NTP_TIMEOUT_MS) {
    delay(10);
  }
  ntpMs = millis() - start;
  if (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED) {
    Serial.println("NTP sync failed, keeping the internal clock");
    return;
  }

  struct timeval after;
  gettimeofday(&after, nullptr);
  if (hadTime) {
    // Where the internal clock would be now without the sync, versus NTP
    int64_t expectedUs = (int64_t)before.tv_sec * 1000000LL + before.tv_usec + (int64_t)ntpMs * 1000LL;
    int64_t actualUs = (int64_t)after.tv_sec * 1000000LL + after.tv_usec;
    long driftMs = (long)((expectedUs - actualUs) / 1000);
    long sinceSync = lastClockSync ? (long)(after.tv_sec - lastClockSync) : -1;
    Serial.printf("[CLOCK] internal clock was %+ld ms off NTP (+ = ahead), %ld s after the previous sync\n",
                  driftMs, sinceSync);
  } else {
    Serial.println("[CLOCK] time set by NTP (no previous time to compare)");
  }
  lastClockSync = after.tv_sec;
}

// After a button wake, wait for the button to be released and pressed again
bool waitForSecondPress() {
  unsigned long start = millis();
  bool released = false;
  while (millis() - start < SETUP_PRESS_WINDOW_MS) {
    bool pressed = (digitalRead(SETUP_BUTTON) == LOW);
    if (!released && !pressed) {
      released = true;
      delay(50); // debounce the release
    } else if (released && pressed) {
      return true;
    }
    delay(5);
  }
  return false;
}

// Hotspot name with the end of the MAC address, so nearby clocks can be told apart
String setupHotspotName() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);  // Works before Wi-Fi is started
  char name[32];
  snprintf(name, sizeof(name), "HebrewClock-Setup-%02X%02X", mac[4], mac[5]);
  return String(name);
}

// Where esp_qrcode_generate() should draw; its callback takes no context
static int16_t qrCenterX, qrTop, qrWidth;

static void drawQrModules(esp_qrcode_handle_t qrcode) {
  int size = esp_qrcode_get_size(qrcode);
  int16_t qrScale = qrWidth / size;
  int16_t qrLeft = qrCenterX - size * qrScale / 2;
  for (int y = 0; y < size; y++) {
    for (int x = 0; x < size; x++) {
      if (esp_qrcode_get_module(qrcode, x, y)) {
        display.fillRect(qrLeft + x * qrScale, qrTop + y * qrScale, qrScale, qrScale, GxEPD_BLACK);
      }
    }
  }
}

// Draw a QR code of about `width` pixels, horizontally centered on centerX
void drawQrCode(const String& text, int16_t centerX, int16_t top, int16_t width) {
  esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
  cfg.display_func = drawQrModules;
  cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;
  qrCenterX = centerX;
  qrTop = top;
  qrWidth = width;
  esp_qrcode_generate(&cfg, text.c_str());
}

void printCenteredAt(const char* text, int16_t centerX, int16_t y) {
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  display.setCursor(centerX - w / 2 - x1, y);
  display.print(text);
}

// First-time setup in two steps, each with a QR code to scan with a phone:
// 1. register on the clock server (needs the phone's normal internet, so it
//    comes first), 2. join the clock's hotspot and enter Wi-Fi and user name.
void showSetupScreen(const String& hotspotName, const String& serverUrl) {
  String serverHost = serverUrl.substring(serverUrl.indexOf("://") + 3);
  String wifiQr = "WIFI:T:nopass;S:" + hotspotName + ";;";
  const int16_t left = display.width() / 4;
  const int16_t right = display.width() * 3 / 4;

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(1);

    display.setFont(&FreeSansBold18pt7b);
    printCenteredAt("Set up your clock", display.width() / 2, 50);
    display.drawLine(display.width() / 2, 80, display.width() / 2, display.height() - 20, GxEPD_BLACK);

    // Step 1: account on the server
    display.setFont(&FreeSansBold12pt7b);
    printCenteredAt("1. Create an account", left, 105);
    drawQrCode(serverUrl, left, 125, 180);
    display.setFont(&FreeSans12pt7b);
    printCenteredAt("Scan to sign up at", left, 345);
    printCenteredAt(serverHost.c_str(), left, 375);
    printCenteredAt("Have a user name? Skip this.", left, 425);

    // Step 2: the clock's own hotspot
    display.setFont(&FreeSansBold12pt7b);
    printCenteredAt("2. Connect the clock", right, 105);
    drawQrCode(wifiQr, right, 125, 180);
    display.setFont(&FreeSans12pt7b);
    printCenteredAt("Scan to join", right, 345);
    printCenteredAt(hotspotName.c_str(), right, 375);
    printCenteredAt("Then choose your Wi-Fi", right, 410);
    printCenteredAt("and enter your user name", right, 440);

    display.setFont(NULL);
  } while (display.nextPage());
}

// Print text horizontally centered with its baseline at y
void printCentered(const char* text, int16_t y) {
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
  display.setCursor((display.width() - w) / 2 - x1, y);
  display.print(text);
}

// Shown when Wi-Fi works but the clock image can't be fetched: the device
// draws the time itself, if it knows it, with the reason underneath
void showOfflineScreen(const char* message) {
  char clockText[8] = "";
  time_t now = time(nullptr);
  if (now > 1700000000) {  // The time was set by NTP since the last reset
    // We wake just before the minute changes, so show the minute about to start
    time_t shown = now + DISPLAY_UPDATE_SEC + 3;
    struct tm t;
    localtime_r(&shown, &t);
    snprintf(clockText, sizeof(clockText), "%02d:%02d", t.tm_hour, t.tm_min);
  }

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setTextColor(GxEPD_BLACK);
    if (clockText[0]) {
      display.setFont(&FreeSansBold24pt7b);
      display.setTextSize(3);
      printCentered(clockText, 220);
    }
    display.setTextSize(1);
    display.setFont(&FreeSansBold18pt7b);
    printCentered(message, clockText[0] ? 330 : 230);
    display.setFont(&FreeSans12pt7b);
    printCentered("Retrying every minute", clockText[0] ? 380 : 280);
    display.setFont(NULL);
  } while (display.nextPage());
}

int pngDraw(PNGDRAW *pDraw) {
  uint16_t usPixels[800];
  png.getLineAsRGB565(pDraw, usPixels, PNG_RGB565_LITTLE_ENDIAN, 0x00000000);
  for (int x = 0; x < pDraw->iWidth; x++) {
    uint16_t color = usPixels[x];
    uint8_t r = (color >> 11) & 0x1F;
    uint8_t g = (color >> 5) & 0x3F;
    uint8_t b = color & 0x1F;
    uint32_t brightness = (r * 8) + (g * 4) + (b * 8);
    
    uint16_t epdColor = (brightness < 256) ? GxEPD_BLACK : GxEPD_WHITE;
    display.drawPixel(x, pDraw->y, epdColor);
  }
  return 1;
}

// Draw a PNG on the whole display with a full refresh; false if it can't be decoded
bool drawImage(uint8_t* data, int len) {
  if (png.openRAM(data, len, pngDraw) != PNG_SUCCESS) return false;
  png.close();
  unsigned long start = millis();
  // Full refresh every time using the panel's fast OTP waveform (~1.2s, ghost-free)
  display.setFullWindow();
  display.firstPage();
  int rc;
  do {
    display.fillScreen(GxEPD_WHITE);
    rc = png.openRAM(data, len, pngDraw);
    if (rc == PNG_SUCCESS) { png.decode(NULL, 0); png.close(); }
  } while (display.nextPage());
  displayMs = millis() - start;
  return rc == PNG_SUCCESS;
}

// Display wake: show the stored image for this minute and delete it.
// False if it is missing or broken, in which case the wake becomes a fetch.
bool showStoredImage(time_t minute) {
  size_t len = 0;
  uint8_t* data = batchLoad(minute, &len);
  bool shown = data && drawImage(data, (int)len);
  free(data);
  batchRemove(minute);
  return shown;
}

// Download one image into a malloc'd buffer (caller frees). afterGet runs on a
// 200 response, while its headers can still be read. On failure returns nullptr
// and puts the reason for the offline screen in failure.
uint8_t* downloadImage(HTTPClient& http, WiFiClient& client, const String& url, int* len,
                       char* failure, size_t failureSize, std::function<void()> afterGet = nullptr) {
  unsigned long start = millis();
  uint8_t* buffer = nullptr;
  if (!http.begin(client, url)) {
    Serial.println("HTTP begin failed!");
    strlcpy(failure, "No internet", failureSize);
  } else {
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      if (afterGet) afterGet();
      *len = http.getSize();
      buffer = *len > 0 ? (uint8_t*)malloc(*len) : nullptr;
      if (buffer == nullptr) {
        Serial.printf("Invalid image length or out of memory: %d\n", *len);
        strlcpy(failure, "Invalid image from server", failureSize);
      } else if (http.getStreamPtr()->readBytes(buffer, *len) != (size_t)*len) {
        Serial.println("Image download incomplete");
        strlcpy(failure, "No internet", failureSize);
        free(buffer);
        buffer = nullptr;
      } else if (png.openRAM(buffer, *len, pngDraw) != PNG_SUCCESS) {
        // E.g. a hotel or captive-portal network answering with a web page
        Serial.println("Downloaded file is not a PNG");
        strlcpy(failure, "Invalid image from server", failureSize);
        free(buffer);
        buffer = nullptr;
      } else {
        png.close();
      }
    } else if (httpCode < 0) {
      // No connection to the server at all: DNS, TCP or TLS failed, or timed out
      Serial.printf("HTTP GET failed: %s\n", HTTPClient::errorToString(httpCode).c_str());
      strlcpy(failure, "No internet", failureSize);
    } else {
      Serial.printf("HTTP GET failed, error code: %d\n", httpCode);
      snprintf(failure, failureSize, "Server error (HTTP %d)", httpCode);
    }
    http.end();  // Keeps the connection open for the next request when the server allows it
  }
  if (downloadMs.length() > 0) downloadMs += ",";
  downloadMs += String(millis() - start);
  return buffer;
}

void setup() {
  Serial.begin(115200);

  // If the setup button woke us, a second press opens the config portal
  rtc_gpio_deinit((gpio_num_t)SETUP_BUTTON);
  pinMode(SETUP_BUTTON, INPUT_PULLUP);
  rtc_gpio_deinit((gpio_num_t)REFRESH_BUTTON);
  pinMode(REFRESH_BUTTON, INPUT_PULLUP);
  esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  bool forceConfigPortal = false;
  if (wakeCause == ESP_SLEEP_WAKEUP_EXT0) {
    forceConfigPortal = waitForSecondPress();
  }

  delay(500);

  // Local time zone, for the logs and the clock drawn when offline
  setenv("TZ", clockTimezone, 1);
  tzset();

  // Initialize SPI bus for display
  SPI.end();
  SPI.begin(EPD_SCK, EPD_MISO, EPD_MOSI, EPD_CS);

  // Always initialize with initial=true to use the fast full-refresh OTP waveform.
  // The GDEY075T7 full refresh is ~1.2s (fast, clean, no ghosting).
  display.init(115200, true, 20, false);
  display.setRotation(0);

  // A button press always fetches fresh images, so settings changes show up
  // right away instead of after the stored minutes run out
  bool buttonWake = wakeCause == ESP_SLEEP_WAKEUP_EXT0 || wakeCause == ESP_SLEEP_WAKEUP_EXT1;
  if (buttonWake && batchExists()) {
    Serial.println("Button pressed: discarding stored images");
    batchDiscard();
  }

  // Display wake: in battery save mode, show the stored image for this minute
  // without Wi-Fi. Anything unexpected turns it into a fetch wake.
  if (wakeCause == ESP_SLEEP_WAKEUP_TIMER) {
    time_t minute = upcomingMinute();
    if (minute != 0 && batchCovers(minute)) {
      wakeType = "display";
      if (showStoredImage(minute)) {
        Serial.println("Display wake: showed stored image");
        display.powerOff();
        sleepUntilNextMinute();
      }
      Serial.println("Stored image missing or unreadable, fetching instead");
      wakeType = "fetch(missing)";
    }
  }

  // Fetch wake from here on: Wi-Fi, fresh images, battery and update check

  // Read the battery, before Wi-Fi starts
  int batteryMv = readBatteryMv();
  updateChargingState(batteryMv);
  if (batteryMv > 0) {
    Serial.printf("Battery: %d mV | Charging: %s\n", batteryMv,
                  chargingState == CHARGING_UNKNOWN ? "unknown" : (chargingState ? "yes" : "no"));
  } else {
    Serial.println("Battery: reading failed or out of range, not reported");
  }

  if (forceConfigPortal) {
    Serial.println("Setup button pressed twice. Forcing config portal...");
  }
  if (wakeCause == ESP_SLEEP_WAKEUP_EXT1) {
    Serial.println("Refresh button pressed. Refreshing now...");
  }

  // Load saved preferences from flash
  String username = loadPreference("username", "");
  String server = loadPreference("server", DEFAULT_SERVER);

  WiFiManager wm;
  wm.setConnectTimeout(WIFI_CONNECT_TIMEOUT_MS / 1000);
  wm.setConfigPortalTimeout(SETUP_PORTAL_TIMEOUT_SEC);

  // Setup custom WiFiManager parameters. The server URL is deliberately not
  // offered: a wrong value cuts the device off from its images and updates.
  // It stays in NVS (default DEFAULT_SERVER) for a later firmware to change.
  char custom_username_buf[64] = {0};
  username.toCharArray(custom_username_buf, sizeof(custom_username_buf));

  WiFiManagerParameter custom_username("username", "User Name", custom_username_buf, sizeof(custom_username_buf));

  wm.addParameter(&custom_username);

  bool configSaved = false;
  wm.setSaveConfigCallback([&configSaved]() {
    Serial.println("WiFiManager: Config save callback triggered");
    configSaved = true;
  });
  wm.setSaveParamsCallback([&configSaved]() {
    Serial.println("WiFiManager: Params save callback triggered");
    configSaved = true;
  });

  String hotspotName = setupHotspotName();
  migrateStoredNetwork();

  // Run the setup portal until a network is entered, the settings are saved,
  // or it times out. With retrySaved, saved networks are also tried every
  // PORTAL_RETRY_INTERVAL_MS, so a router that is just restarting is picked
  // up again without anyone touching the portal.
  auto runSetupPortal = [&](bool retrySaved) -> bool {
    configSaved = false;  // Only a save in this portal session closes it
    showSetupScreen(hotspotName, normalizeServerUrl(server));
    wm.setConfigPortalBlocking(false);
    wm.startConfigPortal(hotspotName.c_str());
    unsigned long lastRetry = millis();
    while (wm.getConfigPortalActive()) {
      if (wm.process()) break;  // Connected to a newly entered network
      if (configSaved) {        // Settings saved without a new network
        wm.stopConfigPortal();
        break;
      }
      if (retrySaved && millis() - lastRetry > PORTAL_RETRY_INTERVAL_MS) {
        Serial.println("Portal open, checking for saved networks in range...");
        if (connectToSavedNetworkInRange(WIFI_CONNECT_TIMEOUT_MS)) {
          Serial.println("Saved network is back. Closing Config Portal...");
          wm.stopConfigPortal();
          break;
        }
        lastRetry = millis();
      }
      delay(10);
    }
    // One more try with the saved networks once the portal has closed
    return WiFi.status() == WL_CONNECTED || connectToSavedNetwork(WIFI_CONNECT_TIMEOUT_MS);
  };

  unsigned long wifiStart = millis();
  bool connected = false;
  if (forceConfigPortal) {
    connected = runSetupPortal(false);
  } else {
    connected = connectToSavedNetwork(WIFI_CONNECT_TIMEOUT_MS);
    if (!connected) {
      Serial.println("Could not connect to a saved network. Opening Config Portal...");
      connected = runSetupPortal(savedNetworkCount() > 0);
    }
  }

  // If connected to Wi-Fi but username has never been configured, force portal open
  if (connected && username.length() == 0 && strlen(custom_username.getValue()) == 0) {
    Serial.println("Username not yet configured. Opening Config Portal...");
    connected = runSetupPortal(false);
  }

  if (connected) {
    wifiMs = millis() - wifiStart;
    rememberConnectedNetwork();
  }

  // Retrieve values from custom fields
  String entered_user = String(custom_username.getValue());
  entered_user.trim();

  if (configSaved || entered_user != username) {
    username = entered_user;
    savePreferences(username, server);
  }

  if (!connected) {
    Serial.println("Failed to connect or hit portal timeout. Going to sleep...");
    enterDeepSleep(60);
  }

  Serial.println("WiFi Connected! IP: " + WiFi.localIP().toString());
  Serial.printf("Configured User: '%s' | Server: '%s'\n", username.c_str(), server.c_str());

  // Sync the clock; the requests below name the exact minutes they want
  syncClock();
  time_t firstMinute = upcomingMinute();  // 0 if the time is still unknown

  String baseUrl = normalizeServerUrl(server);
  HTTPClient http;
  http.setTimeout(15000);
  http.setReuse(true);
  WiFiClient standardClient;
  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  WiFiClient& client = baseUrl.startsWith("https://") ? (WiFiClient&)secureClient : standardClient;

  FirmwareOffer firmwareOffer;
  int batchMinutes = 1;
  char failure[48] = "";

  String imageUrl = buildImageUrl(server, username, batteryMv, chargingState, firstMinute);
  Serial.println("Fetching image from: " + imageUrl);
  static const char* const clockHeaders[] = {"X-Clock-Timezone", "X-Batch-Minutes"};
  collectFirmwareHeaders(http, clockHeaders, 2);
  int len = 0;
  uint8_t* buffer = downloadImage(http, client, imageUrl, &len, failure, sizeof(failure), [&]() {
    firmwareOffer = readFirmwareOffer(http);

    String tz = http.header("X-Clock-Timezone");
    tz.trim();
    if (tz.length() > 0 && tz.length() < sizeof(clockTimezone) && tz != clockTimezone) {
      strlcpy(clockTimezone, tz.c_str(), sizeof(clockTimezone));
      setenv("TZ", clockTimezone, 1);
      tzset();
      Serial.printf("Clock time zone set to %s\n", clockTimezone);
    }

    String batch = http.header("X-Batch-Minutes");
    if (batch.length() > 0) {
      batchMinutes = constrain((int)batch.toInt(), 1, MAX_BATCH_MINUTES);
    }
  });

  bool imageShown = false;
  if (buffer) {
    Serial.println("Display: Full refresh (fast OTP waveform)...");
    imageShown = drawImage(buffer, len);
    free(buffer);
    Serial.println("Display update complete!");
  }
  if (!imageShown) {
    showOfflineScreen(failure[0] ? failure : "No internet");
  }

  // Turn off display power to preserve battery and maintain image state
  display.powerOff();

  // This firmware works: keep it, even if it was just installed over the air
  if (imageShown) {
    confirmRunningFirmware();
  }

  // Battery save mode: fetch the following minutes in this same Wi-Fi session
  // and store them, so the next wakes can show them without Wi-Fi. Needs the
  // time, since every request names its minute. Failed minutes are skipped;
  // their wake becomes a fetch wake.
  if (imageShown && batchMinutes > 1 && firstMinute != 0) {
    Serial.printf("Battery save mode: fetching the next %d minutes\n", batchMinutes - 1);
    if (batchStart()) {
      int stored = 0;
      for (int i = 1; i < batchMinutes; i++) {
        time_t minute = firstMinute + 60 * i;
        String url = buildImageUrl(server, username, batteryMv, chargingState, minute);
        char ignored[48];
        int minuteLen = 0;
        uint8_t* data = downloadImage(http, client, url, &minuteLen, ignored, sizeof(ignored));
        if (data && batchSave(minute, data, minuteLen)) stored++;
        free(data);
      }
      Serial.printf("Battery save mode: stored %d of %d minutes\n", stored, batchMinutes - 1);
    }
  } else if (batchExists()) {
    batchDiscard();  // Leftovers from before battery save mode was turned off
  }

  // Install an offered update only now, so a slow download never delays the minute.
  // Returns only if nothing was installed; on success the device restarts.
  if (imageShown && firmwareOffer.present) {
    if (chargingState == 1 || batteryMv >= OTA_MIN_BATTERY_MV) {
      installFirmwareUpdate(firmwareOffer, baseUrl);
    } else {
      Serial.printf("OTA: %s offered, waiting for USB power or %d mV battery\n",
                    firmwareOffer.version.c_str(), OTA_MIN_BATTERY_MV);
    }
  }

  sleepUntilNextMinute();
}

void loop() {
  // Empty loop as the ESP32 operates entirely in Deep Sleep cycle
}