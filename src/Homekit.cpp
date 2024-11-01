#include <Arduino.h>
#include "settings.h"

#include "AudioPlayer.h"
#include "Cmd.h"
#include "HomeSpan.h"
#include "Log.h"
#include "System.h"
#include "Wlan.h"

//
// ESPuino Homekit support
//
// default pairing code: 466-37-726
//
#ifdef HOMEKIT_ENABLE

bool HomekitInitialized = false;

struct TelevisionSpeakerControl : Service::TelevisionSpeaker {

	SpanCharacteristic *volumeKey = new Characteristic::VolumeSelector();
	SpanCharacteristic *volumeControl = new Characteristic::VolumeControlType(3);
	SpanCharacteristic *muteKey = new Characteristic::Mute();

	boolean update() override {
		if (volumeKey->updated()) {
			// volume has changed
			int newVolume = volumeKey->getNewVal();
			if (newVolume == 0) {
				Log_Println("Homekit: Volume up", LOGLEVEL_DEBUG);
				Cmd_Action(CMD_VOLUMEUP);
			} else if (newVolume == 1) {
				Log_Println("Homekit: Volume down", LOGLEVEL_DEBUG);
				Cmd_Action(CMD_VOLUMEDOWN);
			}
		}
		if (muteKey->updated()) {
			Log_Println("Homekit: Mute", LOGLEVEL_DEBUG);
		}
		return (true);
	}
};

struct HomeSpanEspuino : Service::Television {

	SpanCharacteristic *active = new Characteristic::Active(1); // On/Off (set to ON at start-up)
	SpanCharacteristic *activeID = new Characteristic::ActiveIdentifier(1); // Sets input source on start-up (default speaker)
	SpanCharacteristic *remoteKey = new Characteristic::RemoteKey(); // Used to receive button presses from the Remote Control widget
	SpanCharacteristic *settingsKey = new Characteristic::PowerModeSelection(); // Adds "View TV Setting" option to Selection Screen

	HomeSpanEspuino(const char *name)
		: Service::Television() {
		new Characteristic::ConfiguredName(name); // Name of TV
		Log_Printf(LOGLEVEL_DEBUG, name, "Homekit: Configured device: \"%s\"\n");
	}

	boolean update() override {

		if (active->updated()) {
			bool powerOn = active->getNewVal();
			Log_Printf(LOGLEVEL_DEBUG, "Homekit: Set power to: %s\n", powerOn ? "ON" : "OFF");
			if (!powerOn) {
				// turn off the device
				Cmd_Action(CMD_SLEEPMODE);
			}
		}

		if (activeID->updated()) {
			int inputSource = activeID->getNewVal();
			switch (inputSource) {
				case 2:
					Log_Println("Homekit: Set Input Source to Bluetooth speaker", LOGLEVEL_DEBUG);
					Cmd_Action(CMD_TOGGLE_BLUETOOTH_SINK_MODE);
					break;
				case 3:
					Log_Println("Homekit: Set Input Source to Bluetooth headset", LOGLEVEL_DEBUG);
					Cmd_Action(CMD_TOGGLE_BLUETOOTH_SOURCE_MODE);
					break;
				default:
					Log_Println("Homekit: Set Input Source to ESPuino speaker", LOGLEVEL_DEBUG);
			}
		}

		if (settingsKey->updated()) {
			Log_Println("Homekit: Received request to \"View TV Settings\"", LOGLEVEL_DEBUG);
		}

		if (remoteKey->updated()) {
			Log_Print("Homekit: Remote control key pressed: ", LOGLEVEL_DEBUG, false);
			switch (remoteKey->getNewVal()) {
				case 4:
					Log_Println("UP ARROW", LOGLEVEL_DEBUG);
					Cmd_Action(CMD_LASTTRACK);
					break;
				case 5:
					Log_Println("DOWN ARROW", LOGLEVEL_DEBUG);
					Cmd_Action(CMD_FIRSTTRACK);
					break;
				case 6:
					Log_Println("LEFT ARROW", LOGLEVEL_DEBUG);
					Cmd_Action(CMD_PREVTRACK);
					break;
				case 7:
					Log_Println("RIGHT ARROW", LOGLEVEL_DEBUG);
					Cmd_Action(CMD_NEXTTRACK);
					break;
				case 8:
					Log_Println("SELECT", LOGLEVEL_DEBUG);
					break;
				case 9:
					Log_Println("BACK", LOGLEVEL_DEBUG);
					break;
				case 11:
					Log_Println("PLAY/PAUSE", LOGLEVEL_DEBUG);
					Cmd_Action(CMD_PLAYPAUSE);
					break;
				case 15:
					Log_Println("INFO", LOGLEVEL_DEBUG);
					Cmd_Action(CMD_TELL_IP_ADDRESS);
					break;
				default:
					Log_Println("UNKNOWN KEY", LOGLEVEL_DEBUG);
					System_IndicateError();
			}
		}
		return (true);
	}
};

void setupHomekit() {

	// homeSpan.setLogLevel(1); // for debugging
	homeSpan.begin(Category::Television, "ESPuino", "espuino");
	homeSpan.setQRID("HSPN");

	SPAN_ACCESSORY();

	// Below we define 3 different InputSource Services to appear to the user in the Home App

	// add ESPuino speaker
	SpanService *hdmi1 = new Service::InputSource(); // Source included in Selection List, but excluded from Settings Screen
	#if (LANGUAGE == DE)
	new Characteristic::ConfiguredName("Lautsprecher");
	#else
	new Characteristic::ConfiguredName("Speaker");
	#endif
	new Characteristic::Identifier(1);
	new Characteristic::IsConfigured(1); // Source included in the Settings Screen...
	new Characteristic::CurrentVisibilityState(0); // ...and included in the Selection List

	#ifdef BLUETOOTH_ENABLE
												   // add bluetooth sink
	SpanService *hdmi2 = new Service::InputSource();
		#if (LANGUAGE == DE)
	String btSinkName = "Bluetooth Lautsprecher";
		#else
	String btSinkName = "Bluetooth Speaker";
		#endif
	new Characteristic::ConfiguredName(btSinkName.c_str());
	new Characteristic::Identifier(2);
	new Characteristic::IsConfigured(1); // Source included in the Settings Screen...
	new Characteristic::CurrentVisibilityState(0); // ...and included in the Selection List

	// add bluetooth source
	SpanService *hdmi3 = new Service::InputSource();
		#if (LANGUAGE == DE)
	String btSourceName = "Bluetooth Kopfhörer";
		#else
	String btSourceName = "Bluetooth Headset";
		#endif
	new Characteristic::ConfiguredName(btSourceName.c_str());
	new Characteristic::Identifier(3);
	new Characteristic::IsConfigured(1); // Source included in the Settings Screen...
	new Characteristic::CurrentVisibilityState(0); // ...and included in the Selection List
	#endif

	SpanService *speaker = new TelevisionSpeakerControl();

	(new HomeSpanEspuino(nameBluetoothSinkDevice)) // Define a Television Service.  Must link in InputSources!
		->addLink(hdmi1)
	#ifdef BLUETOOTH_ENABLE
		->addLink(hdmi2)
		->addLink(hdmi3)
	#endif
		->addLink(speaker);
}

void Homekit_Cyclic(void) {
	if (Wlan_IsConnected()) {
		if (!HomekitInitialized) {
			HomekitInitialized = true;
			homeSpan.setPortNum(8080); // avoid collision with our webserver on port 80
			homeSpan.setWifiCredentials(WiFi.SSID().c_str(), WiFi.psk().c_str());
			Log_Println("Homekit: Starting HAP Server", LOGLEVEL_DEBUG);
			setupHomekit();
			Log_Printf(LOGLEVEL_DEBUG, "Homekit: HAP Server started. Free heap: %u", ESP.getFreeHeap());
		}
		homeSpan.poll();
	}
}
#else // Homekit disabled, add dummy method
void Homekit_Cyclic(void) {
}
#endif
