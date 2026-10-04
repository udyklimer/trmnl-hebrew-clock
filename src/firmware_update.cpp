#include "firmware_update.h"
#include "firmware_signing_key.h"

#include <Update.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

#define OTA_MAX_ATTEMPTS 3            // Give up on a version after this many failed installs
#define OTA_STALL_TIMEOUT_MS 15000    // Abort if the download stops sending data this long
#define OTA_MAX_SIGNATURE_LEN 80      // A DER ECDSA P-256 signature is at most 72 bytes

// Failed installs, kept across deep sleep so a bad offer is not retried forever
RTC_DATA_ATTR char failedVersion[16] = "";
RTC_DATA_ATTR uint8_t failedAttempts = 0;

// The Arduino core confirms every new firmware at startup unless this returns
// true. Confirm it ourselves only once an image has been fetched and displayed,
// so a firmware that cannot do that is rolled back on the next wake.
extern "C" bool verifyRollbackLater() {
  return true;
}

// mbedTLS 2.x (Arduino core 2.x) names its SHA-256 calls with a _ret suffix
#if MBEDTLS_VERSION_MAJOR < 3
#define sha256_starts mbedtls_sha256_starts_ret
#define sha256_update mbedtls_sha256_update_ret
#define sha256_finish mbedtls_sha256_finish_ret
#else
#define sha256_starts mbedtls_sha256_starts
#define sha256_update mbedtls_sha256_update
#define sha256_finish mbedtls_sha256_finish
#endif

static const char* OFFER_HEADERS[] = {
  "X-Firmware-Version", "X-Firmware-Url", "X-Firmware-Size",
  "X-Firmware-Sha256", "X-Firmware-Signature"
};

void collectFirmwareHeaders(HTTPClient& http, const char* const extraHeaders[], size_t extraCount) {
  // collectHeaders() replaces any earlier list, so the caller's headers go in the same call
  const size_t offerCount = sizeof(OFFER_HEADERS) / sizeof(OFFER_HEADERS[0]);
  const char* headers[offerCount + 4];
  size_t count = 0;
  for (size_t i = 0; i < offerCount; i++) headers[count++] = OFFER_HEADERS[i];
  for (size_t i = 0; i < extraCount && count < sizeof(headers) / sizeof(headers[0]); i++) headers[count++] = extraHeaders[i];
  http.collectHeaders(headers, count);
}

FirmwareOffer readFirmwareOffer(HTTPClient& http) {
  FirmwareOffer offer;
  offer.version = http.header("X-Firmware-Version");
  offer.url = http.header("X-Firmware-Url");
  offer.size = (size_t)http.header("X-Firmware-Size").toInt();
  offer.sha256 = http.header("X-Firmware-Sha256");
  offer.signature = http.header("X-Firmware-Signature");
  offer.version.trim();
  offer.url.trim();
  offer.sha256.trim();
  offer.signature.trim();
  offer.present = offer.version.length() > 0;
  return offer;
}

void confirmRunningFirmware() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
      Serial.printf("OTA: firmware %s confirmed, rollback cancelled\n", FW_VERSION);
    } else {
      Serial.println("OTA: failed to confirm the running firmware");
    }
  }
}

// Parse a plain x.y.z version; anything else (e.g. 0.0.0-dev) is rejected
static bool parseVersion(const String& v, long out[3]) {
  int part = 0;
  int digits = 0;
  out[0] = out[1] = out[2] = 0;
  for (size_t i = 0; i < v.length(); i++) {
    char c = v[i];
    if (c >= '0' && c <= '9') {
      if (++digits > 6) return false;
      out[part] = out[part] * 10 + (c - '0');
    } else if (c == '.' && digits > 0 && part < 2) {
      part++;
      digits = 0;
    } else {
      return false;
    }
  }
  return part == 2 && digits > 0;
}

static bool isNewerVersion(const String& offered, const String& current) {
  long a[3], b[3];
  if (!parseVersion(offered, a) || !parseVersion(current, b)) return false;
  for (int i = 0; i < 3; i++) {
    if (a[i] != b[i]) return a[i] > b[i];
  }
  return false;
}

static void recordFailure(const String& version) {
  if (version != failedVersion) {
    strlcpy(failedVersion, version.c_str(), sizeof(failedVersion));
    failedAttempts = 0;
  }
  failedAttempts++;
  Serial.printf("OTA: install of %s failed (attempt %u of %u)\n",
                failedVersion, failedAttempts, OTA_MAX_ATTEMPTS);
}

// Check the signature of a SHA-256 digest against the compiled-in public key
static bool verifySignature(const uint8_t hash[32], const String& signatureBase64) {
  uint8_t signature[OTA_MAX_SIGNATURE_LEN];
  size_t signatureLen = 0;
  if (mbedtls_base64_decode(signature, sizeof(signature), &signatureLen,
                            (const unsigned char*)signatureBase64.c_str(), signatureBase64.length()) != 0) {
    Serial.println("OTA: signature is not valid base64");
    return false;
  }

  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  bool ok = false;
  if (mbedtls_pk_parse_public_key(&pk, (const unsigned char*)FIRMWARE_SIGNING_PUBLIC_KEY,
                                  sizeof(FIRMWARE_SIGNING_PUBLIC_KEY)) != 0) {
    Serial.println("OTA: no valid signing key compiled in, refusing all updates");
  } else if (!mbedtls_pk_can_do(&pk, MBEDTLS_PK_ECDSA) || mbedtls_pk_get_bitlen(&pk) != 256) {
    Serial.println("OTA: compiled-in signing key is not ECDSA P-256");
  } else if (mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, 32, signature, signatureLen) != 0) {
    Serial.println("OTA: signature check failed");
  } else {
    ok = true;
  }
  mbedtls_pk_free(&pk);
  return ok;
}

// Stream the firmware into the inactive OTA slot while hashing it.
// Leaves the update open (not yet activated) and fills in hash on success.
static bool downloadFirmware(const String& url, size_t expectedSize, uint8_t hash[32]) {
  HTTPClient http;
  http.setTimeout(OTA_STALL_TIMEOUT_MS);
  WiFiClient plainClient;
  WiFiClientSecure secureClient;
  bool begun;
  if (url.startsWith("https://")) {
    secureClient.setInsecure();  // Integrity comes from the signature, not TLS
    begun = http.begin(secureClient, url);
  } else {
    begun = http.begin(plainClient, url);
  }
  if (!begun) {
    Serial.println("OTA: HTTP begin failed");
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("OTA: download failed, HTTP %d\n", httpCode);
    http.end();
    return false;
  }
  int contentLength = http.getSize();
  if (contentLength != (int)expectedSize) {
    Serial.printf("OTA: size mismatch, header says %u, download is %d\n", (unsigned)expectedSize, contentLength);
    http.end();
    return false;
  }
  if (!Update.begin(expectedSize)) {
    Serial.printf("OTA: cannot start update: %s\n", Update.errorString());
    http.end();
    return false;
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  sha256_starts(&sha, 0);

  static uint8_t buffer[4096];
  WiFiClient* stream = http.getStreamPtr();
  size_t received = 0;
  unsigned long lastData = millis();
  bool ok = true;
  while (received < expectedSize) {
    size_t available = stream->available();
    if (available == 0) {
      if (!stream->connected() || millis() - lastData > OTA_STALL_TIMEOUT_MS) {
        Serial.printf("OTA: download stopped after %u of %u bytes\n", (unsigned)received, (unsigned)expectedSize);
        ok = false;
        break;
      }
      delay(1);
      continue;
    }
    size_t chunk = min(min(available, sizeof(buffer)), expectedSize - received);
    size_t n = stream->readBytes(buffer, chunk);
    if (n == 0) continue;
    sha256_update(&sha, buffer, n);
    if (Update.write(buffer, n) != n) {
      Serial.printf("OTA: flash write failed: %s\n", Update.errorString());
      ok = false;
      break;
    }
    received += n;
    lastData = millis();
  }

  sha256_finish(&sha, hash);
  mbedtls_sha256_free(&sha);
  http.end();
  return ok && received == expectedSize;
}

void installFirmwareUpdate(const FirmwareOffer& offer, const String& serverUrl) {
  if (!offer.present) return;

  long ignored[3];
  if (!parseVersion(FW_VERSION, ignored)) {
    Serial.printf("OTA: development build %s, ignoring offer of %s\n", FW_VERSION, offer.version.c_str());
    return;
  }
  if (!isNewerVersion(offer.version, FW_VERSION)) {
    Serial.printf("OTA: offered %s is not newer than %s, ignoring\n", offer.version.c_str(), FW_VERSION);
    return;
  }
  if (offer.version == failedVersion && failedAttempts >= OTA_MAX_ATTEMPTS) {
    Serial.printf("OTA: %s already failed %u times, not trying again\n", failedVersion, failedAttempts);
    return;
  }
  if (!offer.url.startsWith("/") || offer.size == 0 || offer.sha256.length() != 64 || offer.signature.length() == 0) {
    Serial.println("OTA: incomplete or malformed offer headers, ignoring");
    return;
  }

  String url = serverUrl + offer.url;
  Serial.printf("OTA: installing %s (%u bytes) from %s\n", offer.version.c_str(), (unsigned)offer.size, url.c_str());

  uint8_t hash[32];
  bool ok = downloadFirmware(url, offer.size, hash);

  if (ok) {
    char hashHex[65];
    for (int i = 0; i < 32; i++) {
      snprintf(hashHex + i * 2, 3, "%02x", hash[i]);
    }
    if (!offer.sha256.equalsIgnoreCase(hashHex)) {
      Serial.printf("OTA: SHA-256 mismatch, got %s\n", hashHex);
      ok = false;
    }
  }
  // The signature covers the SHA-256 of the downloaded bytes, so it is checked
  // against the hash computed here, never the one in the header
  if (ok) {
    ok = verifySignature(hash, offer.signature);
  }

  if (ok && Update.end(true)) {
    Serial.printf("OTA: %s verified and installed, restarting\n", offer.version.c_str());
    delay(100);
    ESP.restart();
  }

  if (ok) {
    Serial.printf("OTA: activating the update failed: %s\n", Update.errorString());
  }
  Update.abort();
  recordFailure(offer.version);
}
