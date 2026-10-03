#pragma once

#include <Arduino.h>
#include <HTTPClient.h>

// Firmware version, set by the release build (-DFW_VERSION="x.y.z").
// Local builds are "0.0.0-dev", which never takes part in updates.
#ifndef FW_VERSION
#define FW_VERSION "0.0.0-dev"
#endif

// Update offered by the server in the headers of the image response
struct FirmwareOffer {
  bool present = false;
  String version;
  String url;        // path on the clock server, e.g. /firmware/1.5.0.bin
  size_t size = 0;
  String sha256;     // hex
  String signature;  // base64 of a DER ECDSA P-256 / SHA-256 signature
};

// Call on the image request before GET() so the offer headers are kept
void collectFirmwareHeaders(HTTPClient& http);

// Read the offer headers after GET(); present is false if there is none
FirmwareOffer readFirmwareOffer(HTTPClient& http);

// Confirm the running firmware so the bootloader does not roll it back.
// Call once an image has been fetched and displayed.
void confirmRunningFirmware();

// Download, verify and install the offered firmware, then restart into it.
// Returns only if nothing was installed (not allowed, not newer, or failed).
void installFirmwareUpdate(const FirmwareOffer& offer, const String& serverUrl);
