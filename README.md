# TRMNL Hebrew Clock

Firmware that turns a TRMNL DIY e-paper kit (Seeed XIAO ESP32-S3 with a 7.5" black-and-white panel) into a clock. Once a minute the device wakes up, downloads a ready-made clock image from a server, shows it on the display and goes back to deep sleep.

The clock face itself is rendered by the server. The firmware only fetches and displays a PNG, so the look of the clock (font, location, calendar) is controlled by the settings stored on the server for your user name.

## Hardware

- Seeed Studio XIAO ESP32-S3
- 7.5" 800x480 black-and-white e-paper panel (GDEY075T7), as used in the TRMNL OG DIY kit

Display wiring used by the firmware:

| Display pin | GPIO |
|-------------|------|
| BUSY        | 4    |
| RST         | 38   |
| DC          | 10   |
| CS          | 44   |
| SCK         | 7    |
| MOSI        | 9    |

## Build and flash

The project uses [PlatformIO](https://platformio.org/). Libraries are downloaded automatically on the first build.

```
pio run -t upload
pio device monitor
```

The serial monitor runs at 115200 baud.

## First-time setup

1. Power the device. With no saved Wi-Fi network it shows a "WiFi Setup Mode" screen and opens an access point named `HebrewClock-Setup`.
2. Connect to that access point from a phone or computer and open the configuration page.
3. Choose your Wi-Fi network and enter its password.
4. Fill in the two extra fields:
   - **User Name**: your user name on the clock server.
   - **Server URL**: the clock server address. Defaults to `https://clock.udyklimer.com`. If you leave out the scheme, `https://` is assumed.
5. Save. The device connects and shows the clock.

The portal closes after 3 minutes without input. The device then sleeps for a minute and tries again.

### Changing the settings later

Press the reset button twice within 5 seconds. The device opens the `HebrewClock-Setup` portal again, with the current values filled in.

The portal also opens by itself if Wi-Fi is connected but no user name has been set.

## How it works

On each wake the firmware:

1. Loads the user name and server URL from flash (NVS).
2. Connects to Wi-Fi with WiFiManager.
3. Downloads `<server>/clock.png?user=<user name>`.
4. Decodes the PNG, converts each pixel to black or white and draws it with a full display refresh.
5. Reads the time over NTP and sleeps until about 2 seconds before the next minute, so the next image is on screen as the minute changes. If NTP is unavailable it sleeps for 60 seconds.

## Server requirements

Any server can be used as long as `/clock.png` returns:

- a PNG no wider than 800 pixels (800x480 fills the display)
- a `Content-Length` header, since the image is read into memory in one piece

Both `http://` and `https://` are supported. HTTPS certificates are not verified.

## Libraries

- [GxEPD2](https://github.com/ZinggJM/GxEPD2): e-paper display driver
- [PNGdec](https://github.com/bitbank2/PNGdec): PNG decoder
- [WiFiManager](https://github.com/tzapu/WiFiManager): Wi-Fi and settings portal
- [ESP_DoubleResetDetector](https://github.com/khoih-prog/ESP_DoubleResetDetector): double-reset detection

## License

Released under the [MIT License](LICENSE). The libraries listed above have their own licenses.
