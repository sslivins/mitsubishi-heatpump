/// @file cn105.cpp
/// @brief CN105 protocol engine — PORT STUB.
///
/// The public interface in cn105.h is final. The actual packet engine still
/// needs porting from SwiCago/HeatPump. Everything below installs the UART and
/// provides safe no-op behaviour so the project builds and links today; the
/// TODO blocks mark exactly what to fill in once hardware is on the bench.

#include "cn105.h"

#include <cstring>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace cn105 {

static const char* TAG = "cn105";

// --- Protocol constants (from SwiCago/HeatPump) -----------------------------
// Frames: [0xFC][type][0x01][0x30][len][payload...][checksum]
// type: 0x41 set, 0x42 get-request, 0x5A connect, 0x61 get-response, 0x62 status
static constexpr uint8_t kHeaderStart = 0xFC;
static constexpr int     kBaud        = 2400;
// TODO(port): bring over INFOMODE table, mode/fan/vane lookup arrays, the
// connect packet, the 0x5A handshake, and checksum (0xFC - sum(bytes) & 0xFF).

esp_err_t HeatPump::connect(uart_port_t port, int tx_gpio, int rx_gpio) {
    port_ = port;

    const uart_config_t cfg = {
        .baud_rate = kBaud,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_EVEN,   // Mitsubishi CN105 is 8-E-1
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(port_, 256, 256, 0, nullptr, 0);
    if (err != ESP_OK) return err;
    err = uart_param_config(port_, &cfg);
    if (err != ESP_OK) return err;
    err = uart_set_pin(port_, tx_gpio, rx_gpio,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;

    ESP_LOGW(TAG, "UART up @%d 8-E-1 (tx=%d rx=%d) — packet engine is a STUB",
             kBaud, tx_gpio, rx_gpio);
    // TODO(port): send the connect handshake (0xFC 0x5A ...) and wait for ACK,
    // then set settings_.connected = true on success.
    //
    // Until the packet engine lands, seed believable placeholder values so the
    // web UI and Home Assistant entities show something sensible. `connected`
    // stays false so callers can tell real telemetry from these defaults.
    settings_ = Settings{};
    settings_.power       = "OFF";
    settings_.mode        = "HEAT";
    settings_.temperature = 21.0f;
    settings_.fan         = "AUTO";
    settings_.vane        = "AUTO";
    settings_.wideVane    = "|";
    settings_.connected   = false;
    status_ = Status{};
    status_.roomTemperature     = 21.0f;
    status_.operating           = false;
    status_.compressorFrequency = 0;
    return ESP_OK;
}

void HeatPump::update() {
    // TODO(port): read available bytes, assemble 0xFC frames, validate the
    // checksum, decode get-responses (0x61) into settings_ and status packets
    // (0x62) into status_, invoke on_packet_/on_settings_/on_status_, and (when
    // auto_update_) transmit any queued set packets (0x41). For now: drain RX so
    // the buffer never overflows and hand raw bytes to the packet callback.
    uint8_t buf[64];
    int n = uart_read_bytes(port_, buf, sizeof(buf), 0);
    if (n > 0 && on_packet_) on_packet_(buf, static_cast<size_t>(n), "RX");
}

void HeatPump::sync() {
    // TODO(port): emit get-requests for settings (0x02), room temp (0x03),
    // and status (0x06).
    ESP_LOGD(TAG, "sync() — stub");
}

void HeatPump::setPowerSetting(const std::string& power)        { settings_.power = power; }
void HeatPump::setModeSetting(const std::string& mode)          { settings_.mode = mode; }
void HeatPump::setTemperature(float celsius)                    { settings_.temperature = celsius; }
void HeatPump::setFanSpeed(const std::string& fan)              { settings_.fan = fan; }
void HeatPump::setVaneSetting(const std::string& vane)          { settings_.vane = vane; }
void HeatPump::setWideVaneSetting(const std::string& wideVane)  { settings_.wideVane = wideVane; }

void HeatPump::setRemoteTemperature(float celsius) {
    // TODO(port): build and queue the remote-temperature set packet (0x07).
    ESP_LOGD(TAG, "setRemoteTemperature(%.1f) — stub", celsius);
}

}  // namespace cn105
