#pragma once

#include "settings.h"

#ifdef DLNA_ENABLE

#include <Arduino.h>

// MediaServer types for communication
typedef struct {
	String name;
	String location;
	String usn;
	bool available;
	uint8_t serverId;
} MediaServerInfo_t;

// Cyclic task for MediaServer discovery and handling
void MediaServer_Cyclic(void);

// Check if MediaServer is connected
bool MediaServer_IsConnected(void);

// Get current MediaServer info
MediaServerInfo_t MediaServer_GetInfo(void);

// Browse MediaServer directory (non-blocking, results via WebSocket)
bool MediaServer_Browse(uint8_t serverId, const char *objectId, const char *nodeId);

// Browse MediaServer directory and create M3U playlist (synchronous, for Web.cpp)
// Returns vector of URLs, caller is responsible for memory management
std::vector<String> MediaServer_GetDirectoryUrls(const char *objectId);

// Request MediaServer playlist creation (async, for RFID/AudioPlayer)
// Creates /mediaserver_temp.m3u and triggers callback when ready
// Returns false if request cannot be queued
bool MediaServer_RequestPlaylist(const char *objectId, uint32_t playMode, uint32_t trackLastPlayed);

// Trigger MediaServer discovery (non-blocking)
void MediaServer_TriggerDiscovery(void);

#endif // DLNA_ENABLE
