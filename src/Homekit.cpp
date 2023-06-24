#include <Arduino.h>
#include "settings.h"
#include "Log.h"
#include "System.h"
#include "Cmd.h"
#include "Wlan.h"
#include "HomeSpan.h"



//
// ESPuino Homekit support
//
// default pairing code: 466-37-726
//

#ifdef HOMEKIT_ENABLE

	bool HomekitInitialized;

	struct HomeSpanEspuino : Service::Television {

		SpanCharacteristic *active = new Characteristic::Active(1);						// On/Off (set to ON at start-up)
		SpanCharacteristic *activeID = new Characteristic::ActiveIdentifier(1);			// Sets input source on start-up (default speaker)
		SpanCharacteristic *remoteKey = new Characteristic::RemoteKey();				// Used to receive button presses from the Remote Control widget
		SpanCharacteristic *settingsKey = new Characteristic::PowerModeSelection();		// Adds "View TV Setting" option to Selection Screen  

		HomeSpanEspuino(const char *name) : Service::Television() {
			new Characteristic::ConfiguredName(name);									// Name of device (TV)
			Log_Printf(LOGLEVEL_DEBUG, name, "Homekit: Configured device: \"%s\"\n");
		}

		boolean update() override {

			if(active->updated()){
				bool powerOn = active->getNewVal();
				Log_Printf(LOGLEVEL_DEBUG, "Homekit: Set power to: %s\n", powerOn?"ON":"OFF");
				if (!powerOn) {
					// turn off the device
					Cmd_Action(CMD_SLEEPMODE);
				}
			 }

			if(activeID->updated()){
				int inputSource = activeID->getNewVal(); 
				switch (inputSource){
					case 2:
						Log_Println("Homekit: Set Input Source to Bluetooth speaker", LOGLEVEL_DEBUG);
						Cmd_Action(CMD_TOGGLE_BLUETOOTH_SINK_MODE);
						break;
					case 3:
						Log_Println("Homekit: Set Input Source to Bluetooth headset", LOGLEVEL_DEBUG);
						Cmd_Action(CMD_TOGGLE_BLUETOOTH_SOURCE_MODE);
						break;
					default:
						Log_Println("Homekit: Set Input Source to speaker", LOGLEVEL_DEBUG);
				}
			}

			if(settingsKey->updated()){
				Log_Println("Homekit: Received request to \"View TV Settings\"", LOGLEVEL_DEBUG);
			}

			if(remoteKey->updated()){
				Log_Print("Homekit: Remote control key pressed: ", LOGLEVEL_DEBUG, false);
				switch(remoteKey->getNewVal()){
					case 4:
						Log_Println("UP ARROW", LOGLEVEL_DEBUG);
						Cmd_Action(CMD_VOLUMEUP);
						break;
					case 5:
						Log_Println("DOWN ARROW", LOGLEVEL_DEBUG);
						Cmd_Action(CMD_VOLUMEDOWN);
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
			return(true);
		}
	};

	void setupHomekit() {

		homeSpan.begin(Category::Television, "ESPuino");

		SPAN_ACCESSORY();

		// Below we define 3 different InputSource Services to appear to the user in the Home App

		SpanService *hdmi1 = new Service::InputSource(); 		// Source included in Selection List, but excluded from Settings Screen
		#if (LANGUAGE == DE) 
			new Characteristic::ConfiguredName("Lautsprecher");
		#else
			new Characteristic::ConfiguredName("Speaker");
		#endif
		new Characteristic::Identifier(1);

		#ifdef BLUETOOTH_ENABLE  
			SpanService *hdmi2 = new Service::InputSource();
			new Characteristic::ConfiguredName(nameBluetoothSinkDevice);
			new Characteristic::Identifier(2);
			new Characteristic::IsConfigured(1); 				// Source included in the Settings Screen...
			new Characteristic::CurrentVisibilityState(0);		// ...and included in the Selection List 

			 SpanService *hdmi3 = new Service::InputSource();
			String btDeviceName = gPrefsSettings.getString("btDeviceName", "");
			new Characteristic::ConfiguredName(btDeviceName.c_str());
			new Characteristic::Identifier(3);
			new Characteristic::IsConfigured(1);				// Source included in the Settings Screen...
			new Characteristic::CurrentVisibilityState(0);		// ...and included in the Selection List 
		#endif

		SpanService *speaker = new Service::TelevisionSpeaker();
		new Characteristic::VolumeSelector();
		new Characteristic::VolumeControlType(3);

		(new HomeSpanEspuino(nameBluetoothSinkDevice))			// Define a Television Service.  Must link in InputSources!
			->addLink(hdmi1)
			#ifdef BLUETOOTH_ENABLE 
				->addLink(hdmi2)
				->addLink(hdmi3)
			#endif
			->addLink(speaker)
	}

	void Homekit_Init(void) {
		HomekitInitialized = false;
	}

 	void Homekit_Cyclic(void) { 
		if (Wlan_IsConnected()) {
			if (!HomekitInitialized) {
				HomekitInitialized = true;
				homeSpan.setPortNum(8080);	// avoid collision with our webserver on port 80
        Log_Println("Homekit: Starting HAP Server", LOGLEVEL_DEBUG);
				setupHomekit();
			}
			homeSpan.poll();
		}
	}
#else // Homekit disabled, add dummy methods
	void Homekit_Init(void) {
	}

	void Homekit_Cyclic(void) {
	}  
#endif
