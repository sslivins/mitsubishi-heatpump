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
#include "cn105_codec.h"

#include <cmath>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace cn105 {

static const char* TAG = "cn105";

// --- Protocol constants (from SwiCago/HeatPump) -----------------------------
// The wire vocabularies (value tables), the frame checksum, the incremental
// frame parser and all packet encode/decode now live in the pure, host-tested
// codec (cn105_codec.{h,cpp}). Only the constants the driver's state machine
// itself needs remain here.
static constexpr int      kBaud                 = 2400;
static constexpr int      PACKET_LEN            = 22;
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
static const uint8_t INFOMODE[6]        = {0x02, 0x03, 0x06, 0x04, 0x05, 0x09};

// --- small helpers ----------------------------------------------------------
static inline uint32_t now_ms() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL);
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
    bool wvProbe, wvProbeSend;
    std::string wvTarget;
    xSemaphoreTake(want_mtx_, portMAX_DELAY);
    wsnap = wanted_;
    wvProbe = wvProbe_;
    wvProbeSend = wvProbeSend_;
    wvTarget = wvProbeTarget_;
    xSemaphoreGive(want_mtx_);

    // One-shot wide-vane probe SET: send exactly once, without recording intent
    // (no dirty flag, no optimistic current_ write), so we can then watch the
    // unit's own reported position to tell powered from manual.
    if (wvProbeSend && canSend_(false)) {
        sendWideVaneOnce_(wvTarget);
        xSemaphoreTake(want_mtx_, portMAX_DELAY);
        wvProbeSend_ = false;
        xSemaphoreGive(want_mtx_);
        publishSnapshot_();
        fireCallbacks_();
        return;
    }

    bool differs = (wsnap.power != current_.power) ||
                   (wsnap.mode != current_.mode) ||
                   (wsnap.temperature != current_.temperature) ||
                   (wsnap.fan != current_.fan) ||
                   (wsnap.vane != current_.vane) ||
                   (!wvProbe && wsnap.wideVane != current_.wideVane);

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

    // Otherwise poll the unit for the next info packet. During a wide-vane probe
    // bias every poll to the settings (0x02) reply so current_ tracks the unit's
    // real wide-vane position quickly enough to catch a manual louver's revert.
    if (canSend_(true)) {
        int packetType;
        if (wvProbe) {
            packetType = RQST_PKT_SETTINGS;
        } else {
            packetType = sync_requested_.exchange(false) ? RQST_PKT_SETTINGS
                                                         : PACKET_TYPE_DEFAULT;
        }
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

void HeatPump::sendWideVaneOnce_(const std::string& target) {
    // Build a wide-vane-only SET by diffing a copy that changes only wideVane;
    // createSetPacket_ then emits just the wide-vane field. Deliberately does
    // NOT write target into current_ — the probe must observe the unit's own
    // echoed position (hold vs revert), not our optimistic guess.
    Settings w = current_;
    w.wideVane = target;
    drainPackets_(4);
    uint8_t pkt[PACKET_LEN] = {};
    createSetPacket_(pkt, w);
    writePacket_(pkt, PACKET_LEN);

    uint32_t t0 = now_ms();
    int r = RCVD_PKT_FAIL;
    do {
        r = readPacket_(300);
    } while (r != RCVD_PKT_UPDATE_SUCCESS && (now_ms() - t0) < 1000);
    infoMode_ = 0;  // re-read settings promptly so current_ tracks the unit
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
    codec::buildRemoteTempPacket(pkt, setting);
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
// Framing/validation now lives in the pure codec::FrameParser; this function
// owns only the per-byte read timeouts (which preserve the original phased
// semantics: the caller's timeout for the very first scan byte, 20ms for
// subsequent pre-start scan bytes, 100ms once we are inside a frame) and the
// post-frame dispatch/side-effects.
int HeatPump::readPacket_(uint32_t start_to_ms) {
    codec::FrameParser fp;
    codec::FrameParser::Frame frame;
    bool first = true;

    for (;;) {
        uint32_t to = fp.inFrame() ? 100 : (first ? start_to_ms : 20);
        int b = readByte_(to);
        if (b < 0) return RCVD_PKT_FAIL;
        first = false;

        codec::FrameParser::Push r = fp.push(static_cast<uint8_t>(b), frame);
        if (r == codec::FrameParser::Push::Error) return RCVD_PKT_FAIL;
        if (r == codec::FrameParser::Push::Complete) break;
    }

    lastRecv_ = now_ms();
    connectBackoff_ = CONNECT_BACKOFF_MIN_MS;  // valid traffic: recover fast next time

    // No lock is held here, so callbacks are safe to invoke directly.
    if (on_packet_) {
        uint8_t whole[38];
        int wl = 0;
        whole[wl++] = 0xFC;
        whole[wl++] = frame.type;
        whole[wl++] = 0x01;
        whole[wl++] = 0x30;
        whole[wl++] = static_cast<uint8_t>(frame.len);
        for (int i = 0; i < frame.len; i++) whole[wl++] = frame.data[i];
        whole[wl++] = frame.checksum;
        on_packet_(whole, static_cast<size_t>(wl), "RX");
    }

    if (frame.type == 0x62) {
        return decodeInfo_(frame.data, frame.len);
    } else if (frame.type == 0x61) {   // SET acknowledged
        return RCVD_PKT_UPDATE_SUCCESS;
    } else if (frame.type == 0x7a) {   // CONNECT acknowledged
        connected_ = true;
        return RCVD_PKT_CONNECT_SUCCESS;
    }
    return RCVD_PKT_FAIL;
}

int HeatPump::decodeInfo_(const uint8_t* data, int len) {
    switch (data[0]) {
        case 0x02: {  // settings
            codec::SettingsPayload p = codec::decodeSettings(data, len);
            Settings rs = current_;
            rs.power = p.power;
            rs.mode  = p.mode;
            rs.temperature = p.temperature;
            if (p.tempMode) tempMode_ = true;  // sticky, matching the original
            rs.fan      = p.fan;
            rs.vane     = p.vane;
            rs.wideVane = p.wideVane;
            wideVaneAdj_ = p.wideVaneAdj;

            // Extended telemetry: target humidity (data[12], 1..100 %). Additive
            // — decoded from the same 0x02 reply; not part of the SET contract.
            if (p.hasTargetHumidity) {
                if (!status_.has_targetHumidity || p.targetHumidity != status_.targetHumidity) {
                    status_.targetHumidity = p.targetHumidity;
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
            codec::RoomTempPayload p = codec::decodeRoomTemp(data, len);
            if (p.roomTemperature != status_.roomTemperature) {
                status_.roomTemperature = p.roomTemperature;
                statusChanged_ = true;
            }
            // Extended telemetry from the same 0x03 reply:
            //   outside air/coil temp: data[5] (>1) -> (data[5]-128)/2 °C
            //   cumulative runtime:    (data[11]<<16|data[12]<<8|data[13]) / 60 h
            if (p.hasOutsideTemp) {
                if (!status_.has_outsideTemp || p.outsideTemp != status_.outsideTemp) {
                    status_.outsideTemp = p.outsideTemp;
                    status_.has_outsideTemp = true;
                    statusChanged_ = true;
                }
            }
            if (p.hasRuntimeHours) {
                if (!status_.has_runtimeHours || p.runtimeHours != status_.runtimeHours) {
                    status_.runtimeHours = p.runtimeHours;
                    status_.has_runtimeHours = true;
                    statusChanged_ = true;
                }
            }
            return RCVD_PKT_ROOM_TEMP;
        }

        case 0x06: {  // operating status + compressor frequency
            codec::OperatingPayload p = codec::decodeOperating(data, len);
            if (p.operating != status_.operating ||
                p.compressorFrequency != status_.compressorFrequency) {
                status_.operating = p.operating;
                status_.compressorFrequency = p.compressorFrequency;
                statusChanged_ = true;
            }
            // Extended telemetry from the same 0x06 reply:
            //   input power:  (data[5]<<8|data[6]) W
            //   energy usage: (data[7]<<8|data[8]) / 10 kWh
            if (p.hasInputPower) {
                if (!status_.has_inputPowerW || p.inputPowerW != status_.inputPowerW) {
                    status_.inputPowerW = p.inputPowerW;
                    status_.has_inputPowerW = true;
                    statusChanged_ = true;
                }
            }
            if (p.hasEnergy) {
                if (!status_.has_energyKwh || p.energyKwh != status_.energyKwh) {
                    status_.energyKwh = p.energyKwh;
                    status_.has_energyKwh = true;
                    statusChanged_ = true;
                }
            }
            return RCVD_PKT_STATUS;
        }

        case 0x09: {  // power/standby: sub-mode + fan stage (additive telemetry)
            codec::StandbyPayload p = codec::decodeStandby(data, len);
            if (p.hasSubMode && p.subMode != status_.subMode) {
                status_.subMode = p.subMode;
                statusChanged_ = true;
            }
            if (p.hasStage && p.stage != status_.stage) {
                status_.stage = p.stage;
                statusChanged_ = true;
            }
            return RCVD_PKT_STATUS;
        }

        case 0x04: {  // error info (additive telemetry)
            if (len >= 6) {
                codec::ErrorPayload p = codec::decodeError(data, len);
                if (status_.errorCode != p.text) {
                    status_.errorCode = p.text;
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
    return codec::checksum(bytes, len);
}

void HeatPump::createSetPacket_(uint8_t* packet, const Settings& w) {
    codec::buildSetPacket(packet, w, current_, tempMode_, wideVaneAdj_);
}

void HeatPump::createInfoPacket_(uint8_t* packet, int packetType) {
    // The infoMode cycling counter is driver state, so it stays here; the codec
    // just serialises the resolved info-mode byte into a packet.
    uint8_t modeByte;
    if (packetType != PACKET_TYPE_DEFAULT) {
        int idx = (packetType >= 0 && packetType < INFOMODE_LEN) ? packetType : 0;
        modeByte = INFOMODE[idx];
    } else {
        modeByte = INFOMODE[infoMode_];
        infoMode_ = (infoMode_ == (INFOMODE_LEN - 1)) ? 0 : (infoMode_ + 1);
    }
    codec::buildInfoPacket(packet, modeByte);
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
    std::string nv = codec::normPower(power);
    xSemaphoreTake(want_mtx_, portMAX_DELAY);
    wanted_.power = nv;
    d_power_ = true;
    lastWanted_ = now_ms();
    xSemaphoreGive(want_mtx_);
}

void HeatPump::setModeSetting(const std::string& mode) {
    std::string nv = codec::normMode(mode);
    xSemaphoreTake(want_mtx_, portMAX_DELAY);
    wanted_.mode = nv;
    d_mode_ = true;
    lastWanted_ = now_ms();
    xSemaphoreGive(want_mtx_);
}

void HeatPump::setTemperature(float celsius) {
    float nv;
    if (!tempMode_) {
        nv = codec::snapIntSetpoint(celsius);
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
    std::string nv = codec::normFan(fan);
    xSemaphoreTake(want_mtx_, portMAX_DELAY);
    wanted_.fan = nv;
    d_fan_ = true;
    lastWanted_ = now_ms();
    xSemaphoreGive(want_mtx_);
}

void HeatPump::setVaneSetting(const std::string& vane) {
    std::string nv = codec::normVane(vane);
    xSemaphoreTake(want_mtx_, portMAX_DELAY);
    wanted_.vane = nv;
    d_vane_ = true;
    lastWanted_ = now_ms();
    xSemaphoreGive(want_mtx_);
}

void HeatPump::setWideVaneSetting(const std::string& wideVane) {
    std::string nv = codec::normWideVane(wideVane);
    xSemaphoreTake(want_mtx_, portMAX_DELAY);
    wanted_.wideVane = nv;
    d_wideVane_ = true;
    lastWanted_ = now_ms();
    xSemaphoreGive(want_mtx_);
}

void HeatPump::beginWideVaneProbe(const std::string& target) {
    std::string nv = codec::normWideVane(target);
    xSemaphoreTake(want_mtx_, portMAX_DELAY);
    wvProbe_ = true;
    wvProbeSend_ = true;
    wvProbeTarget_ = nv;
    xSemaphoreGive(want_mtx_);
}

void HeatPump::endWideVaneProbeRestore(const std::string& restore) {
    std::string nv = codec::normWideVane(restore);
    xSemaphoreTake(want_mtx_, portMAX_DELAY);
    wvProbe_ = false;
    wvProbeSend_ = false;
    wanted_.wideVane = nv;
    d_wideVane_ = true;  // enforce the restore back to the pre-probe position
    lastWanted_ = now_ms();
    xSemaphoreGive(want_mtx_);
}

void HeatPump::abortWideVaneProbe() {
    xSemaphoreTake(want_mtx_, portMAX_DELAY);
    wvProbe_ = false;
    wvProbeSend_ = false;
    // Leave wanted_ untouched: enforcement resumes toward whatever the user (or
    // adoption) last set, so a concurrent user change wins.
    xSemaphoreGive(want_mtx_);
}

void HeatPump::setRemoteTemperature(float celsius) {
    xSemaphoreTake(want_mtx_, portMAX_DELAY);
    pendingRemoteTemp_ = celsius;
    pendingRemoteTempSet_ = true;
    xSemaphoreGive(want_mtx_);
}

}  // namespace cn105
