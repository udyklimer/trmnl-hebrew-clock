#include "wifi_networks.h"

#include <Preferences.h>
#include <WiFi.h>
#include <esp_wifi.h>

#define NETWORKS_NAMESPACE "wifi_nets"

struct SavedNetwork {
  String ssid;
  String pass;
};

static int loadNetworks(SavedNetwork nets[MAX_SAVED_NETWORKS]) {
  Preferences p;
  p.begin(NETWORKS_NAMESPACE, false);
  int count = min((int)p.getUChar("count", 0), MAX_SAVED_NETWORKS);
  for (int i = 0; i < count; i++) {
    nets[i].ssid = p.getString(("s" + String(i)).c_str(), "");
    nets[i].pass = p.getString(("p" + String(i)).c_str(), "");
  }
  p.end();
  return count;
}

static void saveNetworks(const SavedNetwork nets[], int count) {
  Preferences p;
  p.begin(NETWORKS_NAMESPACE, false);
  p.putUChar("count", count);
  for (int i = 0; i < count; i++) {
    p.putString(("s" + String(i)).c_str(), nets[i].ssid);
    p.putString(("p" + String(i)).c_str(), nets[i].pass);
  }
  p.end();
}

// Put a network at the front of the list, dropping the oldest if it is full.
// Writes to flash only if the list actually changes.
static void addNetwork(const String& ssid, const String& pass) {
  if (ssid.length() == 0) return;
  SavedNetwork nets[MAX_SAVED_NETWORKS];
  int count = loadNetworks(nets);
  if (count > 0 && nets[0].ssid == ssid && nets[0].pass == pass) return;

  SavedNetwork updated[MAX_SAVED_NETWORKS];
  updated[0] = {ssid, pass};
  int n = 1;
  for (int i = 0; i < count && n < MAX_SAVED_NETWORKS; i++) {
    if (nets[i].ssid != ssid) updated[n++] = nets[i];
  }
  saveNetworks(updated, n);
  Serial.printf("WiFi: saved network '%s' (%d remembered)\n", ssid.c_str(), n);
}

void migrateStoredNetwork() {
  SavedNetwork nets[MAX_SAVED_NETWORKS];
  if (loadNetworks(nets) > 0) return;

  WiFi.mode(WIFI_STA);
  wifi_config_t conf;
  if (esp_wifi_get_config(WIFI_IF_STA, &conf) != ESP_OK || conf.sta.ssid[0] == 0) return;
  String ssid = String((const char*)conf.sta.ssid).substring(0, sizeof(conf.sta.ssid));
  String pass = String((const char*)conf.sta.password).substring(0, sizeof(conf.sta.password));
  Serial.printf("WiFi: importing previously saved network '%s'\n", ssid.c_str());
  addNetwork(ssid, pass);
}

int savedNetworkCount() {
  SavedNetwork nets[MAX_SAVED_NETWORKS];
  return loadNetworks(nets);
}

static bool connectTo(const SavedNetwork& net, uint32_t timeoutMs) {
  Serial.printf("WiFi: connecting to '%s'...\n", net.ssid.c_str());
  WiFi.persistent(false);  // Our own list holds the credentials; don't rewrite them every wake
  WiFi.begin(net.ssid.c_str(), net.pass.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(50);
  }
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.disconnect();
  return false;
}

static bool isInRange(const String& ssid, int found) {
  for (int i = 0; i < found; i++) {
    if (WiFi.SSID(i) == ssid) return true;
  }
  return false;
}

// Try the saved networks from index `from` that show up in a scan
static bool connectInRange(const SavedNetwork nets[], int count, int from, uint32_t timeoutMs) {
  int found = WiFi.scanNetworks();
  bool connected = false;
  for (int i = from; i < count && !connected; i++) {
    if (isInRange(nets[i].ssid, found)) {
      connected = connectTo(nets[i], timeoutMs);
    }
  }
  WiFi.scanDelete();
  return connected;
}

bool connectToSavedNetwork(uint32_t timeoutMs) {
  SavedNetwork nets[MAX_SAVED_NETWORKS];
  int count = loadNetworks(nets);
  if (count == 0) return false;

  WiFi.mode(WIFI_STA);
  // The most recent network is tried without a scan, which keeps normal wakes short
  if (connectTo(nets[0], timeoutMs)) return true;
  return count > 1 && connectInRange(nets, count, 1, timeoutMs);
}

bool connectToSavedNetworkInRange(uint32_t timeoutMs) {
  SavedNetwork nets[MAX_SAVED_NETWORKS];
  int count = loadNetworks(nets);
  return count > 0 && connectInRange(nets, count, 0, timeoutMs);
}

void rememberConnectedNetwork() {
  if (WiFi.status() == WL_CONNECTED) {
    addNetwork(WiFi.SSID(), WiFi.psk());
  }
}
