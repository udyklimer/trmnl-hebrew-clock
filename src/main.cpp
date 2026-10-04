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
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSans12pt7b.h>

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
// state leaves out only the charging parameter.
String buildImageUrl(const String& srv, const String& user, int batteryMv, int charging) {
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
  return url;
}

// Deep sleep for the given time, waking early if either button is pressed
void enterDeepSleep(uint32_t seconds) {
  // Keep the buttons pulled up while sleeping so only a press pulls them low
  rtc_gpio_pullup_en((gpio_num_t)SETUP_BUTTON);
  rtc_gpio_pulldown_dis((gpio_num_t)SETUP_BUTTON);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)SETUP_BUTTON, 0);
  rtc_gpio_pullup_en((gpio_num_t)REFRESH_BUTTON);
  rtc_gpio_pulldown_dis((gpio_num_t)REFRESH_BUTTON);
  esp_sleep_enable_ext1_wakeup(1ULL << REFRESH_BUTTON, ESP_EXT1_WAKEUP_ANY_LOW);
  esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
  esp_deep_sleep_start();
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

void showSetupScreen(const String& hotspotName) {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawRect(20, 20, display.width() - 40, display.height() - 40, GxEPD_BLACK);

    display.setFont(NULL);
    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(3);
    display.setCursor(60, 100);
    display.print("WiFi Setup Mode");

    display.setTextSize(2);
    display.setCursor(60, 180);
    display.print("1. Connect to Wi-Fi:");
    display.setCursor(80, 220);
    display.print("   '" + hotspotName + "'");

    display.setCursor(60, 280);
    display.print("2. Configure WiFi & Username");
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

  // Read the battery once per wake, before Wi-Fi starts
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
    // Nothing special to do: every wake fetches a fresh image and then sleeps
    // until the next minute boundary, which is exactly what a refresh needs.
    Serial.println("Refresh button pressed. Refreshing now...");
  }

  // Initialize SPI bus for display
  SPI.end();
  SPI.begin(EPD_SCK, EPD_MISO, EPD_MOSI, EPD_CS);

  // Always initialize with initial=true to use the fast full-refresh OTP waveform.
  // The GDEY075T7 full refresh is ~1.2s (fast, clean, no ghosting).
  display.init(115200, true, 20, false);
  display.setRotation(0);

  // Load saved preferences from flash
  String username = loadPreference("username", "");
  String server = loadPreference("server", DEFAULT_SERVER);

  WiFiManager wm;
  wm.setConnectTimeout(WIFI_CONNECT_TIMEOUT_MS / 1000);
  wm.setConfigPortalTimeout(SETUP_PORTAL_TIMEOUT_SEC);

  // Setup custom WiFiManager parameters
  char custom_username_buf[64] = {0};
  char custom_server_buf[128] = {0};
  username.toCharArray(custom_username_buf, sizeof(custom_username_buf));
  server.toCharArray(custom_server_buf, sizeof(custom_server_buf));

  WiFiManagerParameter custom_username("username", "User Name", custom_username_buf, sizeof(custom_username_buf));
  WiFiManagerParameter custom_server("server", "Server URL (e.g. https://clock.udyklimer.com)", custom_server_buf, sizeof(custom_server_buf));

  wm.addParameter(&custom_username);
  wm.addParameter(&custom_server);

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
    showSetupScreen(hotspotName);
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
    rememberConnectedNetwork();
  }

  // Retrieve values from custom fields
  String entered_user = String(custom_username.getValue());
  entered_user.trim();
  String entered_server = String(custom_server.getValue());
  entered_server.trim();
  if (entered_server.length() == 0) {
    entered_server = DEFAULT_SERVER;
  }

  if (configSaved || entered_user != username || entered_server != server) {
    username = entered_user;
    server = entered_server;
    savePreferences(username, server);
  }

  if (!connected) {
    Serial.println("Failed to connect or hit portal timeout. Going to sleep...");
    enterDeepSleep(60);
  }

  Serial.println("WiFi Connected! IP: " + WiFi.localIP().toString());
  Serial.printf("Configured User: '%s' | Server: '%s'\n", username.c_str(), server.c_str());

  // Sync time via NTP so we can sleep until the next exact minute boundary
  configTzTime(clockTimezone, "pool.ntp.org", "time.google.com");

  // Construct URL to fetch image
  String imageUrl = buildImageUrl(server, username, batteryMv, chargingState);
  Serial.println("Fetching image from: " + imageUrl);

  HTTPClient http;
  http.setTimeout(15000);
  bool httpBeginSuccess = false;

  WiFiClient standardClient;
  WiFiClientSecure secureClient;

  if (imageUrl.startsWith("https://")) {
    secureClient.setInsecure();
    httpBeginSuccess = http.begin(secureClient, imageUrl);
  } else {
    httpBeginSuccess = http.begin(standardClient, imageUrl);
  }

  bool imageShown = false;
  FirmwareOffer firmwareOffer;
  char failure[48] = "";

  if (httpBeginSuccess) {
    static const char* const clockHeaders[] = {"X-Clock-Timezone"};
    collectFirmwareHeaders(http, clockHeaders, 1);
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      firmwareOffer = readFirmwareOffer(http);

      String tz = http.header("X-Clock-Timezone");
      tz.trim();
      if (tz.length() > 0 && tz.length() < sizeof(clockTimezone) && tz != clockTimezone) {
        strlcpy(clockTimezone, tz.c_str(), sizeof(clockTimezone));
        setenv("TZ", clockTimezone, 1);
        tzset();
        Serial.printf("Clock time zone set to %s\n", clockTimezone);
      }

      int len = http.getSize();
      uint8_t* buffer = len > 0 ? (uint8_t*)malloc(len) : nullptr;
      if (buffer == nullptr) {
        Serial.printf("Invalid image length or out of memory: %d\n", len);
        strlcpy(failure, "Invalid image from server", sizeof(failure));
      } else if (http.getStreamPtr()->readBytes(buffer, len) != (size_t)len) {
        Serial.println("Image download incomplete");
        strlcpy(failure, "No internet", sizeof(failure));
      } else if (png.openRAM(buffer, len, pngDraw) != PNG_SUCCESS) {
        // E.g. a hotel or captive-portal network answering with a web page
        Serial.println("Downloaded file is not a PNG");
        strlcpy(failure, "Invalid image from server", sizeof(failure));
      } else {
        png.close();
        // Full refresh every time using the panel's fast OTP waveform (~1.2s, ghost-free)
        Serial.println("Display: Full refresh (fast OTP waveform)...");
        display.setFullWindow();
        display.firstPage();
        int rc;
        do {
          display.fillScreen(GxEPD_WHITE);
          rc = png.openRAM(buffer, len, pngDraw);
          if (rc == PNG_SUCCESS) { png.decode(NULL, 0); png.close(); }
        } while (display.nextPage());
        imageShown = (rc == PNG_SUCCESS);
        Serial.println("Display update complete!");
      }
      free(buffer);
    } else if (httpCode < 0) {
      // No connection to the server at all: DNS, TCP or TLS failed, or timed out
      Serial.printf("HTTP GET failed: %s\n", HTTPClient::errorToString(httpCode).c_str());
      strlcpy(failure, "No internet", sizeof(failure));
    } else {
      Serial.printf("HTTP GET failed, error code: %d\n", httpCode);
      snprintf(failure, sizeof(failure), "Server error (HTTP %d)", httpCode);
    }
    http.end();
  } else {
    Serial.println("HTTP begin failed!");
    strlcpy(failure, "No internet", sizeof(failure));
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

  // Install an offered update only now, so a slow download never delays the minute.
  // Returns only if nothing was installed; on success the device restarts.
  if (imageShown && firmwareOffer.present) {
    if (chargingState == 1 || batteryMv >= OTA_MIN_BATTERY_MV) {
      installFirmwareUpdate(firmwareOffer, normalizeServerUrl(server));
    } else {
      Serial.printf("OTA: %s offered, waiting for USB power or %d mV battery\n",
                    firmwareOffer.version.c_str(), OTA_MIN_BATTERY_MV);
    }
  }

  // Calculate sleep duration to wake up right at the next minute boundary.
  // We subtract DISPLAY_UPDATE_SEC so the display finishes updating before the minute ticks.
  uint32_t sleepSec = SLEEP_FALLBACK_SEC;
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 3000)) {
    int secsIntoMinute = timeinfo.tm_sec;
    // Sleep until next minute, minus display update time so we're ready right on the minute
    int secsUntilNextMinute = 60 - secsIntoMinute - DISPLAY_UPDATE_SEC;
    if (secsUntilNextMinute <= 0) secsUntilNextMinute += 60; // already past the window
    sleepSec = (uint32_t)secsUntilNextMinute;
    Serial.printf("NTP time: %02d:%02d:%02d | Sleeping %u sec to sync with next minute boundary\n",
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, sleepSec);
  } else {
    Serial.printf("NTP sync failed. Falling back to %u sec sleep.\n", sleepSec);
  }

  enterDeepSleep(sleepSec);
}

void loop() {
  // Empty loop as the ESP32 operates entirely in Deep Sleep cycle
}