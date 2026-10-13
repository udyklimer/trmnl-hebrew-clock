#include "image_batch.h"

#include <LittleFS.h>

#define BATCH_DIR "/batch"

// Range of minutes stored in flash, kept across deep sleep (0 = no batch)
RTC_DATA_ATTR time_t batchFirstMinute = 0;
RTC_DATA_ATTR time_t batchLastMinute = 0;

static bool mounted = false;

// Mount LittleFS on the "spiffs" data partition, formatting it the first
// time it is used
static bool mount() {
  if (!mounted) {
    mounted = LittleFS.begin(true);
    if (!mounted) Serial.println("Batch: LittleFS mount failed");
  }
  return mounted;
}

static String pathFor(time_t minute) {
  return String(BATCH_DIR "/") + String((unsigned long)minute) + ".png";
}

static void removeAllFiles() {
  File dir = LittleFS.open(BATCH_DIR);
  if (!dir || !dir.isDirectory()) return;
  File f = dir.openNextFile();
  while (f) {
    String path = f.path();
    f.close();
    LittleFS.remove(path);
    f = dir.openNextFile();
  }
}

bool batchCovers(time_t minute) {
  return batchFirstMinute != 0 && minute >= batchFirstMinute && minute <= batchLastMinute;
}

bool batchExists() {
  return batchFirstMinute != 0;
}

void batchDiscard() {
  batchFirstMinute = batchLastMinute = 0;
  if (mount()) removeAllFiles();
}

bool batchStart() {
  batchFirstMinute = batchLastMinute = 0;
  if (!mount()) return false;
  removeAllFiles();
  LittleFS.mkdir(BATCH_DIR);
  return true;
}

bool batchSave(time_t minute, const uint8_t* data, size_t len) {
  if (!mount()) return false;
  File f = LittleFS.open(pathFor(minute), FILE_WRITE);
  if (!f) return false;
  size_t written = f.write(data, len);
  f.close();
  if (written != len) {
    LittleFS.remove(pathFor(minute));
    return false;
  }
  if (batchFirstMinute == 0 || minute < batchFirstMinute) batchFirstMinute = minute;
  if (minute > batchLastMinute) batchLastMinute = minute;
  return true;
}

uint8_t* batchLoad(time_t minute, size_t* len) {
  if (!mount()) return nullptr;
  File f = LittleFS.open(pathFor(minute), FILE_READ);
  if (!f) return nullptr;
  size_t size = f.size();
  uint8_t* data = size > 0 ? (uint8_t*)malloc(size) : nullptr;
  if (data && f.read(data, size) != size) {
    free(data);
    data = nullptr;
  }
  f.close();
  *len = size;
  return data;
}

void batchRemove(time_t minute) {
  if (mount()) LittleFS.remove(pathFor(minute));
}
