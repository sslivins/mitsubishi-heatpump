/// @file cn105.cpp
/// @brief CN105 protocol engine — ESP-IDF/C++ port of SwiCago/HeatPump.
///
/// Wire protocol: 2400 baud 8-E-1, 0xFC-framed packets
///   [0xFC][type][0x01][0x30][len][payload...][checksum]
///   checksum = (0xFC - sum(all preceding bytes)) & 0xFF
///
/// Threading model (this differs from the single-threaded Arduino reference):
///   * update() runs in ONE dedicated task ("cn105_task"). That task is the
///     sole owner of the UART, current_/status_ and all the pump-private flags,
///     so those need no locking.
///   * Setters and accessors are called from OTHER tasks. wanted_/dirty state is
///     guarded by want_mtx_; the published snapshot read by accessors is guarded
///     by pub_mtx_. The two mutexes are never held nested, and callbacks are
///     always fired while holding neither — so there is no deadlock path.
///   * All UART writes happen only in the pump task; setRemoteTemperature() and
///     sync() just record deferred work that the pump performs on its next tick.

#include "cn105.h"

#include <cmath>
#include <cstring>
#include <strings.h>  // strcasecmp

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace cn105 {

static const char* TAG = "cn105";

// --- Protocol constants (from SwiCago/HeatPump) -----------------------------
static constexpr int      kBaud                 = 2400;
static constexpr int      PACKET_LEN            = 22;
static constexpr int      INFOHEADER_LEN        = 5;
static constexpr int      INFOMODE_LEN          = 6;
static constexpr uint32_t PACKET_SENT_INTERVAL_MS = 1000;
static constexpr uint32_t PACKET_INFO_INTERVAL_MS = 2000;
static constexpr uint32_t RECV_TIMEOUT_MS       = 10000;   // reconnect if silent this long
static constexpr uint32_t CONNECT_BACKOFF_MIN_MS = 1000;   // first reconnect interval
static constexpr uint32_t CONNECT_BACKOFF_MAX_MS = 15000;  // cap on the reconnect interval
static constexpr uint32_t AUTOUPDATE_GRACE_MS   = 30000;   // ignore external changes after a local one
static constexpr int      PACKET_TYPE_DEFAULT   = 99;      // "cycle infoMode" sentinel
static constexpr int      RQST_PKT_SETTINGS     = 0;       // index into INFOMODE

enum {
    RCVD_PKT_FAIL            = 0,
    RCVD_PKT_CONNECT_SUCCESS = 1,
    RCVD_PKT_SETTINGS        = 2,
    RCVD_PKT_ROOM_TEMP       = 3,
    RCVD_PKT_UPDATE_SUCCESS  = 4,
    RCVD_PKT_STATUS          = 5,
    RCVD_PKT_TIMER           = 6,
};

static const uint8_t CONNECT[8]         = {0xfc, 0x5a, 0x01, 0x30, 0x02, 0xca, 0x01, 0xa8};
static const uint8_t HEADER_SET[8]      = {0xfc, 0x41, 0x01, 0x30, 0x10, 0x01, 0x00, 0x00};
static const uint8_t INFOHEADER[5]      = {0xfc, 0x42, 0x01, 0x30, 0x10};
static const uint8_t INFOMODE[6]        = {0x02, 0x03, 0x06, 0x04, 0x05, 0x09};
static const uint8_t CONTROL_PACKET_1[5] = {0x01, 0x02, 0x04, 0x08, 0x10};  // power,mode,temp,fan,vane
static const uint8_t CONTROL_PACKET_2[1] = {0x01};                          // wideVane

static const uint8_t POWER_B[2]    = {0x00, 0x01};
static const char*   POWER_MAP[2]  = {"OFF", "ON"};
static const uint8_t MODE_B[5]     = {0x01, 0x02, 0x03, 0x07, 0x08};
static const char*   MODE_MAP[5]   = {"HEAT", "DRY", "COOL", "FAN", "AUTO"};
static const uint8_t TEMP_B[16]    = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                      0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
static const int     TEMP_MAP[16]  = {31, 30, 29, 28, 27, 26, 25, 24,
                                      23, 22, 21, 20, 19, 18, 17, 16};
static const uint8_t FAN_B[6]      = {0x00, 0x01, 0x02, 0x03, 0x05, 0x06};
static const char*   FAN_MAP[6]    = {"AUTO", "QUIET", "1", "2", "3", "4"};
static const uint8_t VANE_B[7]     = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x07};
static const char*   VANE_MAP[7]   = {"AUTO", "1", "2", "3", "4", "5", "SWING"};
static const uint8_t WIDEVANE_B[7] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x08, 0x0c};
static const char*   WIDEVANE_MAP[7] = {"<<", "<", "|", ">", ">>", "<>", "SWING"};
static const uint8_t ROOM_TEMP_B[32] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
                                        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
static const int     ROOM_TEMP_MAP[32] = {10, 11, 12, 13, 14, 15, 16, 17,
                                          18, 19, 20, 21, 22, 23, 24, 25,
                                          26, 27, 28, 29, 30, 31, 32, 33,
                                          34, 35, 36, 37, 38, 39, 40, 41};

// Extended-telemetry maps (0x09 power/standby reply). Additive decode only;
// these bytes come from an info mode we already poll (see INFOMODE) but did not
// previously decode. Values/formulas per echavet/MitsubishiCN105ESPHome.
static const uint8_t SUB_MODE_B[6]   = {0x00, 0x01, 0x02, 0x04, 0x08, 0x10};
static const char*   SUB_MODE_MAP[6] = {"NORMAL", "WARMUP", "DEFROST",
                                        "PREHEAT", "STANDBY", "OFF"};
static const uint8_t STAGE_B[7]      = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
static const char*   STAGE_MAP[7]    = {"IDLE", "LOW", "GENTLE", "MEDIUM",
                                        "MODERATE", "HIGH", "DIFFUSE"};

// --- small helpers ----------------------------------------------------------
static inline uint32_t now_ms() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
}

static int lookupIdxStr(const char* const map[], int len, const std::string& v) {
    for (int i = 0; i < len; i++) {
        if (strcasecmp(map[i], v.c_str()) == 0) return i;
    }
    return -1;
}
static const char* lookupValStr(const char* const map[], const uint8_t bmap[],
                                int len, uint8_t b) {
    for (int i = 0; i < len; i++) {
        if (bmap[i] == b) return map[i];
    }
    return map[0];
}
static int lookupValInt(const int map[], const uint8_t bmap[], int len, uint8_t b) {
    for (int i = 0; i < len; i++) {
        if (bmap[i] == b) return map[i];
    }
    return map[0];
}
static int lookupIdxInt(const int map[], int len, int v) {
    for (int i = 0; i < len; i++) {
        if (map[i] == v) return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------

HeatPump::HeatPump() {
    // Created eagerly (before the scheduler starts is fine) so setters called
    // from other tasks always have a valid mutex.
    want_mtx_ = xSemaphoreCreateMutex();
    pub_mtx_  = xSemaphoreCreateMutex();
}

esp_err_t HeatPump::connect(uart_port_t port, int tx_gpio, int rx_gpio) {
    port_ = port;
    tx_gpio_ = tx_gpio;
    rx_gpio_ = rx_gpio;
    if (!want_mtx_) want_mtx_ = xSemaphoreCreateMutex();
    if (!pub_mtx_)  pub_mtx_  = xSemaphoreCreateMutex();

    const uart_config_t cfg = {
        .baud_rate  = kBaud,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_EVEN,   // CN105 is 8-E-1
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags      = {},
    };

    esp_err_t err = uart_driver_install(port_, 256, 256, 0, nullptr, 0);
    if (err != ESP_OK) return err;
    err = uart_param_config(port_, &cfg);
    if (err != ESP_OK) return err;
    err = uart_set_pin(port_, tx_gpio, rx_gpio, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;

    // Seed believable placeholders so the UI/HA entities show something until
    // real telemetry arrives. connected_ stays false so callers can distinguish
    // these defaults from live data.
    current_ = Settings{};
    current_.power       = "OFF";
    current_.mode        = "HEAT";
    current_.temperature = 21.0f;
    current_.fan         = "AUTO";
    current_.vane        = "AUTO";
    current_.wideVane    = "|";
    current_.connected   = false;
    status_ = Status{};
    status_.roomTemperature     = 21.0f;
    status_.operating           = false;
    status_.compressorFrequency = 0;

    xSemaphoreTake(want_mtx_, portMAX_DELAY);
    wanted_ = current_;
    xSemaphoreGive(want_mtx_);

    connected_ = false;
    firstRun_  = true;
    infoMode_  = 0;
    lastSend_  = 0;
    lastRecv_  = now_ms() - (RECV_TIMEOUT_MS + 1000);  // allow an immediate connect

    ESP_LOGI(TAG, "CN105 UART up @%d 8-E-1 (tx=%d rx=%d)", kBaud, tx_gpio, rx_gpio);

    vTaskDelay(pdMS_TO_TICKS(1000));  // let the line settle before the handshake
    connectBackoff_ = CONNECT_BACKOFF_MIN_MS;
    lastConnectAttempt_ = now_ms();
    sendConnect_();                   // one attempt; pump() keeps retrying (backed off)
    publishSnapshot_();
    return ESP_OK;
}

// The public pump entry point. Runs entirely in cn105_task.
void HeatPump::update() {
    pump_();
}

void HeatPump::sync() {
    sync_requested_.store(true);  // serviced by the pump on its next info tick
}

void HeatPump::pump_() {
    settingsChanged_ = false;
    statusChanged_   = false;
    uint32_t now = now_ms();

    // Detect a dropped link (unit unplugged / powered off) so the UI and HA
    // reflect it, and re-adopt the unit's state cleanly once it returns.
    if (connected_ && (now - lastRecv_ > RECV_TIMEOUT_MS)) {
        connected_ = false;
        firstRun_  = true;
        settingsChanged_ = true;  // notify listeners the unit went away
        ESP_LOGW(TAG, "CN105 link lost (no packet for %ums)", (unsigned)RECV_TIMEOUT_MS);
    }

    // (Re)connect with exponential backoff while disconnected. Backoff grows on
    // each failed handshake and is reset to the minimum the moment any valid
    // packet arrives (see readPacket_), so a real reconnect stays snappy while
    // an absent/dead unit doesn't get hammered once per second forever.
    if (!connected_) {
        if (now - lastConnectAttempt_ >= connectBackoff_) {
            lastConnectAttempt_ = now;
            sendConnect_();
            if (!connected_) {
                connectBackoff_ = connectBackoff_ * 2;
                if (connectBackoff_ > CONNECT_BACKOFF_MAX_MS)
                    connectBackoff_ = CONNECT_BACKOFF_MAX_MS;
            }
        }
        publishSnapshot_();
        fireCallbacks_();
        return;
    }

    // Absorb any unsolicited packets (bounded so this can't starve sends).
    drainPackets_(4);

    // Flush wanted settings if they differ from what the unit reports.
    Settings wsnap;
    xSemaphoreTake(want_mtx_, portMAX_DELAY);
    wsnap = wanted_;
    xSemaphoreGive(want_mtx_);

    bool differs = (wsnap.power != current_.power) ||
                   (wsnap.mode != current_.mode) ||
                   (wsnap.temperature != current_.temperature) ||
                   (wsnap.fan != current_.fan) ||
                   (wsnap.vane != current_.vane) ||
                   (wsnap.wideVane != current_.wideVane);

    if (autoUpdate_ && !firstRun_ && differs && canSend_(false)) {
        flushWanted_(wsnap);
        publishSnapshot_();
        fireCallbacks_();
        return;
    }

    // Deferred remote-temperature write (only clears the flag once we send).
    if (canSend_(false)) {
        bool doRemote = false;
        float rt = 0.0f;
        xSemaphoreTake(want_mtx_, portMAX_DELAY);
        if (pendingRemoteTempSet_) {
            doRemote = true;
            rt = pendingRemoteTemp_;
            pendingRemoteTempSet_ = false;
        }
        xSemaphoreGive(want_mtx_);
        if (doRemote) {
            sendRemoteTemp_(rt);
            publishSnapshot_();
            fireCallbacks_();
            return;
        }
    }

    // Otherwise poll the unit for the next info packet.
    if (canSend_(true)) {
        int packetType = sync_requested_.exchange(false) ? RQST_PKT_SETTINGS
                                                         : PACKET_TYPE_DEFAULT;
        sendInfoPacket_(packetType);
    }

    publishSnapshot_();
    fireCallbacks_();
}

bool HeatPump::canSend_(bool isInfo) const {
    uint32_t interval = isInfo ? PACKET_INFO_INTERVAL_MS : PACKET_SENT_INTERVAL_MS;
    return (now_ms() - lastSend_) > interval;
}

void HeatPump::sendConnect_() {
    uint8_t pkt[8];
    memcpy(pkt, CONNECT, sizeof(pkt));
    writePacket_(pkt, sizeof(pkt));

    uint32_t t0 = now_ms();
    int r = RCVD_PKT_FAIL;
    do {
        r = readPacket_(300);
    } while (r != RCVD_PKT_CONNECT_SUCCESS && (now_ms() - t0) < 1000);

    if (r == RCVD_PKT_CONNECT_SUCCESS) {
        connected_ = true;
        ESP_LOGI(TAG, "CN105 connected");
    }
}

void HeatPump::flushWanted_(const Settings& wanted) {
    drainPackets_(4);  // clear stale replies so we can see our ACK

    uint8_t pkt[PACKET_LEN] = {};
    createSetPacket_(pkt, wanted);
    writePacket_(pkt, PACKET_LEN);

    uint32_t t0 = now_ms();
    int r = RCVD_PKT_FAIL;
    do {
        r = readPacket_(300);
    } while (r != RCVD_PKT_UPDATE_SUCCESS && (now_ms() - t0) < 1000);

    if (r == RCVD_PKT_UPDATE_SUCCESS) {
        // The unit accepted our command: the user's intent is delivered, so the
        // dirty flags can clear (external changes may now be adopted again).
        xSemaphoreTake(want_mtx_, portMAX_DELAY);
        d_power_ = d_mode_ = d_temp_ = d_fan_ = d_vane_ = d_wideVane_ = false;
        xSemaphoreGive(want_mtx_);

        // Optimistically reflect the accepted values into current_ so `differs`
        // clears right away. Otherwise pump_ re-enters this flush every cycle:
        // current_ is only refreshed by an INFO poll, but the early-return in
        // pump_ after a flush starves INFO polling while any set is pending.
        // The result was a permanent SET storm (same packet every ~1s) with the
        // UI/MQTT frozen on the pre-change state even though the unit had already
        // applied the change. The next INFO tick still re-reads to confirm.
        bool changed = (current_.power != wanted.power) ||
                       (current_.mode != wanted.mode) ||
                       (current_.temperature != wanted.temperature) ||
                       (current_.fan != wanted.fan) ||
                       (current_.vane != wanted.vane) ||
                       (current_.wideVane != wanted.wideVane);
        current_.power       = wanted.power;
        current_.mode        = wanted.mode;
        current_.temperature = wanted.temperature;
        current_.fan         = wanted.fan;
        current_.vane        = wanted.vane;
        current_.wideVane    = wanted.wideVane;
        if (changed) settingsChanged_ = true;  // push the new state to UI + MQTT
        infoMode_ = 0;  // re-read settings on the next info tick to confirm
    }
}

void HeatPump::sendInfoPacket_(int packetType) {
    uint8_t pkt[PACKET_LEN] = {};
    createInfoPacket_(pkt, packetType);
    writePacket_(pkt, PACKET_LEN);

    uint32_t t0 = now_ms();
    int r = RCVD_PKT_FAIL;
    do {
        r = readPacket_(300);
    } while (r == RCVD_PKT_FAIL && (now_ms() - t0) < 600);
}

void HeatPump::sendRemoteTemp_(float setting) {
    uint8_t pkt[PACKET_LEN] = {};
    memcpy(pkt, HEADER_SET, sizeof(HEADER_SET));
    pkt[5] = 0x07;
    if (setting > 0) {
        pkt[6] = 0x01;
        setting = roundf(setting * 2.0f) / 2.0f;  // snap to 0.5C
        float temp1 = 3 + ((setting - 10) * 2);
        pkt[7] = static_cast<uint8_t>(static_cast<int>(temp1));
        float temp2 = (setting * 2) + 128;
        pkt[8] = static_cast<uint8_t>(static_cast<int>(temp2));
    } else {
        pkt[6] = 0x00;
        pkt[8] = 0x80;  // MHK1 sends 0x80 even though control byte is 0x00
    }
    pkt[21] = checkSum_(pkt, 21);
    writePacket_(pkt, PACKET_LEN);  // fire-and-forget
}

void HeatPump::drainPackets_(int maxPackets) {
    while (maxPackets-- > 0 && available_() > 0) {
        if (readPacket_(5) == RCVD_PKT_FAIL) break;
    }
}

int HeatPump::readByte_(uint32_t to_ms) {
    uint8_t b;
    int r = uart_read_bytes(port_, &b, 1, pdMS_TO_TICKS(to_ms));
    return (r == 1) ? static_cast<int>(b) : -1;
}

int HeatPump::available_() {
    size_t n = 0;
    uart_get_buffered_data_len(port_, &n);
    return static_cast<int>(n);
}

// Parse exactly one frame. Bounded: every read has a timeout and the length is
// validated before we read the payload, so a garbled line can never hang us.
int HeatPump::readPacket_(uint32_t start_to_ms) {
    int b;
    // Scan for the 0xFC start byte.
    uint32_t to = start_to_ms;
    for (;;) {
        b = readByte_(to);
        if (b < 0) return RCVD_PKT_FAIL;
        if (b == 0xFC) break;
        to = 20;  // once we're mid-stream, remaining bytes are already buffered
    }

    uint8_t header[INFOHEADER_LEN];
    header[0] = 0xFC;
    for (int i = 1; i < INFOHEADER_LEN; i++) {
        b = readByte_(100);
        if (b < 0) return RCVD_PKT_FAIL;
        header[i] = static_cast<uint8_t>(b);
    }
    if (header[2] != 0x01 || header[3] != 0x30) return RCVD_PKT_FAIL;

    uint8_t dataLength = header[4];
    if (dataLength > 32) return RCVD_PKT_FAIL;  // frame-length sanity check

    uint8_t data[33] = {};
    for (int i = 0; i < dataLength; i++) {
        b = readByte_(100);
        if (b < 0) return RCVD_PKT_FAIL;
        data[i] = static_cast<uint8_t>(b);
    }
    b = readByte_(100);  // checksum byte
    if (b < 0) return RCVD_PKT_FAIL;
    data[dataLength] = static_cast<uint8_t>(b);

    int dataSum = 0;
    for (int i = 0; i < INFOHEADER_LEN; i++) dataSum += header[i];
    for (int i = 0; i < dataLength; i++) dataSum += data[i];
    uint8_t checksum = static_cast<uint8_t>((0xFC - dataSum) & 0xFF);
    if (data[dataLength] != checksum) return RCVD_PKT_FAIL;

    lastRecv_ = now_ms();
    connectBackoff_ = CONNECT_BACKOFF_MIN_MS;  // valid traffic: recover fast next time

    // No lock is held here, so callbacks are safe to invoke directly.
    if (on_packet_) {
        uint8_t whole[38];
        int wl = 0;
        for (int i = 0; i < INFOHEADER_LEN; i++) whole[wl++] = header[i];
        for (int i = 0; i < dataLength + 1; i++) whole[wl++] = data[i];
        on_packet_(whole, static_cast<size_t>(wl), "RX");
    }

    uint8_t type = header[1];
    if (type == 0x62) {
        return decodeInfo_(data, dataLength);
    } else if (type == 0x61) {   // SET acknowledged
        return RCVD_PKT_UPDATE_SUCCESS;
    } else if (type == 0x7a) {   // CONNECT acknowledged
        connected_ = true;
        return RCVD_PKT_CONNECT_SUCCESS;
    }
    return RCVD_PKT_FAIL;
}

int HeatPump::decodeInfo_(const uint8_t* data, int len) {
    switch (data[0]) {
        case 0x02: {  // settings
            Settings rs = current_;
            rs.power = lookupValStr(POWER_MAP, POWER_B, 2, data[3]);
            bool iSee = data[4] > 0x08;  // iSee sensor present -> mode byte offset
            uint8_t modeByte = iSee ? static_cast<uint8_t>(data[4] - 0x08) : data[4];
            rs.mode = lookupValStr(MODE_MAP, MODE_B, 5, modeByte);

            if (data[11] != 0x00) {  // half-degree ("tempMode") encoding
                int t = static_cast<int>(data[11]) - 128;
                rs.temperature = static_cast<float>(t) / 2.0f;
                tempMode_ = true;
            } else {
                rs.temperature = static_cast<float>(
                    lookupValInt(TEMP_MAP, TEMP_B, 16, data[5]));
            }
            rs.fan      = lookupValStr(FAN_MAP, FAN_B, 6, data[6]);
            rs.vane     = lookupValStr(VANE_MAP, VANE_B, 7, data[7]);
            rs.wideVane = lookupValStr(WIDEVANE_MAP, WIDEVANE_B, 7,
                                       static_cast<uint8_t>(data[10] & 0x0F));
            wideVaneAdj_ = ((data[10] & 0xF0) == 0x80);

            // Extended telemetry: target humidity (data[12], 1..100 %). Additive
            // — decoded from the same 0x02 reply; not part of the SET contract.
            if (len >= 13 && data[12] >= 1 && data[12] <= 100) {
                int th = static_cast<int>(data[12]);
                if (!status_.has_targetHumidity || th != status_.targetHumidity) {
                    status_.targetHumidity = th;
                    status_.has_targetHumidity = true;
                    statusChanged_ = true;
                }
            }

            bool changed = (rs.power != current_.power) ||
                           (rs.mode != current_.mode) ||
                           (rs.temperature != current_.temperature) ||
                           (rs.fan != current_.fan) ||
                           (rs.vane != current_.vane) ||
                           (rs.wideVane != current_.wideVane);
            current_.power = rs.power;
            current_.mode = rs.mode;
            current_.temperature = rs.temperature;
            current_.fan = rs.fan;
            current_.vane = rs.vane;
            current_.wideVane = rs.wideVane;
            if (changed) settingsChanged_ = true;

            // Adopt the unit's state into wanted_ on first sync (so we don't
            // immediately fight it) or after the external-update grace period.
            // Per-field: never clobber a field the user changed but we haven't
            // yet delivered (its dirty flag is still set).
            uint32_t now = now_ms();
            bool graceElapsed = autoUpdate_ && externalUpdate_ &&
                                (now - lastWanted_ > AUTOUPDATE_GRACE_MS);
            if (firstRun_ || graceElapsed) {
                xSemaphoreTake(want_mtx_, portMAX_DELAY);
                if (!d_power_)    wanted_.power = current_.power;
                if (!d_mode_)     wanted_.mode = current_.mode;
                if (!d_temp_)     wanted_.temperature = current_.temperature;
                if (!d_fan_)      wanted_.fan = current_.fan;
                if (!d_vane_)     wanted_.vane = current_.vane;
                if (!d_wideVane_) wanted_.wideVane = current_.wideVane;
                xSemaphoreGive(want_mtx_);
                firstRun_ = false;
            }
            return RCVD_PKT_SETTINGS;
        }

        case 0x03: {  // room temperature (the unit's own sensor)
            float rt;
            if (data[6] != 0x00) {
                int t = static_cast<int>(data[6]) - 128;
                rt = static_cast<float>(t) / 2.0f;
            } else {
                rt = static_cast<float>(
                    lookupValInt(ROOM_TEMP_MAP, ROOM_TEMP_B, 32, data[3]));
            }
            if (rt != status_.roomTemperature) {
                status_.roomTemperature = rt;
                statusChanged_ = true;
            }

            // Extended telemetry from the same 0x03 reply:
            //   outside air/coil temp: data[5] (>1) -> (data[5]-128)/2 °C
            //   cumulative runtime:    (data[11]<<16|data[12]<<8|data[13]) / 60 h
            if (len >= 6 && data[5] > 1) {
                float ot = (static_cast<float>(data[5]) - 128.0f) / 2.0f;
                if (!status_.has_outsideTemp || ot != status_.outsideTemp) {
                    status_.outsideTemp = ot;
                    status_.has_outsideTemp = true;
                    statusChanged_ = true;
                }
            }
            if (len >= 14) {
                uint32_t rawMin = (static_cast<uint32_t>(data[11]) << 16) |
                                  (static_cast<uint32_t>(data[12]) << 8) |
                                  static_cast<uint32_t>(data[13]);
                float rh = static_cast<float>(rawMin) / 60.0f;
                if (!status_.has_runtimeHours || rh != status_.runtimeHours) {
                    status_.runtimeHours = rh;
                    status_.has_runtimeHours = true;
                    statusChanged_ = true;
                }
            }
            return RCVD_PKT_ROOM_TEMP;
        }

        case 0x06: {  // operating status + compressor frequency
            bool op = data[4] != 0;
            int cf = data[3];
            if (op != status_.operating || cf != status_.compressorFrequency) {
                status_.operating = op;
                status_.compressorFrequency = cf;
                statusChanged_ = true;
            }
            // Extended telemetry from the same 0x06 reply:
            //   input power:  (data[5]<<8|data[6]) W
            //   energy usage: (data[7]<<8|data[8]) / 10 kWh
            if (len >= 7) {
                int ip = (static_cast<int>(data[5]) << 8) | static_cast<int>(data[6]);
                if (!status_.has_inputPowerW || ip != status_.inputPowerW) {
                    status_.inputPowerW = ip;
                    status_.has_inputPowerW = true;
                    statusChanged_ = true;
                }
            }
            if (len >= 9) {
                float ek = static_cast<float>(
                               (static_cast<int>(data[7]) << 8) |
                               static_cast<int>(data[8])) / 10.0f;
                if (!status_.has_energyKwh || ek != status_.energyKwh) {
                    status_.energyKwh = ek;
                    status_.has_energyKwh = true;
                    statusChanged_ = true;
                }
            }
            return RCVD_PKT_STATUS;
        }

        case 0x09: {  // power/standby: sub-mode + fan stage (additive telemetry)
            if (len >= 5) {
                std::string sm;
                for (int i = 0; i < 6; i++)
                    if (SUB_MODE_B[i] == data[3]) { sm = SUB_MODE_MAP[i]; break; }
                std::string st;
                for (int i = 0; i < 7; i++)
                    if (STAGE_B[i] == data[4]) { st = STAGE_MAP[i]; break; }
                if (!sm.empty() && sm != status_.subMode) {
                    status_.subMode = sm;
                    statusChanged_ = true;
                }
                if (!st.empty() && st != status_.stage) {
                    status_.stage = st;
                    statusChanged_ = true;
                }
            }
            return RCVD_PKT_STATUS;
        }

        case 0x04: {  // error info (additive telemetry)
            if (len >= 6) {
                uint8_t ecode = data[4] & 0x7F;  // bit7 is a status flag, not error
                uint8_t esub  = data[5];
                char buf[32];
                if (ecode == 0x00 && esub == 0x00) {
                    snprintf(buf, sizeof(buf), "No Error");
                } else {
                    snprintf(buf, sizeof(buf), "Error 0x%02X sub 0x%02X", ecode, esub);
                }
                if (status_.errorCode != buf) {
                    status_.errorCode = buf;
                    statusChanged_ = true;
                }
            }
            return RCVD_PKT_STATUS;
        }

        case 0x05:  // timers — parsed-and-ignored (not surfaced yet)
            return RCVD_PKT_TIMER;

        default:
            return RCVD_PKT_FAIL;
    }
}

void HeatPump::writePacket_(const uint8_t* packet, int len) {
    uart_write_bytes(port_, packet, len);
    if (on_packet_) on_packet_(packet, static_cast<size_t>(len), "TX");
    lastSend_ = now_ms();
}

uint8_t HeatPump::checkSum_(const uint8_t* bytes, int len) {
    uint8_t sum = 0;
    for (int i = 0; i < len; i++) sum += bytes[i];
    return static_cast<uint8_t>((0xFC - sum) & 0xFF);
}

void HeatPump::createSetPacket_(uint8_t* packet, const Settings& w) {
    memset(packet, 0, PACKET_LEN);
    memcpy(packet, HEADER_SET, sizeof(HEADER_SET));

    if (w.power != current_.power) {
        int i = lookupIdxStr(POWER_MAP, 2, w.power);
        if (i >= 0) { packet[8] = POWER_B[i]; packet[6] += CONTROL_PACKET_1[0]; }
    }
    if (w.mode != current_.mode) {
        int i = lookupIdxStr(MODE_MAP, 5, w.mode);
        if (i >= 0) { packet[9] = MODE_B[i]; packet[6] += CONTROL_PACKET_1[1]; }
    }
    if (!tempMode_ && w.temperature != current_.temperature) {
        int i = lookupIdxInt(TEMP_MAP, 16, static_cast<int>(w.temperature + 0.5f));
        if (i >= 0) { packet[10] = TEMP_B[i]; packet[6] += CONTROL_PACKET_1[2]; }
    } else if (tempMode_ && w.temperature != current_.temperature) {
        float temp = (w.temperature * 2) + 128;
        packet[19] = static_cast<uint8_t>(static_cast<int>(temp));
        packet[6] += CONTROL_PACKET_1[2];
    }
    if (w.fan != current_.fan) {
        int i = lookupIdxStr(FAN_MAP, 6, w.fan);
        if (i >= 0) { packet[11] = FAN_B[i]; packet[6] += CONTROL_PACKET_1[3]; }
    }
    if (w.vane != current_.vane) {
        int i = lookupIdxStr(VANE_MAP, 7, w.vane);
        if (i >= 0) { packet[12] = VANE_B[i]; packet[6] += CONTROL_PACKET_1[4]; }
    }
    if (w.wideVane != current_.wideVane) {
        int i = lookupIdxStr(WIDEVANE_MAP, 7, w.wideVane);
        if (i >= 0) {
            packet[18] = static_cast<uint8_t>(WIDEVANE_B[i] | (wideVaneAdj_ ? 0x80 : 0x00));
            packet[7] += CONTROL_PACKET_2[0];
        }
    }
    packet[21] = checkSum_(packet, 21);
}

void HeatPump::createInfoPacket_(uint8_t* packet, int packetType) {
    memset(packet, 0, PACKET_LEN);
    for (int i = 0; i < INFOHEADER_LEN; i++) packet[i] = INFOHEADER[i];

    if (packetType != PACKET_TYPE_DEFAULT) {
        int idx = (packetType >= 0 && packetType < INFOMODE_LEN) ? packetType : 0;
        packet[5] = INFOMODE[idx];
    } else {
        packet[5] = INFOMODE[infoMode_];
        infoMode_ = (infoMode_ == (INFOMODE_LEN - 1)) ? 0 : (infoMode_ + 1);
    }
    packet[21] = checkSum_(packet, 21);
}

void HeatPump::publishSnapshot_() {
    current_.connected = connected_;
    xSemaphoreTake(pub_mtx_, portMAX_DELAY);
    pub_settings_  = current_;
    pub_status_    = status_;
    pub_connected_ = connected_;
    xSemaphoreGive(pub_mtx_);
}

void HeatPump::fireCallbacks_() {
    // Called with no mutex held, so callbacks may freely call back into us.
    if (settingsChanged_ && on_settings_) on_settings_(current_);
    if (statusChanged_ && on_status_) on_status_(status_);
    settingsChanged_ = false;
    statusChanged_ = false;
}

// --- cross-task accessors (return a published snapshot) ---------------------

Settings HeatPump::getSettings() const {
    if (!pub_mtx_) return current_;
    xSemaphoreTake(pub_mtx_, portMAX_DELAY);
    Settings s = pub_settings_;
    xSemaphoreGive(pub_mtx_);
    return s;
}

Status HeatPump::getStatus() const {
    if (!pub_mtx_) return status_;
    xSemaphoreTake(pub_mtx_, portMAX_DELAY);
    Status s = pub_status_;
    xSemaphoreGive(pub_mtx_);
    return s;
}

bool HeatPump::isConnected() const {
    if (!pub_mtx_) return connected_;
    xSemaphoreTake(pub_mtx_, portMAX_DELAY);
    bool c = pub_connected_;
    xSemaphoreGive(pub_mtx_);
    return c;
}

// --- setters (record intent under want_mtx_; the pump delivers it) ----------

void HeatPump::setPowerSetting(const std::string& power) {
    int i = lookupIdxStr(POWER_MAP, 2, power);
    std::string nv = (i >= 0) ? POWER_MAP[i] : POWER_MAP[0];
    xSemaphoreTake(want_mtx_, portMAX_DELAY);
    wanted_.power = nv;
    d_power_ = true;
    lastWanted_ = now_ms();
    xSemaphoreGive(want_mtx_);
}

void HeatPump::setModeSetting(const std::string& mode) {
    int i = lookupIdxStr(MODE_MAP, 5, mode);
    std::string nv = (i >= 0) ? MODE_MAP[i] : MODE_MAP[0];
    xSemaphoreTake(want_mtx_, portMAX_DELAY);
    wanted_.mode = nv;
    d_mode_ = true;
    lastWanted_ = now_ms();
    xSemaphoreGive(want_mtx_);
}

void HeatPump::setTemperature(float celsius) {
    float nv;
    if (!tempMode_) {
        nv = (lookupIdxInt(TEMP_MAP, 16, static_cast<int>(celsius + 0.5f)) > -1)
                 ? celsius
                 : static_cast<float>(TEMP_MAP[0]);
    } else {
        float s = roundf(celsius * 2.0f) / 2.0f;
        nv = s < 10 ? 10 : (s > 31 ? 31 : s);
    }
    xSemaphoreTake(want_mtx_, portMAX_DELAY);
    wanted_.temperature = nv;
    d_temp_ = true;
    lastWanted_ = now_ms();
    xSemaphoreGive(want_mtx_);
}

void HeatPump::setFanSpeed(const std::string& fan) {
    int i = lookupIdxStr(FAN_MAP, 6, fan);
    std::string nv = (i >= 0) ? FAN_MAP[i] : FAN_MAP[0];
    xSemaphoreTake(want_mtx_, portMAX_DELAY);
    wanted_.fan = nv;
    d_fan_ = true;
    lastWanted_ = now_ms();
    xSemaphoreGive(want_mtx_);
}

void HeatPump::setVaneSetting(const std::string& vane) {
    int i = lookupIdxStr(VANE_MAP, 7, vane);
    std::string nv = (i >= 0) ? VANE_MAP[i] : VANE_MAP[0];
    xSemaphoreTake(want_mtx_, portMAX_DELAY);
    wanted_.vane = nv;
    d_vane_ = true;
    lastWanted_ = now_ms();
    xSemaphoreGive(want_mtx_);
}

void HeatPump::setWideVaneSetting(const std::string& wideVane) {
    int i = lookupIdxStr(WIDEVANE_MAP, 7, wideVane);
    std::string nv = (i >= 0) ? WIDEVANE_MAP[i] : WIDEVANE_MAP[0];
    xSemaphoreTake(want_mtx_, portMAX_DELAY);
    wanted_.wideVane = nv;
    d_wideVane_ = true;
    lastWanted_ = now_ms();
    xSemaphoreGive(want_mtx_);
}

void HeatPump::setRemoteTemperature(float celsius) {
    xSemaphoreTake(want_mtx_, portMAX_DELAY);
    pendingRemoteTemp_ = celsius;
    pendingRemoteTempSet_ = true;
    xSemaphoreGive(want_mtx_);
}

}  // namespace cn105
