#include <Arduino.h>
#include "settings.h"

#include "Bluetooth.h"

#include "Common.h"
#include "Log.h"
#include "RotaryEncoder.h"
#include "System.h"
#include "Web.h"

#include <AudioPlayer.h>

#ifdef BLUETOOTH_ENABLE
	#include "BluetoothA2DPCommon.h"
	#include "BluetoothA2DPSink.h"
	#include "BluetoothA2DPSource.h"
	#include "esp_bt.h"
	#include "esp_heap_caps.h"
	#if (defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3))
		#include "ESP_I2S.h"
I2SClass i2s;
	#endif
#endif

#ifdef BLUETOOTH_ENABLE
	#define BLUETOOTHPLAYER_VOLUME_MAX 21u
	#define BLUETOOTHPLAYER_VOLUME_MIN 0u
	#define BT_DEVICE_LIST_MAX_SIZE	   10

BluetoothA2DPSink *a2dp_sink;
BluetoothA2DPSource *a2dp_source;
RingbufHandle_t audioSourceRingBuffer;
static bool bluetoothSourceConnected = false;
String btDeviceName;

// Bluetooth device list for source mode (ringbuffer)
typedef struct {
	char name[64];
	int rssi;
} BtDeviceInfo_t;

static BtDeviceInfo_t *btDeviceList = nullptr; // allocated in PSRAM if available
static uint8_t btDeviceListCount = 0;
static uint8_t btDeviceListHead = 0; // ringbuffer write position
static SemaphoreHandle_t btDeviceListMutex = nullptr;
static bool btDeviceListChanged = false; // flag set when device found
static unsigned long btDeviceListLastSent = 0; // timestamp of last WebSocket send
static String btConnectedDeviceName = ""; // name of currently connected device
static bool btConnectionStatusChanged = false; // flag for connection status change
#endif

#ifdef BLUETOOTH_ENABLE
const char *getType() {
	if (System_GetOperationMode() == OPMODE_BLUETOOTH_SINK) {
		return "sink";
	} else {
		return "source";
	}
}
#endif

#ifdef BLUETOOTH_ENABLE
static void Bluetooth_Source_FlushRingbuffer(void) {
	if (!audioSourceRingBuffer) {
		return;
	}
	size_t bytesRead = 0;
	uint8_t *item = nullptr;
	while ((item = static_cast<uint8_t *>(xRingbufferReceiveUpTo(audioSourceRingBuffer, &bytesRead, 0, SIZE_MAX))) != nullptr) {
		vRingbufferReturnItem(audioSourceRingBuffer, item);
	}
}
#endif

#ifdef BLUETOOTH_ENABLE
// callback for peer device name (Bluetooth sink mode)
void peer_name_callback(char *peer_name) {
	if (peer_name != nullptr && strlen(peer_name) > 0) {
		btConnectedDeviceName = peer_name;
		btConnectionStatusChanged = true; // Trigger WebSocket update
		Log_Printf(LOGLEVEL_INFO, "Bluetooth sink => peer device name: %s", peer_name);
	}
}
#endif

#ifdef BLUETOOTH_ENABLE
// for esp_a2d_connection_state_t see https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/bluetooth/esp_a2dp.html#_CPPv426esp_a2d_connection_state_t
void connection_state_changed(esp_a2d_connection_state_t state, void *ptr) {
	const char *deviceName = (state == ESP_A2D_CONNECTION_STATE_CONNECTED && !btConnectedDeviceName.isEmpty()) ? btConnectedDeviceName.c_str() : "";
	if (deviceName[0] != '\0') {
		Log_Printf(LOGLEVEL_INFO, "Bluetooth %s => connection state: %s, device: %s (Free heap: %u Bytes)", getType(), ((BluetoothA2DPCommon *) ptr)->to_str(state), deviceName, ESP.getFreeHeap());
	} else {
		Log_Printf(LOGLEVEL_INFO, "Bluetooth %s => connection state: %s (Free heap: %u Bytes)", getType(), ((BluetoothA2DPCommon *) ptr)->to_str(state), ESP.getFreeHeap());
	}
	if (System_GetOperationMode() == OPMODE_BLUETOOTH_SINK) {
		// for Neopixel (indicator LEDs) use the webstream mode
		gPlayProperties.isWebstream = false;
		gPlayProperties.pausePlay = false;
		gPlayProperties.playlistFinished = true;
		const bool connected = (state == ESP_A2D_CONNECTION_STATE_CONNECTED);
		if (!connected) {
			btConnectedDeviceName = "";
		}
		btConnectionStatusChanged = true; // Trigger WebSocket update
	} else if (System_GetOperationMode() == OPMODE_BLUETOOTH_SOURCE) {
		const bool connected = (state == ESP_A2D_CONNECTION_STATE_CONNECTED);
		if (!connected && bluetoothSourceConnected) {
			Bluetooth_Source_FlushRingbuffer();
			btConnectedDeviceName = "";
		} else if (connected && !bluetoothSourceConnected) {
			// Connected - device name should already be set from scan callback or connect function
			// If not set, use the saved device name
			if (btConnectedDeviceName.isEmpty() && !btDeviceName.isEmpty()) {
				btConnectedDeviceName = btDeviceName;
				Log_Printf(LOGLEVEL_INFO, "Bluetooth source => Using saved device name: %s", btDeviceName.c_str());
			}
		}
		bluetoothSourceConnected = connected;
		btConnectionStatusChanged = true; // Trigger WebSocket update
	}
}
#endif

#ifdef BLUETOOTH_ENABLE
// for esp_a2d_audio_state_t see https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/bluetooth/esp_a2dp.html#_CPPv421esp_a2d_audio_state_t
void audio_state_changed(esp_a2d_audio_state_t state, void *ptr) {
	Log_Printf(LOGLEVEL_INFO, "Bluetooth %s => audio state: %s (Free heap: %u Bytes)", getType(), ((BluetoothA2DPCommon *) ptr)->to_str(state), ESP.getFreeHeap());
	if (System_GetOperationMode() == OPMODE_BLUETOOTH_SINK) {
		// set play/pause status
		gPlayProperties.pausePlay = (state != ESP_A2D_AUDIO_STATE_STARTED);
		// for Neopixel (indicator LEDs) use the webstream mode
		gPlayProperties.playlistFinished = false;
		gPlayProperties.isWebstream = true;
	}
}
#endif

#ifdef BLUETOOTH_ENABLE
// gets called when pause/resume/next/previous button on bluetooth speaker is pressed
void button_handler(uint8_t id, bool isReleased) {
	if (isReleased) {
		switch (id) {
			case 70:
			case 68:
				// 70=pause, 68=resume
				Log_Printf(LOGLEVEL_DEBUG, "Bluetooth button id %u (pause/resume) is released.", id);
				AudioPlayer_SetTrackControl(PAUSEPLAY);
				break;
			case 75:
				// 75=next
				Log_Printf(LOGLEVEL_DEBUG, "Bluetooth button id %u (next track) is released.", id);
				AudioPlayer_SetTrackControl(NEXTTRACK);
				break;
			case 76:
				// 76=previous
				Log_Printf(LOGLEVEL_DEBUG, "Bluetooth button id %u (previous track) is released.", id);
				AudioPlayer_SetTrackControl(PREVIOUSTRACK);
				break;
			default:
				// unknown/unsupported button id
				Log_Printf(LOGLEVEL_DEBUG, "Unknown bluetooth button id %u is released.", id);
				break;
		}
	}
}
#endif

#ifdef BLUETOOTH_ENABLE
// handle Bluetooth AVRC metadata
// https://docs.espressif.com/projects/esp-idf/en/release-v3.2/api-reference/bluetooth/esp_avrc.html
void avrc_metadata_callback(uint8_t id, const uint8_t *text) {
	if (strlen((char *) text) == 0) {
		return;
	}
	switch (id) {
		case ESP_AVRC_MD_ATTR_TITLE:
			// title
			Log_Printf(LOGLEVEL_DEBUG, "Bluetooth => AVRC Title: %s", text);
			break;
		case ESP_AVRC_MD_ATTR_ARTIST:
			// artists
			Log_Printf(LOGLEVEL_DEBUG, "Bluetooth => AVRC Artist: %s", text);
			break;
		case ESP_AVRC_MD_ATTR_ALBUM:
			// album
			Log_Printf(LOGLEVEL_DEBUG, "Bluetooth => AVRC Album: %s", text);
			break;
		case ESP_AVRC_MD_ATTR_TRACK_NUM:
			// current track number
			Log_Printf(LOGLEVEL_DEBUG, "Bluetooth => AVRC Track-No: %s", text);
			break;
		case ESP_AVRC_MD_ATTR_NUM_TRACKS:
			// number of tracks in playlist
			Log_Printf(LOGLEVEL_DEBUG, "Bluetooth => AVRC Number of tracks: %s", text);
			break;
		case ESP_AVRC_MD_ATTR_GENRE:
			// genre
			Log_Printf(LOGLEVEL_DEBUG, "Bluetooth => AVRC Genre: %s", text);
			break;
		default:
			// unknown/unsupported metadata
			Log_Printf(LOGLEVEL_DEBUG, "Bluetooth => AVRC metadata rsp: attribute id 0x%x, %s", id, text);
			break;
	}
}
#endif

#ifdef BLUETOOTH_ENABLE
// feed the A2DP source with audio data
int32_t get_data_channels(Frame *frame, int32_t channel_len) {
	if (channel_len < 0 || frame == NULL) {
		return 0;
	}
	if (audioSourceRingBuffer == NULL) {
		return 0;
	}
	// Receive data from ring buffer
	size_t len {};
	vRingbufferGetInfo(audioSourceRingBuffer, nullptr, nullptr, nullptr, nullptr, &len);
	if (len < (channel_len * 4)) {
		// Serial.println("Bluetooth source => not enough data");
		return 0;
	};
	size_t sampleSize = 0;
	uint8_t *sampleBuff;
	sampleBuff = (uint8_t *) xRingbufferReceiveUpTo(audioSourceRingBuffer, &sampleSize, (TickType_t) portMAX_DELAY, channel_len * 4);
	if (sampleBuff != NULL) {
		// fill the channel data
		for (int sample = 0; sample < (channel_len); ++sample) {
			frame[sample].channel1 = (sampleBuff[sample * 4 + 3] << 8) | sampleBuff[sample * 4 + 2];
			frame[sample].channel2 = (sampleBuff[sample * 4 + 1] << 8) | sampleBuff[sample * 4];
		};
		vRingbufferReturnItem(audioSourceRingBuffer, (void *) sampleBuff);
	};
	// avoid WDT reset & give audio/other tasks some CPU time
	vTaskDelay(portTICK_PERIOD_MS * 1);
	return channel_len;
};
#endif

#ifdef BLUETOOTH_ENABLE
// callback which is notified on update Receiver RSSI
void rssi(esp_bt_gap_cb_param_t::read_rssi_delta_param &rssiParam) {
	Log_Printf(LOGLEVEL_DEBUG, "Bluetooth => RSSI value: %d", rssiParam.rssi_delta);
}
#endif

#ifdef BLUETOOTH_ENABLE
// Add device to list (called from scan callback)
// Returns true if a new device was added, false if only RSSI was updated
static bool addDeviceToList(const char *ssid, int rssi) {
	if (btDeviceListMutex == nullptr || btDeviceList == nullptr) {
		return false;
	}

	bool newDeviceAdded = false;
	if (xSemaphoreTake(btDeviceListMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
		// Check if device already exists
		bool found = false;
		for (uint8_t i = 0; i < btDeviceListCount; i++) {
			if (strcmp(btDeviceList[i].name, ssid) == 0) {
				// Update RSSI only
				btDeviceList[i].rssi = rssi;
				found = true;
				break;
			}
		}

		if (!found) {
			// Add new device at head position (ringbuffer)
			strncpy(btDeviceList[btDeviceListHead].name, ssid, sizeof(btDeviceList[0].name) - 1);
			btDeviceList[btDeviceListHead].name[sizeof(btDeviceList[0].name) - 1] = '\0';
			btDeviceList[btDeviceListHead].rssi = rssi;

			// Move head forward (ringbuffer)
			btDeviceListHead = (btDeviceListHead + 1) % BT_DEVICE_LIST_MAX_SIZE;

			// Increase count up to max size
			if (btDeviceListCount < BT_DEVICE_LIST_MAX_SIZE) {
				btDeviceListCount++;
			}
			newDeviceAdded = true;
		}

		// Sort by RSSI (highest first)
		for (uint8_t i = 0; i < btDeviceListCount - 1; i++) {
			for (uint8_t j = 0; j < btDeviceListCount - i - 1; j++) {
				if (btDeviceList[j].rssi < btDeviceList[j + 1].rssi) {
					BtDeviceInfo_t temp = btDeviceList[j];
					btDeviceList[j] = btDeviceList[j + 1];
					btDeviceList[j + 1] = temp;
				}
			}
		}

		xSemaphoreGive(btDeviceListMutex);
	}
	return newDeviceAdded;
}
#endif

#ifdef BLUETOOTH_ENABLE
// Callback notifying BT-source devices available.
// Return true to connect, false will continue scanning
bool scan_bluetooth_device_callback(const char *ssid, esp_bd_addr_t address, int rssi) {
	// Add device to list - set flag if new device was added for immediate notification
	if (addDeviceToList(ssid, rssi)) {
		Log_Printf(LOGLEVEL_INFO, "Bluetooth source => New device found: %s", ssid);
		btDeviceListChanged = true;
	}

	if (btDeviceName == "") {
		return false;
	} else {
		// connect if device name (partially) matching, todo: compare case insensitive here?
		bool shouldConnect = startsWith(ssid, btDeviceName.c_str());
		if (shouldConnect) {
			// Set the connected device name when we decide to connect
			btConnectedDeviceName = ssid;
			Log_Printf(LOGLEVEL_INFO, "Bluetooth source => Connecting to: %s", ssid);
		}
		return shouldConnect;
	}
}
#endif

void Bluetooth_VolumeChanged(int _newVolume) {
#ifdef BLUETOOTH_ENABLE
	if ((_newVolume < 0) || (_newVolume > 0x7F)) {
		return;
	}
	// map bluetooth volume (0..127) to ESPuino volume (0..21) to
	uint8_t _volume;
	_volume = map(_newVolume, 0, 0x7F, BLUETOOTHPLAYER_VOLUME_MIN, BLUETOOTHPLAYER_VOLUME_MAX);
	if (AudioPlayer_GetCurrentVolume() != _volume) {
		Log_Printf(LOGLEVEL_INFO, "Bluetooth => volume changed:  %d !", _volume);
		AudioPlayer_SetVolume(_volume, true);
	}
#endif
}

void Bluetooth_Init(void) {
#ifdef BLUETOOTH_ENABLE
	bluetoothSourceConnected = false;
	if (System_GetOperationMode() == OPMODE_BLUETOOTH_SINK) {
		// bluetooth in sink mode (player acts as a BT-Speaker)
		a2dp_sink = new BluetoothA2DPSink();
	#if (defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3))
		i2s.setPins(I2S_BCLK, I2S_LRC, I2S_DOUT);
		if (!i2s.begin(I2S_MODE_STD, 44100, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
			Log_Println("Failed to initialize I2S!", LOGLEVEL_ERROR);
			while (1)
				; // do nothing
		}
		a2dp_sink->set_output(i2s);
	#else
		i2s_pin_config_t pin_config = {
		#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 0, 0)
			.mck_io_num = 0,
		#endif
			.bck_io_num = I2S_BCLK,
			.ws_io_num = I2S_LRC,
			.data_out_num = I2S_DOUT,
			.data_in_num = I2S_PIN_NO_CHANGE,
		};
		a2dp_sink->set_pin_config(pin_config);
	#endif
		a2dp_sink->set_rssi_callback(rssi);
		a2dp_sink->activate_pin_code(false);
		if (gPrefsSettings.getBool("playMono", false)) {
			a2dp_sink->set_mono_downmix(true);
		}
		a2dp_sink->set_auto_reconnect(true);
		a2dp_sink->set_rssi_active(true);
		// start bluetooth sink
		a2dp_sink->start(nameBluetoothSinkDevice);
		Log_Printf(LOGLEVEL_INFO, "Bluetooth sink started, Device: %s", nameBluetoothSinkDevice);
		// connect events after startup
		a2dp_sink->set_on_connection_state_changed(connection_state_changed, a2dp_sink);
		a2dp_sink->set_on_audio_state_changed(audio_state_changed, a2dp_sink);
		a2dp_sink->set_avrc_metadata_callback(avrc_metadata_callback);
		a2dp_sink->set_on_volumechange(Bluetooth_VolumeChanged);
		a2dp_sink->set_peer_name_callback(peer_name_callback);
	} else if (System_GetOperationMode() == OPMODE_BLUETOOTH_SOURCE) {
		// Ringbuffer will be allocated on first use (lazy initialization)
		audioSourceRingBuffer = NULL;

		// Initialize BT device list
		btDeviceListMutex = xSemaphoreCreateMutex();
		if (btDeviceListMutex != NULL) {
	#ifdef BOARD_HAS_PSRAM
			if (psramFound()) {
				btDeviceList = (BtDeviceInfo_t *) heap_caps_malloc(BT_DEVICE_LIST_MAX_SIZE * sizeof(BtDeviceInfo_t), MALLOC_CAP_SPIRAM);
				if (btDeviceList != NULL) {
					Log_Printf(LOGLEVEL_INFO, "Bluetooth => btDeviceList created in PSRAM (%d bytes)", BT_DEVICE_LIST_MAX_SIZE * sizeof(BtDeviceInfo_t));
				} else {
					Log_Println("Failed to allocate PSRAM for btDeviceList, using heap", LOGLEVEL_ERROR);
					btDeviceList = (BtDeviceInfo_t *) malloc(BT_DEVICE_LIST_MAX_SIZE * sizeof(BtDeviceInfo_t));
				}
			} else
	#endif
			{
				btDeviceList = (BtDeviceInfo_t *) malloc(BT_DEVICE_LIST_MAX_SIZE * sizeof(BtDeviceInfo_t));
				Log_Printf(LOGLEVEL_INFO, "Bluetooth => btDeviceList created in heap (%d bytes)", BT_DEVICE_LIST_MAX_SIZE * sizeof(BtDeviceInfo_t));
			}

			if (btDeviceList != NULL) {
				memset(btDeviceList, 0, BT_DEVICE_LIST_MAX_SIZE * sizeof(BtDeviceInfo_t));
				btDeviceListCount = 0;
				btDeviceListHead = 0;
				Log_Println("Bluetooth device list initialized", LOGLEVEL_INFO);
			} else {
				Log_Println("Failed to allocate btDeviceList!", LOGLEVEL_ERROR);
			}
		} else {
			Log_Println("Failed to create btDeviceListMutex!", LOGLEVEL_ERROR);
		}

		//  setup BT source
		a2dp_source = new BluetoothA2DPSource();

		// a2dp_source->set_task_core(1);            // task core
		// a2dp_source->set_nvs_init(true);          // erase/initialize NVS
		// a2dp_source->set_ssp_enabled(true);       // enable secure simple pairing

		// pairing pin-code, see https://forum.espuino.de/t/neues-feature-bluetooth-kopfhoerer/1293/30
		String btPinCode = gPrefsSettings.getString("btPinCode", "");
		if (btPinCode != "") {
			a2dp_source->set_ssp_enabled(true);
			a2dp_source->set_pin_code(btPinCode.c_str(), ESP_BT_PIN_TYPE_VARIABLE);
		}
		// start bluetooth source
		a2dp_source->set_ssid_callback(scan_bluetooth_device_callback);
		a2dp_source->set_avrc_passthru_command_callback(button_handler);
		// get device name
		btDeviceName = "";
		if (gPrefsSettings.isKey("btDeviceName")) {
			btDeviceName = gPrefsSettings.getString("btDeviceName", "");
			btConnectedDeviceName = btDeviceName; // Set for auto-reconnect
		}
		a2dp_source->set_auto_reconnect(btDeviceName != ""); // auto reconnect
		a2dp_source->start(get_data_channels);
		Log_Printf(LOGLEVEL_INFO, "Bluetooth source started, connect to device: '%s'", (btDeviceName == "") ? "connect to first device found" : btDeviceName.c_str());
		// connect events after startup
		a2dp_source->set_on_connection_state_changed(connection_state_changed, a2dp_source);
		a2dp_source->set_on_audio_state_changed(audio_state_changed, a2dp_source);
		// max headphone volume (0..255): volume is controlled by audio class
		a2dp_source->set_volume(127);
	} else {
		esp_bt_mem_release(ESP_BT_MODE_BTDM);
	}
#endif
}

void Bluetooth_Cyclic(void) {
#ifdef BLUETOOTH_ENABLE
	if ((System_GetOperationMode() == OPMODE_BLUETOOTH_SINK) && (a2dp_sink)) {
		esp_a2d_audio_state_t state = a2dp_sink->get_audio_state();
		// Reset Sleep Timer when audio is playing
		if (state == ESP_A2D_AUDIO_STATE_STARTED) {
			System_UpdateActivityTimer();
		}
	}
	if ((System_GetOperationMode() == OPMODE_BLUETOOTH_SOURCE) && (a2dp_source)) {
		esp_a2d_audio_state_t state = a2dp_source->get_audio_state();
		// Reset Sleep Timer when audio is playing
		if (state == ESP_A2D_AUDIO_STATE_STARTED) {
			System_UpdateActivityTimer();
		}
		// Send device list update: immediately when new device found OR every 10 seconds
		unsigned long now = millis();
		if (btDeviceListChanged || (now - btDeviceListLastSent >= 10000)) {
			btDeviceListChanged = false;
			btDeviceListLastSent = now;
			Web_SendWebsocketData(0, WebsocketCodeType::BluetoothDeviceList);
		}
	}
	// Send connection status update when changed (both sink and source)
	if ((System_GetOperationMode() == OPMODE_BLUETOOTH_SOURCE || System_GetOperationMode() == OPMODE_BLUETOOTH_SINK)) {
		// Send connection status update when changed
		if (btConnectionStatusChanged) {
			btConnectionStatusChanged = false;
			Web_SendWebsocketData(0, WebsocketCodeType::BluetoothConnectionStatus);
		}
	}
#endif
}

void Bluetooth_PlayPauseTrack(void) {
#ifdef BLUETOOTH_ENABLE
	if ((System_GetOperationMode() == OPMODE_BLUETOOTH_SINK) && (a2dp_sink)) {
		esp_a2d_audio_state_t state = a2dp_sink->get_audio_state();
		if (state == ESP_A2D_AUDIO_STATE_STARTED) {
			a2dp_sink->play();
		} else {
			a2dp_sink->pause();
		}
	}
#endif
}

void Bluetooth_NextTrack(void) {
#ifdef BLUETOOTH_ENABLE
	if ((System_GetOperationMode() == OPMODE_BLUETOOTH_SINK) && (a2dp_sink)) {
		a2dp_sink->next();
	}
#endif
}

void Bluetooth_PreviousTrack(void) {
#ifdef BLUETOOTH_ENABLE
	if ((System_GetOperationMode() == OPMODE_BLUETOOTH_SINK) && (a2dp_sink)) {
		a2dp_sink->previous();
	}
#endif
}

// set volume from ESPuino to phone needs at least Arduino ESP version 2.0.0
void Bluetooth_SetVolume(const int32_t _newVolume, bool reAdjustRotary) {
#ifdef BLUETOOTH_ENABLE
	if (!a2dp_sink) {
		return;
	}
	uint8_t _volume;
	if (_newVolume < int32_t(BLUETOOTHPLAYER_VOLUME_MIN)) {
		return;
	} else if (_newVolume > BLUETOOTHPLAYER_VOLUME_MAX) {
		return;
	} else {
		// map ESPuino min/max volume (0..21) to bluetooth volume (0..127)
		_volume = map(_newVolume, BLUETOOTHPLAYER_VOLUME_MIN, BLUETOOTHPLAYER_VOLUME_MAX, 0, 0x7F);
		a2dp_sink->set_volume(_volume);
		if (reAdjustRotary) {
			RotaryEncoder_Readjust();
		}
	}
#endif
}

bool Bluetooth_Source_SendAudioData(int16_t *outBuff, int32_t validSamples) {
#ifdef BLUETOOTH_ENABLE
	// Lazy initialization: create ringbuffer on first use to save heap memory
	if ((System_GetOperationMode() == OPMODE_BLUETOOTH_SOURCE) && (a2dp_source) && bluetoothSourceConnected && (validSamples > 0)) {
		// Create ringbuffer on first use (lazy initialization)
		if (audioSourceRingBuffer == NULL) {
	#ifdef BOARD_HAS_PSRAM
			if (psramFound()) {
				// Allocate ringbuffer storage in PSRAM
				size_t bufferSize = 8192;
				StaticRingbuffer_t *bufferStruct = (StaticRingbuffer_t *) heap_caps_malloc(sizeof(StaticRingbuffer_t), MALLOC_CAP_SPIRAM);
				uint8_t *bufferStorage = (uint8_t *) heap_caps_malloc(bufferSize, MALLOC_CAP_SPIRAM);

				if (bufferStruct != NULL && bufferStorage != NULL) {
					audioSourceRingBuffer = xRingbufferCreateStatic(bufferSize, RINGBUF_TYPE_BYTEBUF, bufferStorage, bufferStruct);
					Log_Printf(LOGLEVEL_INFO, "Bluetooth => audioSourceRingBuffer created in PSRAM on demand (%d bytes, free heap: %u Bytes)", bufferSize, ESP.getFreeHeap());
				} else {
					Log_Println("Failed to allocate PSRAM for audioSourceRingBuffer, using heap", LOGLEVEL_ERROR);
					if (bufferStruct) {
						heap_caps_free(bufferStruct);
					}
					if (bufferStorage) {
						heap_caps_free(bufferStorage);
					}
					audioSourceRingBuffer = xRingbufferCreate(8192, RINGBUF_TYPE_BYTEBUF);
				}
			} else
	#endif
			{
				audioSourceRingBuffer = xRingbufferCreate(8192, RINGBUF_TYPE_BYTEBUF);
				Log_Printf(LOGLEVEL_INFO, "Bluetooth => audioSourceRingBuffer created in heap on demand (Free heap: %u Bytes)", ESP.getFreeHeap());
			}

			if (audioSourceRingBuffer == NULL) {
				Log_Println("Failed to create audioSourceRingBuffer!", LOGLEVEL_ERROR);
				return false;
			}
		}

		const TickType_t sendTimeout = pdMS_TO_TICKS(20);
		return (pdTRUE == xRingbufferSend(audioSourceRingBuffer, outBuff, sizeof(uint32_t) * validSamples, sendTimeout));
	} else {
		return false;
	}
#else
	return false;
#endif
}

bool Bluetooth_Device_Connected() {
#ifdef BLUETOOTH_ENABLE
	// send audio data to ringbuffer
	return (((System_GetOperationMode() == OPMODE_BLUETOOTH_SINK) && (a2dp_sink) && a2dp_sink->is_connected()) || ((System_GetOperationMode() == OPMODE_BLUETOOTH_SOURCE) && (a2dp_source) && bluetoothSourceConnected));
#else
	return false;
#endif
}

// Connect to a specific Bluetooth device (Source mode)
void Bluetooth_ConnectDevice(const char *deviceName) {
#ifdef BLUETOOTH_ENABLE
	if (System_GetOperationMode() == OPMODE_BLUETOOTH_SOURCE && a2dp_source && deviceName) {
		Log_Printf(LOGLEVEL_INFO, "Bluetooth source => Connecting to device: %s", deviceName);
		btDeviceName = deviceName;
		btConnectedDeviceName = deviceName; // Optimistically set the name
		gPrefsSettings.putString("btDeviceName", btDeviceName);
		// Reconnect to apply new device name
		a2dp_source->set_auto_reconnect(true);
		a2dp_source->reconnect();
	}
#endif
}

// Get name of currently connected device
String Bluetooth_GetConnectedDeviceName() {
#ifdef BLUETOOTH_ENABLE
	// Return device name if connected in either sink or source mode
	if (System_GetOperationMode() == OPMODE_BLUETOOTH_SINK) {
		if (a2dp_sink && a2dp_sink->is_connected()) {
			return btConnectedDeviceName;
		}
	} else if (System_GetOperationMode() == OPMODE_BLUETOOTH_SOURCE) {
		if (bluetoothSourceConnected) {
			return btConnectedDeviceName;
		}
	}
#endif
	return "";
}

// Get device list - calls callback for each device
void Bluetooth_GetDeviceList(BluetoothDeviceCallback callback) {
#ifdef BLUETOOTH_ENABLE
	if (btDeviceListMutex == nullptr || btDeviceList == nullptr || callback == nullptr) {
		return;
	}

	if (xSemaphoreTake(btDeviceListMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
		for (uint8_t i = 0; i < btDeviceListCount; i++) {
			callback(btDeviceList[i].name, btDeviceList[i].rssi);
		}
		xSemaphoreGive(btDeviceListMutex);
	}
#endif
}
