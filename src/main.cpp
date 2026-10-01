#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <GxEPD2_BW.h>
#include <PNGdec.h>
#include <Preferences.h>
#include <time.h>

#define ESP_DRD_USE_EEPROM true
#define DOUBLERESETDETECTOR_DEBUG false
#include <ESP_DoubleResetDetector.h>

// Double reset detector settings (5s timeout)
#define DRD_TIMEOUT 5
#define DRD_ADDRESS 0
DoubleResetDetector drd(DRD_TIMEOUT, DRD_ADDRESS);

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

// Always use full refresh with the GDEY075T7's fast OTP waveform (~1.2s).
// Partial refresh on this panel accumulates ghosting that only a full refresh clears.

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

// Build final image URL based on server and username
String buildImageUrl(String srv, const String& user) {
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

  String url = srv + "/clock.png";
  if (user.length() > 0) {
    url += "?user=" + user;
  }
  return url;
}

void showSetupScreen() {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawRect(20, 20, display.width() - 40, display.height() - 40, GxEPD_BLACK);
    
    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(3);
    display.setCursor(60, 100);
    display.print("WiFi Setup Mode");

    display.setTextSize(2);
    display.setCursor(60, 180);
    display.print("1. Connect to Wi-Fi:");
    display.setCursor(80, 220);
    display.print("   'HebrewClock-Setup'");

    display.setCursor(60, 280);
    display.print("2. Configure WiFi & Username");
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
  delay(500);

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

  // Check double reset (only if not waking from deep sleep)
  bool forceConfigPortal = false;
  if (esp_reset_reason() != ESP_RST_DEEPSLEEP) {
    if (drd.detectDoubleReset()) {
      Serial.println("Double reset detected! Forcing config portal...");
      forceConfigPortal = true;
    }
  }

  WiFiManager wm;
  wm.setConnectTimeout(10);
  wm.setConfigPortalTimeout(180);

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

  bool portalShown = false;
  wm.setAPCallback([&portalShown](WiFiManager *myWiFiManager) {
    Serial.println("Could not connect to saved WiFi. Opening Config Portal...");
    portalShown = true;
    showSetupScreen();
  });

  bool connected = false;
  if (forceConfigPortal) {
    portalShown = true;
    showSetupScreen();
    connected = wm.startConfigPortal("HebrewClock-Setup");
  } else {
    // Attempt auto-connect to saved WiFi, fallback to Access Point if connection fails
    connected = wm.autoConnect("HebrewClock-Setup");
  }

  // If connected to Wi-Fi but username has never been configured, force portal open
  if (connected && username.length() == 0 && strlen(custom_username.getValue()) == 0) {
    Serial.println("Username not yet configured. Opening Config Portal...");
    portalShown = true;
    showSetupScreen();
    connected = wm.startConfigPortal("HebrewClock-Setup");
  }




  // Stop double reset detector state
  drd.stop();

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
    esp_sleep_enable_timer_wakeup(60 * 1000000ULL);
    esp_deep_sleep_start();
  }

  Serial.println("WiFi Connected! IP: " + WiFi.localIP().toString());
  Serial.printf("Configured User: '%s' | Server: '%s'\n", username.c_str(), server.c_str());

  // Sync time via NTP so we can sleep until the next exact minute boundary
  configTime(0, 0, "pool.ntp.org", "time.google.com");

  // Construct URL to fetch image
  String imageUrl = buildImageUrl(server, username);
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

  if (httpBeginSuccess) {
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      int len = http.getSize();
      if (len > 0) {
        uint8_t* buffer = (uint8_t*)malloc(len);
        if (buffer) {
          WiFiClient* stream = http.getStreamPtr();
          stream->readBytes(buffer, len);

          // Full refresh every time using the panel's fast OTP waveform (~1.2s, ghost-free)
          Serial.println("Display: Full refresh (fast OTP waveform)...");
          display.setFullWindow();
          display.firstPage();
          do {
            display.fillScreen(GxEPD_WHITE);
            int rc = png.openRAM(buffer, len, pngDraw);
            if (rc == PNG_SUCCESS) { png.decode(NULL, 0); png.close(); }
          } while (display.nextPage());

          free(buffer);
          Serial.println("Display update complete!");
        } else {
          Serial.println("Error: Failed to allocate memory for image buffer");
        }
      } else {
        Serial.printf("Invalid image length: %d\n", len);
      }
    } else {
      Serial.printf("HTTP GET failed, error code: %d\n", httpCode);
    }
    http.end();
  } else {
    Serial.println("HTTP begin failed!");
  }

  // Turn off display power to preserve battery and maintain image state
  display.powerOff();

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

  esp_sleep_enable_timer_wakeup((uint64_t)sleepSec * 1000000ULL);
  esp_deep_sleep_start();
}

void loop() {
  // Empty loop as the ESP32 operates entirely in Deep Sleep cycle
}