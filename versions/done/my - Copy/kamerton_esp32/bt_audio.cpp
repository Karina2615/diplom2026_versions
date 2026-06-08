#include "bt_audio.h"
#include "esp_gap_bt_api.h"

BTAudio g_bt;

// Called from BT stack task when it needs more audio data.
// MUST be fast — no heap allocation, no SD I/O here.
int32_t BTAudio::audioCallback(Frame* frame, int32_t count) {
    g_synth.fillBuffer(reinterpret_cast<AudioFrame*>(frame), count);
    return count;
}

void BTAudio::begin(const char* device_name) {
    a2dp.set_auto_reconnect(true);

    if (device_name && strlen(device_name) > 0) {
        // Try to connect to the named device
        a2dp.start(device_name, audioCallback);
    } else {
        // Auto-reconnect to last bonded device (no name required)
        a2dp.start("Kamerton", audioCallback);
    }
}

bool BTAudio::isConnected() const {
    return const_cast<BluetoothA2DPSource&>(a2dp).is_connected();
}

void BTAudio::startScan(esp_bt_gap_cb_t found_cb) {
    esp_bt_gap_register_callback(found_cb);
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
}

void BTAudio::stopScan() {
    esp_bt_gap_cancel_discovery();
}

void BTAudio::connectTo(esp_bd_addr_t addr) {
    // Currently unused — the address-based pairing UI is not wired up anywhere.
    // The ESP32-A2DP API for connecting by BD address changed across library
    // versions (older 'connect()' was removed), so this is left as a no-op to
    // keep the build clean.  Pairing is done in begin() via device name +
    // auto-reconnect.
    (void)addr;
}
