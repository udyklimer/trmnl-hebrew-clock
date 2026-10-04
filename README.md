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

The `KEY1` button (pin `D1`, GPIO 2) is used as the setup button. The `KEY2` button (pin `D2`, GPIO 3) refreshes the clock on demand.

## Build and flash

The project uses [PlatformIO](https://platformio.org/). Libraries are downloaded automatically on the first build.

```
pio run -t upload
pio device monitor
```

The serial monitor runs at 115200 baud.

A local build reports its version as `0.0.0-dev` and never installs over-the-air updates. Released firmware is built by GitHub Actions (see [Firmware updates](#firmware-updates)).

The first firmware with over-the-air update support has to be flashed by cable. After that, updates arrive automatically.

## First-time setup

1. Power the device. With no saved Wi-Fi network it shows a "WiFi Setup Mode" screen and opens an access point named `HebrewClock-Setup-XXXX`, where `XXXX` is the end of the device's MAC address, so several clocks in one place can be told apart. The exact name is shown on the screen.
2. Connect to that access point from a phone or computer and open the configuration page.
3. Choose your Wi-Fi network and enter its password.
4. Fill in the two extra fields:
   - **User Name**: your user name on the clock server.
   - **Server URL**: the clock server address. Defaults to `https://clock.udyklimer.com`. If you leave out the scheme, `https://` is assumed.
5. Save. The device connects and shows the clock.

The portal closes after 3 minutes without input. The device then sleeps for a minute and tries again. Saving only the user name or server URL closes the portal straight away.

### Changing the settings later

Press the `KEY1` button twice, with the second press within 3 seconds of the first. The first press wakes the device; the second makes it show the "WiFi Setup Mode" screen and open the setup portal again, with the current values filled in.

A single press only wakes the device, which then refreshes the clock as usual. This is useful for waking it before uploading new firmware.

### Refreshing on demand

Press the `KEY2` button once. The device wakes, downloads the current image and shows it straight away, without waiting for the minute to end. Use it after changing your settings on the server. The device then goes back to its normal rhythm and refreshes again when the minute changes.

The portal also opens by itself if Wi-Fi is connected but no user name has been set.

## Wi-Fi

### Several networks

The device remembers the last 5 Wi-Fi networks it connected to. Taking it to another place and back works without setting it up again: on each wake it tries the network it used last, and if that fails, any other remembered network that is in range. Each attempt waits up to 10 seconds.

### When Wi-Fi is lost

If no remembered network can be reached, the device shows the "WiFi Setup Mode" screen and opens the setup portal, as on first setup. While the portal is open it keeps checking every 30 seconds whether a remembered network is back in range, for example after a router restart, and if so closes the portal and shows the clock again.

The portal closes after 3 minutes without use. The device then sleeps for a minute and starts over, so it keeps trying for as long as the outage lasts. Note that this cycle keeps the radio on most of the time, which drains the battery much faster than normal operation.

### When there is no internet

If Wi-Fi is connected but the clock image can't be downloaded, the device draws the time itself, with the reason underneath ("No internet", "Server error (HTTP 500)" or "Invalid image from server"), and tries again every minute.

The device's own time comes from the last successful time sync. It is lost on a reset, in which case only the message is shown, and it drifts slowly during a long outage, since the board keeps time with its internal oscillator. The time zone defaults to Israel; a server can change it with an `X-Clock-Timezone` response header in POSIX TZ format (for example `IST-2IDT,M3.4.4/26,M10.5.0`).

## How it works

On each wake the firmware:

1. Loads the user name and server URL from flash (NVS).
2. Reads the battery voltage.
3. Connects to one of the remembered Wi-Fi networks, or opens the setup portal (see [Wi-Fi](#wi-fi)).
4. Downloads the clock image, reporting the battery status in the request (see below).
5. Decodes the PNG, converts each pixel to black or white and draws it with a full display refresh. If the image can't be downloaded, it draws the time itself instead (see [When there is no internet](#when-there-is-no-internet)).
6. Confirms the running firmware, and installs a firmware update if the server offered one (see [Firmware updates](#firmware-updates)).
7. Syncs the time over NTP and sleeps until about 2 seconds before the next minute, so the next image is on screen as the minute changes. If NTP is unavailable it sleeps for 60 seconds.

## Image request and battery status

The image is requested from:

```
<server>/clock.png?user=<user name>&fw=<version>&battery_mv=<millivolts>&charging=<0|1>
```

- `user`: the user name set in the setup portal.
- `fw`: the firmware version, for example `1.5.0`, or `0.0.0-dev` for a local build.
- `battery_mv`: the battery voltage in millivolts, for example `4063`. It is measured once per wake, before Wi-Fi starts. The device sends the voltage only; the server turns it into a percentage.
- `charging`: `1` when the device believes USB power is connected, `0` when it believes it is not.

Both battery parameters are optional. If the reading fails or is outside 2500 to 4500 mV, neither is sent. `charging` is left out while the device does not know the state.

The board has no charge-status pin, so `charging` is worked out from the voltage: USB power raises the reading by about 50 mV. A rise of more than 30 mV since the previous wake sets it to `1`, and a drop of more than 30 mV sets it to `0`. After power-on or a reset the state is unknown until the first such step, which means plugging or unplugging USB once.

Whether and how the battery is shown on the clock is chosen on the server's settings page.

## Server requirements

Any server can be used as long as `/clock.png` returns:

- a PNG no wider than 800 pixels (800x480 fills the display)
- a `Content-Length` header, since the image is read into memory in one piece

Both `http://` and `https://` are supported. HTTPS certificates are not verified.

## Firmware updates

The device updates itself over the air from releases published on GitHub.

### Versioning

Versions have the form `x.y.z`. The release workflow sets the version from the git tag at build time (`-DFW_VERSION="x.y.z"`); a build without it is `0.0.0-dev`. The server only offers updates to devices reporting a plain `x.y.z` version, and the device itself also refuses updates while running a development build, so boards you are developing on are left alone.

### Releasing a version

```
git tag v1.5.0
git push origin v1.5.0
```

Pushing a tag of the form `vX.Y.Z` runs [.github/workflows/release.yml](.github/workflows/release.yml), which:

1. builds the firmware with PlatformIO, with the version taken from the tag
2. signs `firmware.bin` with the private key in the `FIRMWARE_SIGNING_KEY` secret (ECDSA P-256, DER signature of the SHA-256), after checking that the key matches the public key in [include/firmware_signing_key.h](include/firmware_signing_key.h)
3. creates a GitHub release for the tag with two assets, `firmware.bin` and `firmware.bin.sig`

### How a device installs an update

When a newer version exists, the server adds `X-Firmware-*` headers to the image response. The device:

1. shows the clock image first, so a slow download never delays the minute
2. installs only when it is on USB power (`charging=1`) or the battery is at least 3700 mV
3. streams the download straight into the inactive firmware slot, computing its SHA-256 on the way
4. checks the size, the SHA-256 and the signature against the public key compiled into the firmware, and activates the new firmware only if all three pass
5. restarts into the new version

The server is not trusted: it never holds the private key, and a download that fails any check is discarded while the current firmware keeps running. After three failed attempts at the same version the device stops trying it, until it is next reset.

### Rollback

A new firmware must prove that it works: on its first start it has to fetch and display a clock image. Only then does it mark itself as good. If it does not, the next wake (every wake from deep sleep is a restart) goes back to the previous firmware.

This means that a Wi-Fi or server outage right after an update also causes a fallback to the previous version. That is harmless: the device reports the old version again, and the server simply offers the update again later.

### Signing key

The key pair is created once, by the maintainer:

```
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out firmware_signing_private.pem
openssl pkey -in firmware_signing_private.pem -pubout -out firmware_signing_public.pem
```

- The contents of `firmware_signing_private.pem` go into the GitHub Actions secret `FIRMWARE_SIGNING_KEY`. The private key must never be committed (`*.pem` is in `.gitignore`).
- The contents of `firmware_signing_public.pem` go into [include/firmware_signing_key.h](include/firmware_signing_key.h). Devices only accept firmware signed by that key, so changing it later means flashing every device by cable again.

## Libraries

- [GxEPD2](https://github.com/ZinggJM/GxEPD2): e-paper display driver
- [PNGdec](https://github.com/bitbank2/PNGdec): PNG decoder
- [WiFiManager](https://github.com/tzapu/WiFiManager): Wi-Fi and settings portal

## License

Released under the [MIT License](LICENSE). The libraries listed above have their own licenses.
