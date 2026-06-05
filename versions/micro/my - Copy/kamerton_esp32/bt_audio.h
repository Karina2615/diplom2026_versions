#pragma once
#include <Arduino.h>
#include "BluetoothA2DPSource.h"
#include "audio_synth.h"

// Bluetooth A2DP audio source wrapper.
// Streams audio from g_synth to paired BT headphones.
// Library required: "ESP32-A2DP" by pschatzmann (Arduino Library Manager)
class BTAudio {
public:
    // device_name: name of the BT headphones to connect to.
    // Pass empty string to auto-reconnect to last paired device.
    void begin(const char* device_name = "");

    bool isConnected() const;

    // Start BT device discovery; calls found_cb for each device found.
    // Use for initial pairing UI. Stops after ~10 s or when stopScan() called.
    void startScan(esp_bt_gap_cb_t found_cb);
    void stopScan();

    // Connect to a specific device by BT address (obtained from scan)
    void connectTo(esp_bd_addr_t addr);

    BluetoothA2DPSource a2dp;

private:
    static int32_t audioCallback(Frame* frame, int32_t count);
};

extern BTAudio g_bt;
