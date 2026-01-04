#pragma once

#include <functional>

void Bluetooth_Init(void);
void Bluetooth_Cyclic(void);

// AVRC commands, see https://github.com/pschatzmann/ESP32-A2DP/wiki/Controlling-your-Phone-with-AVRC-Commands

// Support for AVRC Commands starting from ESP32 Release 1.0.6
void Bluetooth_PlayPauseTrack(void);
void Bluetooth_NextTrack(void);
void Bluetooth_PreviousTrack(void);

// Support for AVRC Commands starting from ESP32 Release 2.0.0
void Bluetooth_SetVolume(const int32_t _newVolume, bool reAdjustRotary);

bool Bluetooth_Source_SendAudioData(int16_t *outBuff, int32_t validSamples);
bool Bluetooth_Device_Connected();

// Bluetooth device connection
void Bluetooth_ConnectDevice(const char* deviceName);
String Bluetooth_GetConnectedDeviceName();

// Bluetooth device list (for source mode) - callback for each device
// callback signature: void callback(const char* name, int rssi)
typedef std::function<void(const char* name, int rssi)> BluetoothDeviceCallback;
void Bluetooth_GetDeviceList(BluetoothDeviceCallback callback);
