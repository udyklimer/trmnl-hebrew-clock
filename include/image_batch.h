#pragma once

#include <Arduino.h>
#include <time.h>

// Battery save mode stores the clock images for the coming minutes in flash
// (LittleFS, /batch/<unix time of the minute>.png), so most wakes can show
// the next minute without Wi-Fi. What is stored is also kept in RTC memory.
// Nothing touches the filesystem unless a batch is actually stored.

// True if a stored image may exist for this minute (start of minute, Unix time)
bool batchCovers(time_t minute);

// True if a batch is stored, even if its minutes have passed
bool batchExists();

// Delete all stored images and forget the batch
void batchDiscard();

// Start a new batch: delete earlier files. Returns false if the filesystem
// can't be used, in which case nothing should be saved.
bool batchStart();

// Save the image for one minute; returns false if writing failed
bool batchSave(time_t minute, const uint8_t* data, size_t len);

// Load the image for one minute into a malloc'd buffer (caller frees),
// or return nullptr if there is none
uint8_t* batchLoad(time_t minute, size_t* len);

// Delete the image for one minute once it has been shown
void batchRemove(time_t minute);
