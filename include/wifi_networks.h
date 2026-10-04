#pragma once

#include <Arduino.h>

// The ESP32 itself remembers only one Wi-Fi network, so the clock keeps its
// own list of the last few networks it connected to, most recent first.
#define MAX_SAVED_NETWORKS 5

// Import the single network saved by firmware from before the list existed
void migrateStoredNetwork();

int savedNetworkCount();

// Connect to a saved network: the most recent one first, then any other saved
// network that is in range. Each attempt waits up to timeoutMs.
bool connectToSavedNetwork(uint32_t timeoutMs);

// Scan, and try only the saved networks that are in range. Used while the
// setup portal is open, to reconnect as soon as a lost network comes back.
bool connectToSavedNetworkInRange(uint32_t timeoutMs);

// Store the network we are connected to as the most recent one
void rememberConnectedNetwork();
