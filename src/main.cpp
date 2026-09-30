#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <GxEPD2_BW.h>
#include <PNGdec.h>

// Official pin setup for Seeed Studio / TRMNL OG DIY Kit
#define EPD_BUSY 4   // GPIO 4
#define EPD_RST  38  // GPIO 38
#define EPD_DC   10  // GPIO 10
#define EPD_CS   44  // GPIO 44

#define EPD_SCK  7   // GPIO 7
#define EPD_MISO -1  
#define EPD_MOSI 9   // GPIO 9

GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT> display(
  GxEPD2_750_T7(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY)
);

PNG png;

const char* IMAGE_URL = "https://clock.udyklimer.com/clock.png?font=DavidLibre-Bold&location=Haifa&calendar=gregorian&sleeptime=0";
#define TIME_TO_SLEEP 55 

void showSetupScreen() {
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
    display.print("2. Configure your WiFi");
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

  // Initialize display for clean, full-waveform refresh (prevents ghosting and blurry text)
  display.init(115200, true, 20, false);
  display.setRotation(0);

  WiFiManager wm;
  wm.setConnectTimeout(10);
  wm.setConfigPortalTimeout(180);

  wm.setAPCallback([](WiFiManager *myWiFiManager) {
    Serial.println("Could not connect to saved WiFi. Opening Config Portal...");
    showSetupScreen();
  });

  // Attempt auto-connect to saved WiFi, fallback to Access Point if connection fails
  bool res = wm.autoConnect("HebrewClock-Setup");

  if (!res) {
    Serial.println("Failed to connect or hit portal timeout. Going to sleep...");
    esp_sleep_enable_timer_wakeup(60 * 1000000ULL);
    esp_deep_sleep_start();
  }

  Serial.println("WiFi Connected! IP: " + WiFi.localIP().toString());

  // Fetch image from clock server
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  Serial.println("Fetching image from: " + String(IMAGE_URL));
  
  if (http.begin(client, IMAGE_URL)) {
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      int len = http.getSize();
      if (len > 0) {
        uint8_t* buffer = (uint8_t*)malloc(len);
        if (buffer) {
          WiFiClient* stream = http.getStreamPtr();
          stream->readBytes(buffer, len);

          // Perform full-window clean update
          display.setFullWindow();

          Serial.println("Decoding PNG & Drawing...");
          display.firstPage();
          do {
            display.fillScreen(GxEPD_WHITE);
            int rc = png.openRAM(buffer, len, pngDraw);
            if (rc == PNG_SUCCESS) {
              png.decode(NULL, 0);
              png.close();
            }
          } while (display.nextPage());

          free(buffer);
          Serial.println("Display update complete!");
        }
      }
    } else {
      Serial.printf("HTTP GET failed, error code: %d\n", httpCode);
    }
    http.end();
  }

  // Turn off display power to preserve battery and maintain image state
  display.powerOff();

  Serial.printf("Entering Deep Sleep for %d seconds...\n", TIME_TO_SLEEP);
  esp_sleep_enable_timer_wakeup((uint64_t)TIME_TO_SLEEP * 1000000ULL);
  esp_deep_sleep_start();
}

void loop() {
  // Empty loop as the ESP32 operates entirely in Deep Sleep cycle
}