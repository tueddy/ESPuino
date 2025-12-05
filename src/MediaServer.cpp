#include "MediaServer.h"

#ifdef DLNA_ENABLE

	#include "AudioPlayer.h"
	#include "Log.h"
	#include "MemX.h"
	#include "SdCard.h"
	#include "System.h"
	#include "Web.h"
	#include "Wlan.h"

	#include <ArduinoJson.h>
	#include <SoapESP32.h>
	#include <WiFi.h>

// PSRAM allocator for JSON (if PSRAM available)
struct SpiRamAllocator : ArduinoJson::Allocator {
	void *allocate(size_t size) override {
		return ps_malloc(size);
	}
	void deallocate(void *pointer) override {
		free(pointer);
	}
	void *reallocate(void *ptr, size_t new_size) override {
		return ps_realloc(ptr, new_size);
	}
};

// WiFi clients for DLNA/UPnP
static WiFiClient wifiClient;
static WiFiUDP wifiUdp;

// DLNA/UPnP client instance (for task operations)
static SoapESP32 soap(&wifiClient, &wifiUdp);

// Current MediaServer information
static MediaServerInfo_t currentMediaServer = {
	.name = "",
	.location = "",
	.usn = "",
	.available = false,
	.serverId = 0};

// Discovery state
static bool discoveryStarted = false;
static bool serverFound = false;
static unsigned long lastDiscoveryAttempt = 0;
static const unsigned long DISCOVERY_INTERVAL = 1000; // Retry discovery every 1 second if no server found
static const unsigned int DISCOVERY_SCAN_DURATION = 8; // Scan for 8 seconds (sufficient for most networks)

// Browse request queue
struct BrowseRequest {
	uint8_t serverId;
	String objectId;
	String nodeId;
	bool pending;
};
static BrowseRequest pendingBrowseRequest = {0, "", "", false};

// Playlist request queue (for async M3U creation)
struct PlaylistRequest {
	String objectId;
	uint32_t playMode;
	uint32_t trackLastPlayed;
	bool pending;
};
static PlaylistRequest pendingPlaylistRequest = {"", 0, 0, false};

// FreeRTOS task handle
static TaskHandle_t mediaServerTaskHandle = NULL;
static bool initialized = false;

// Forward declarations
static void mediaServerTask(void *parameter);
static void discoverMediaServers();
static void processBrowseRequest(uint8_t serverId, const String &objectId, const String &nodeId);
static void processPlaylistRequest(const String &objectId, uint32_t playMode, uint32_t trackLastPlayed);
static void MediaServer_Init(void);

// Initialize MediaServer support (called automatically on first Cyclic call)
static void MediaServer_Init(void) {

	discoveryStarted = false;
	serverFound = false;
	lastDiscoveryAttempt = 0;
	pendingBrowseRequest.pending = false;
	pendingPlaylistRequest.pending = false;
}

// Cyclic task for MediaServer handling (called from main loop)
void MediaServer_Cyclic(void) {
	// Initialize on first call
	if (!initialized) {
		MediaServer_Init();
		initialized = true;
	}
	
	if (!Wlan_IsConnected()) {
		// WiFi disconnected - stop task if running
		if (mediaServerTaskHandle != NULL) {
			Log_Println("WiFi disconnected, stopping MediaServer task", LOGLEVEL_INFO);
			vTaskDelete(mediaServerTaskHandle);
			mediaServerTaskHandle = NULL;
			discoveryStarted = false;
			serverFound = false;
			currentMediaServer.available = false;
		}
	} else {
		// WiFi connected - start task if not running
		if (mediaServerTaskHandle == NULL) {
			Log_Println("WiFi connected, starting MediaServer task", LOGLEVEL_NOTICE);
			xTaskCreatePinnedToCore(
				mediaServerTask, // Task function
				"MediaServerTask", // Task name
				8192, // tack size (bytes) - increased for network operations
				NULL, // Parameters
				1, // Priority
				&mediaServerTaskHandle, // Task handle
				0 // Core 0 (main loop is on core 1)
			);
		}
	}
}

// Check if MediaServer is connected
bool MediaServer_IsConnected(void) {
	return currentMediaServer.available;
}

// Get current MediaServer info
MediaServerInfo_t MediaServer_GetInfo(void) {
	return currentMediaServer;
}

// Browse MediaServer directory (non-blocking, results via WebSocket)
bool MediaServer_Browse(uint8_t serverId, const char *objectId, const char *nodeId) {
	if (!serverFound) {
		Log_Println("Cannot browse: Discovery not yet complete", LOGLEVEL_ERROR);
		return false;
	}

	if (!currentMediaServer.available) {
		Log_Println("Cannot browse: No MediaServer available", LOGLEVEL_ERROR);
		return false;
	}

	// Queue browse request for task
	if (pendingBrowseRequest.pending) {
		Log_Println("Browse already in progress", LOGLEVEL_NOTICE);
		return false;
	}

	pendingBrowseRequest.serverId = serverId;
	pendingBrowseRequest.objectId = String(objectId);
	pendingBrowseRequest.nodeId = String(nodeId);
	pendingBrowseRequest.pending = true;

	Log_Printf(LOGLEVEL_INFO, "Queued browse request: serverId=%d, objectId=%s, nodeId=%s", serverId, objectId, nodeId);
	return true;
}

// Browse MediaServer directory synchronously (blocking, for playlist creation) - REMOVED
bool MediaServer_BrowseSync(const char *serverUsn, const char *objectId, std::vector<String> &urls) {
	if (!serverFound) {
		Log_Println("Cannot browse sync: Discovery not yet complete", LOGLEVEL_ERROR);
		return false;
	}

	if (!currentMediaServer.available) {
		Log_Println("Cannot browse sync: No MediaServer available", LOGLEVEL_ERROR);
		return false;
	}

	// Validate that current server matches requested server USN
	if (currentMediaServer.usn != String(serverUsn)) {
		Log_Printf(LOGLEVEL_ERROR, "MediaServer USN mismatch! Expected: %s, Current: %s", serverUsn, currentMediaServer.usn.c_str());
		Log_Println("This RFID was assigned to a different MediaServer", LOGLEVEL_ERROR);
		return false;
	}

	uint8_t serverId = currentMediaServer.serverId;
	Log_Printf(LOGLEVEL_INFO, "Synchronous browse: serverUsn=%s, serverId=%d, objectId=%s", serverUsn, serverId, objectId);

	// Browse server - results stored in vector
	soapObjectVect_t browseResult;
	bool success = soap.browseServer(serverId, objectId, &browseResult);

	if (!success || browseResult.size() == 0) {
		Log_Printf(LOGLEVEL_NOTICE, "Browse returned no items for objectId=%s", objectId);
		return false;
	}

	Log_Printf(LOGLEVEL_INFO, "Browse returned %d items", browseResult.size());

	// Extract URLs from audio files only
	for (size_t i = 0; i < browseResult.size(); i++) {
		const soapObject_t &item = browseResult[i];

		// Only add audio files (not directories)
		if (!item.isDirectory && item.fileType == fileTypeAudio && item.uri.length() > 0) {
			// Build complete URL from downloadIp, downloadPort and uri
			String uri = item.uri;
			if (!uri.startsWith("/")) {
				uri = "/" + uri;
			}
			String fileUrl = "http://" + item.downloadIp.toString() + ":" + String(item.downloadPort) + uri;
			urls.push_back(fileUrl);
			Log_Printf(LOGLEVEL_DEBUG, "Added to playlist: %s", fileUrl.c_str());
		}
	}

	Log_Printf(LOGLEVEL_INFO, "Added %d audio files to playlist", urls.size());
	return urls.size() > 0;
}

/*
// Download MediaServer file to SD card (non-blocking) - REMOVED (unreliable)
bool MediaServer_DownloadToSD(uint8_t serverId, const char *objectId, const char *filename) {
	if (!serverFound) {
		Log_Println("Cannot download: Discovery not yet complete", LOGLEVEL_ERROR);
		return false;
	}

	if (!currentMediaServer.available) {
		Log_Println("Cannot download: No MediaServer available", LOGLEVEL_ERROR);
		return false;
	}

	// Queue download request for task
	if (xSemaphoreTake(downloadMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
		if (pendingDownloadRequest.pending) {
			xSemaphoreGive(downloadMutex);
			Log_Println("Download already in progress", LOGLEVEL_NOTICE);
			return false;
		}

		pendingDownloadRequest.serverId = serverId;
		pendingDownloadRequest.objectId = String(objectId);
		pendingDownloadRequest.filename = String(filename);
		pendingDownloadRequest.pending = true;
		xSemaphoreGive(downloadMutex);

		Log_Printf(LOGLEVEL_INFO, "Queued download request: serverId=%d, objectId=%s, filename=%s",
			serverId, objectId, filename);
		return true;
	}

	Log_Println("Failed to queue download request (mutex timeout)", LOGLEVEL_ERROR);
	return false;
}
*/

// FreeRTOS task for DLNA operations
static void mediaServerTask(void *parameter) {
	Log_Println("MediaServer task started", LOGLEVEL_NOTICE);

	while (true) {
		// Check WiFi and perform discovery
		if (Wlan_IsConnected()) {
			// Only run discovery if no server found yet, or if retry interval has passed
			bool shouldDiscover = false;
			if (!serverFound) {
				// No server found yet - start immediately on first attempt
				if (!discoveryStarted) {
					shouldDiscover = true;
				} else if (millis() - lastDiscoveryAttempt > DISCOVERY_INTERVAL) {
					// Retry after interval if still no server
					shouldDiscover = true;
				}
			}

			if (shouldDiscover) {
				discoverMediaServers();
				lastDiscoveryAttempt = millis();
			}

			// Process pending browse requests
			if (pendingBrowseRequest.pending) {
				uint8_t serverId = pendingBrowseRequest.serverId;
				String objectId = pendingBrowseRequest.objectId;
				String nodeId = pendingBrowseRequest.nodeId;
				pendingBrowseRequest.pending = false;

				processBrowseRequest(serverId, objectId, nodeId);
			}

			// Process pending playlist requests
			if (pendingPlaylistRequest.pending) {
				String objectId = pendingPlaylistRequest.objectId;
				uint32_t playMode = pendingPlaylistRequest.playMode;
				uint32_t trackLastPlayed = pendingPlaylistRequest.trackLastPlayed;
				pendingPlaylistRequest.pending = false;

				processPlaylistRequest(objectId, playMode, trackLastPlayed);
			}
		}

		// Shorter delay when discovery is active to not miss UDP responses
		if (!serverFound && discoveryStarted) {
			vTaskDelay(pdMS_TO_TICKS(10)); // 10ms during active discovery
		} else {
			vTaskDelay(pdMS_TO_TICKS(100)); // 100ms when idle
		}
	}
}

// Browse MediaServer directory and create playlist (synchronous, for Web.cpp)
// Returns vector of URLs, empty vector on error
std::vector<String> MediaServer_GetDirectoryUrls(const char *objectId) {
	std::vector<String> urls;
	
	if (!serverFound || !currentMediaServer.available) {
		Log_Println("Cannot browse: No MediaServer available", LOGLEVEL_ERROR);
		return urls;
	}

	uint8_t serverId = currentMediaServer.serverId;
	Log_Printf(LOGLEVEL_INFO, "Browsing MediaServer directory: serverId=%d, objectId=%s", serverId, objectId);
	
	// Browse server - results stored in vector
	soapObjectVect_t browseResult;
	bool success = soap.browseServer(serverId, objectId, &browseResult);
	
	if (!success || browseResult.size() == 0) {
		Log_Printf(LOGLEVEL_NOTICE, "Browse returned no items for objectId=%s", objectId);
		return urls;
	}

	Log_Printf(LOGLEVEL_INFO, "Browse returned %d items", browseResult.size());

	// Extract audio file URLs
	for (size_t i = 0; i < browseResult.size(); i++) {
		const soapObject_t &item = browseResult[i];
		
		// Only add audio files (not directories)
		if (!item.isDirectory && item.fileType == fileTypeAudio && item.uri.length() > 0) {
			// Build complete URL from downloadIp, downloadPort and uri
			String uri = item.uri;
			if (!uri.startsWith("/")) {
				uri = "/" + uri;
			}
			String fileUrl = "http://" + item.downloadIp.toString() + ":" + 
						 String(item.downloadPort) + uri;
			
			urls.push_back(fileUrl);
			Log_Printf(LOGLEVEL_DEBUG, "Added to playlist [%d]: %s", urls.size() - 1, fileUrl.c_str());
		}
	}

	Log_Printf(LOGLEVEL_INFO, "Created playlist with %d audio files", urls.size());
	return urls;
}

// Request MediaServer playlist creation (async, for RFID/AudioPlayer)
bool MediaServer_RequestPlaylist(const char *objectId, uint32_t playMode, uint32_t trackLastPlayed) {
	if (!serverFound || !currentMediaServer.available) {
		Log_Println("Cannot request playlist: No MediaServer available", LOGLEVEL_ERROR);
		return false;
	}

	if (pendingPlaylistRequest.pending) {
		Log_Println("Playlist request already in progress", LOGLEVEL_NOTICE);
		return false;
	}

	pendingPlaylistRequest.objectId = String(objectId);
	pendingPlaylistRequest.playMode = playMode;
	pendingPlaylistRequest.trackLastPlayed = trackLastPlayed;
	pendingPlaylistRequest.pending = true;

	Log_Printf(LOGLEVEL_INFO, "Queued playlist request: objectId=%s, playMode=%d, trackLastPlayed=%d", 
		objectId, playMode, trackLastPlayed);
	return true;
}

// Process playlist request (runs in task context)
static void processPlaylistRequest(const String &objectId, uint32_t playMode, uint32_t trackLastPlayed) {
	Log_Printf(LOGLEVEL_INFO, "Processing playlist request: objectId=%s", objectId.c_str());

	// Get URLs from MediaServer
	std::vector<String> urls = MediaServer_GetDirectoryUrls(objectId.c_str());
	
	if (urls.size() == 0) {
		Log_Println("Failed to get MediaServer URLs", LOGLEVEL_ERROR);
		return;
	}

	// Create M3U content
	String playlistContent = "";
	for (size_t i = 0; i < urls.size(); i++) {
		playlistContent += urls[i];
		if (i < urls.size() - 1) {
			playlistContent += "\n";
		}
	}

	// Write to temporary M3U file
	String tempM3uPath = "/.mediaserver_temp.m3u";
	File m3uFile = gFSystem.open(tempM3uPath, FILE_WRITE);
	if (!m3uFile) {
		Log_Println("Failed to create temp M3U file", LOGLEVEL_ERROR);
		return;
	}
	m3uFile.print(playlistContent);
	m3uFile.close();

	Log_Printf(LOGLEVEL_NOTICE, "Created MediaServer M3U playlist with %d tracks", urls.size());

	// Trigger AudioPlayer to start playback
	AudioPlayer_SetPlaylist(tempM3uPath.c_str(), trackLastPlayed, LOCAL_M3U, 0);
}

// Trigger MediaServer discovery (public function)
void MediaServer_TriggerDiscovery(void) {
	Log_Println("Manual MediaServer discovery triggered", LOGLEVEL_NOTICE);
	// Reset discovery state to force new scan
	serverFound = false;
	discoveryStarted = false;
	currentMediaServer.available = false;
	lastDiscoveryAttempt = 0;
}

// Discover media servers on network
static void discoverMediaServers() {
	unsigned long scanStart = millis();
	if (!discoveryStarted) {
		Log_Printf(LOGLEVEL_NOTICE, "Starting MediaServer discovery (scan duration: %d seconds)...", DISCOVERY_SCAN_DURATION);
	} else {
		Log_Println("Retrying MediaServer discovery...", LOGLEVEL_INFO);
	}

	int numServers = soap.seekServer(DISCOVERY_SCAN_DURATION);
	discoveryStarted = true;
	unsigned long scanDuration = millis() - scanStart;

	Log_Printf(LOGLEVEL_DEBUG, "Discovery scan completed in %lu ms", scanDuration);

	if (numServers > 0) {
		Log_Printf(LOGLEVEL_NOTICE, "Found %d media server(s)", numServers);

		// Get first server info (still holding mutex)
		soapServer_t serverInfo;
		if (soap.getServerInfo(0, &serverInfo)) {
			currentMediaServer.name = serverInfo.friendlyName;
			currentMediaServer.location = serverInfo.location;
			currentMediaServer.usn = serverInfo.controlURL;
			currentMediaServer.serverId = 0;
			currentMediaServer.available = true;
			serverFound = true;

			Log_Printf(LOGLEVEL_NOTICE, "MediaServer ready: name=%s, location=%s",
				serverInfo.friendlyName.c_str(), serverInfo.location.c_str());

			// Notify Web UI via WebSocket
			Web_SendWebsocketData(0, WebsocketCode::MediaServerDiscovery);
		}
	} else if (!serverFound) {
		Log_Println("No media servers found", LOGLEVEL_INFO);
	}
}

// Process browse request (runs in task context)
static void processBrowseRequest(uint8_t serverId, const String &objectId, const String &nodeId) {
	unsigned long startTime = millis();
	Log_Printf(LOGLEVEL_INFO, "Processing browse request: serverId=%d, objectId=%s, nodeId=%s",
		serverId, objectId.c_str(), nodeId.c_str());

	#ifdef BOARD_HAS_PSRAM
	SpiRamAllocator allocator;
	JsonDocument doc(&allocator);
	#else
	JsonDocument doc;
	#endif

	JsonArray resultArray = doc.to<JsonArray>();

	// Browse server - results stored in vector
	// This operation can take several seconds, so reset watchdog
	soapObjectVect_t browseResult;
	bool success = soap.browseServer(serverId, objectId.c_str(), &browseResult);

	Log_Printf(LOGLEVEL_DEBUG, "browseServer() returned: success=%d, items=%d", success, browseResult.size());

	if (success && browseResult.size() > 0) {
		Log_Printf(LOGLEVEL_INFO, "Browse returned %d items", browseResult.size());

		// Convert soapObjectVect_t to JSON
		for (size_t i = 0; i < browseResult.size(); i++) {
			const soapObject_t &item = browseResult[i];
			Log_Printf(LOGLEVEL_DEBUG, "Item %d: name=%s, id=%s, isDir=%d", i, item.name.c_str(), item.id.c_str(), item.isDirectory);
			JsonObject entry = resultArray.add<JsonObject>();
			entry["name"] = item.name;
			entry["objectId"] = item.id; // Check if it's a directory
			if (item.isDirectory) {
				entry["dir"] = true;
			} else {
				// It's a file - add URL constructed from downloadIp, downloadPort and uri
				if (item.uri.length() > 0) {
					// Use downloadIp and downloadPort for the actual file URL
					// Ensure uri starts with / for proper URL construction
					String uri = item.uri;
					if (!uri.startsWith("/")) {
						uri = "/" + uri;
					}
					String fileUrl = "http://" + item.downloadIp.toString() + ":" + String(item.downloadPort) + uri;
					entry["url"] = fileUrl;

					// Also store individual components for download
					entry["downloadIp"] = item.downloadIp.toString();
					entry["downloadPort"] = item.downloadPort;
					entry["uri"] = uri;

					Log_Printf(LOGLEVEL_DEBUG, "File URL: %s", fileUrl.c_str());
				}
				// Add fileType: 0=other, 1=audio, 2=image, 3=video
				entry["fileType"] = (int) item.fileType;
			}
		}
	} else {
		Log_Printf(LOGLEVEL_NOTICE, "Browse returned no items for objectId=%s", objectId.c_str());
	}

	// Convert to JSON string
	String jsonResult;
	serializeJson(doc, jsonResult);

	unsigned long elapsed = millis() - startTime;
	Log_Printf(LOGLEVEL_INFO, "Browse completed in %lu ms, result size: %d bytes", elapsed, jsonResult.length());
	Log_Printf(LOGLEVEL_DEBUG, "Browse result JSON: %s", jsonResult.c_str());

	// Send result via WebSocket
	Web_SendWebsocketData(0, WebsocketCode::MediaServerBrowseResult, nodeId.c_str(), jsonResult.c_str());
	System_UpdateActivityTimer();
	// Mark request as completed
	pendingBrowseRequest.pending = false;
}

/*
// Process download request (runs in task context)
static void processDownloadRequest(uint8_t serverId, const String &objectId, const String &filename) {
	unsigned long startTime = millis();
	Log_Printf(LOGLEVEL_INFO, "Processing download request: serverId=%d, objectId=%s, filename=%s",
		serverId, objectId.c_str(), filename.c_str());

	// First, we need to browse to get the file details (URI, etc.)
	soapObjectVect_t browseResult;
	bool success = soap.browseServer(serverId, objectId.c_str(), &browseResult);

	if (!success || browseResult.size() == 0) {
		Log_Printf(LOGLEVEL_ERROR, "Failed to get file info for objectId=%s", objectId.c_str());
		return;
	}

	// Find the matching item (should be first one for single objectId browse)
	soapObject_t *fileObj = nullptr;
	for (size_t i = 0; i < browseResult.size(); i++) {
		if (browseResult[i].id == objectId) {
			fileObj = &browseResult[i];
			break;
		}
	}

	if (!fileObj || fileObj->uri.length() == 0) {
		Log_Println("File URI not found", LOGLEVEL_ERROR);
		return;
	}

	// Determine file extension from protocolInfo or use .mp3 as default
	String extension = ".mp3"; // Default extension
	#if !defined(NO_PROTOCOL_INFO)
	if (fileObj->protInfo.length() > 0) {
		Log_Printf(LOGLEVEL_INFO, "Protocol Info: %s", fileObj->protInfo.c_str());

		// Parse protocolInfo format: "http-get:*:audio/mpeg:*" or similar
		int secondColon = fileObj->protInfo.indexOf(':', fileObj->protInfo.indexOf(':') + 1);
		int thirdColon = fileObj->protInfo.indexOf(':', secondColon + 1);

		if (secondColon > 0 && thirdColon > secondColon) {
			String mimeType = fileObj->protInfo.substring(secondColon + 1, thirdColon);

			// Extract extension from MIME type
			if (mimeType.indexOf("audio/mpeg") >= 0 || mimeType.indexOf("audio/mp3") >= 0) {
				extension = ".mp3";
			} else if (mimeType.indexOf("audio/mp4") >= 0 || mimeType.indexOf("audio/x-m4a") >= 0) {
				extension = ".m4a";
			} else if (mimeType.indexOf("audio/flac") >= 0) {
				extension = ".flac";
			} else if (mimeType.indexOf("audio/wav") >= 0 || mimeType.indexOf("audio/x-wav") >= 0) {
				extension = ".wav";
			} else if (mimeType.indexOf("audio/ogg") >= 0) {
				extension = ".ogg";
			} else if (mimeType.indexOf("audio/aac") >= 0) {
				extension = ".aac";
			} else if (mimeType.indexOf("video/mp4") >= 0) {
				extension = ".mp4";
			} else if (mimeType.indexOf("video/x-msvideo") >= 0) {
				extension = ".avi";
			} else if (mimeType.indexOf("image/jpeg") >= 0) {
				extension = ".jpg";
			} else if (mimeType.indexOf("image/png") >= 0) {
				extension = ".png";
			}

			Log_Printf(LOGLEVEL_DEBUG, "Detected file extension from MIME type: %s", extension.c_str());
		}
	}
	#endif

	// Create target path on SD card with proper extension
	String targetPath = "/mediaserver/" + filename + extension;

	// Create directory if needed
	if (!gFSystem.exists("/mediaserver")) {
		if (!gFSystem.mkdir("/mediaserver")) {
			Log_Println("Failed to create /mediaserver directory", LOGLEVEL_ERROR);
			return;
		}
		Log_Println("Created /mediaserver directory", LOGLEVEL_INFO);
	}

	// Start download using SoapESP32 built-in function
	size_t fileSize = 0;
	if (!soap.readStart(fileObj, &fileSize)) {
		Log_Println("Failed to start download from media server", LOGLEVEL_ERROR);
		return;
	}

	Log_Printf(LOGLEVEL_NOTICE, "Download started: %s (%d bytes)", filename.c_str(), fileSize);

	// Check SD card space
	uint64_t freeBytes = SdCard_GetFreeSize();
	Log_Printf(LOGLEVEL_INFO, "SD card space: %llu bytes free", freeBytes);

	if (freeBytes < fileSize) {
		Log_Printf(LOGLEVEL_ERROR, "Not enough space on SD card (need: %d, free: %llu)", fileSize, freeBytes);
		soap.readStop();
		return;
	}

	// Open file for writing using FILE_WRITE mode
	File file = gFSystem.open(targetPath.c_str(), FILE_WRITE);
	if (!file) {
		Log_Printf(LOGLEVEL_ERROR, "Failed to create file: %s", targetPath.c_str());
		soap.readStop();
		return;
	}

	Log_Printf(LOGLEVEL_DEBUG, "File opened successfully: %s", targetPath.c_str());

	// Allocate buffer for download
	const size_t BUFFER_SIZE = 4096;
	uint8_t *buffer = (uint8_t *)ps_malloc(BUFFER_SIZE);
	if (!buffer) {
		Log_Println("Failed to allocate download buffer", LOGLEVEL_ERROR);
		file.close();
		soap.readStop();
		return;
	}

	size_t totalRead = 0;
	size_t lastProgress = 0;

	// Read data from server and write to SD card
	while (soap.available()) {
		int bytesRead = soap.read(buffer, BUFFER_SIZE);

		if (bytesRead < 0) {
			// Read error
			Log_Printf(LOGLEVEL_ERROR, "Read error at %d bytes", totalRead);
			break;
		}
		else if (bytesRead > 0) {
			// Write to SD card
			size_t bytesWritten = file.write(buffer, bytesRead);
			if (bytesWritten != (size_t)bytesRead) {
				Log_Printf(LOGLEVEL_ERROR, "Write error at %d bytes (read: %d, written: %d)",
					totalRead, bytesRead, bytesWritten);
				Log_Printf(LOGLEVEL_ERROR, "File status - size: %d, position: %d", file.size(), file.position());
				break;
			}

			totalRead += bytesRead;

			// Flush to SD card periodically (every 64KB)
			if (totalRead % (64 * 1024) == 0) {
				file.flush();
			}

			// Log progress every 10%
			size_t progress = (totalRead * 100) / fileSize;
			if (progress >= lastProgress + 10) {
				Log_Printf(LOGLEVEL_INFO, "Download progress: %d%% (%d/%d bytes)", progress, totalRead, fileSize);
				lastProgress = progress;
			}
		}

		// Yield periodically to prevent watchdog timeout
		vTaskDelay(pdMS_TO_TICKS(1));
	}

	// Close connection and file
	soap.readStop();
	free(buffer);

	// Final flush before closing
	file.flush();
	Log_Printf(LOGLEVEL_DEBUG, "Final file size before close: %d bytes", file.size());
	file.close();

	unsigned long elapsed = millis() - startTime;

	if (totalRead == fileSize) {
		Log_Printf(LOGLEVEL_NOTICE, "Download completed: %s (%d bytes in %lu ms)",
			filename.c_str(), totalRead, elapsed);
	} else {
		Log_Printf(LOGLEVEL_ERROR, "Download incomplete: %d/%d bytes", totalRead, fileSize);
		// Delete incomplete file
		gFSystem.remove(targetPath.c_str());
	}

	System_UpdateActivityTimer();
}
*/

#endif // DLNA_ENABLE
