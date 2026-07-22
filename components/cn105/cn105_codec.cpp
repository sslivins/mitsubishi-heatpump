/// @file cn105_codec.cpp
/// @brief Implementation of the pure CN105 wire codec (see cn105_codec.h).
///
/// This is a behaviour-preserving extraction of the framing / decode / encode
/// logic that previously lived inline in cn105.cpp. It contains no I/O, no
/// timers and no shared mutable state, so it is unit-tested directly on the
/// host with a bare g++ (test/host/test_cn105_codec.cpp).

#include "cn105_codec.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <strings.h>  // strcasecmp

namespace cn105 {
namespace codec {

// --- Protocol constants (from SwiCago/HeatPump) -----------------------------
namespace {

constexpr int INFOHEADER_LEN = 5;

const uint8_t HEADER_SET[8]       = {0xfc, 0x41, 0x01, 0x30, 0x10, 0x01, 0x00, 0x00};
const uint8_t INFOHEADER[5]       = {0xfc, 0x42, 0x01, 0x30, 0x10};
const uint8_t CONTROL_PACKET_1[5] = {0x01, 0x02, 0x04, 0x08, 0x10};  // power,mode,temp,fan,vane
const uint8_t CONTROL_PACKET_2[1] = {0x01};                          // wideVane

const uint8_t POWER_B[2]    = {0x00, 0x01};
const char*   POWER_MAP[2]  = {"OFF", "ON"};
const uint8_t MODE_B[5]     = {0x01, 0x02, 0x03, 0x07, 0x08};
const char*   MODE_MAP[5]   = {"HEAT", "DRY", "COOL", "FAN", "AUTO"};
const uint8_t TEMP_B[16]    = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                               0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
const int     TEMP_MAP[16]  = {31, 30, 29, 28, 27, 26, 25, 24,
                               23, 22, 21, 20, 19, 18, 17, 16};
const uint8_t FAN_B[6]      = {0x00, 0x01, 0x02, 0x03, 0x05, 0x06};
const char*   FAN_MAP[6]    = {"AUTO", "QUIET", "1", "2", "3", "4"};
const uint8_t VANE_B[7]     = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x07};
const char*   VANE_MAP[7]   = {"AUTO", "1", "2", "3", "4", "5", "SWING"};
const uint8_t WIDEVANE_B[7] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x08, 0x0c};
const char*   WIDEVANE_MAP[7] = {"<<", "<", "|", ">", ">>", "<>", "SWING"};
const uint8_t ROOM_TEMP_B[32] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
                                 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
                                 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
const int     ROOM_TEMP_MAP[32] = {10, 11, 12, 13, 14, 15, 16, 17,
                                   18, 19, 20, 21, 22, 23, 24, 25,
                                   26, 27, 28, 29, 30, 31, 32, 33,
                                   34, 35, 36, 37, 38, 39, 40, 41};

const uint8_t SUB_MODE_B[6]   = {0x00, 0x01, 0x02, 0x04, 0x08, 0x10};
const char*   SUB_MODE_MAP[6] = {"NORMAL", "WARMUP", "DEFROST",
                                 "PREHEAT", "STANDBY", "OFF"};
const uint8_t STAGE_B[7]      = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
const char*   STAGE_MAP[7]    = {"IDLE", "LOW", "GENTLE", "MEDIUM",
                                 "MODERATE", "HIGH", "DIFFUSE"};

int lookupIdxStr(const char* const map[], int len, const std::string& v) {
    for (int i = 0; i < len; i++) {
        if (strcasecmp(map[i], v.c_str()) == 0) return i;
    }
    return -1;
}
const char* lookupValStr(const char* const map[], const uint8_t bmap[],
                         int len, uint8_t b) {
    for (int i = 0; i < len; i++) {
        if (bmap[i] == b) return map[i];
    }
    return map[0];
}
int lookupValInt(const int map[], const uint8_t bmap[], int len, uint8_t b) {
    for (int i = 0; i < len; i++) {
        if (bmap[i] == b) return map[i];
    }
    return map[0];
}
int lookupIdxInt(const int map[], int len, int v) {
    for (int i = 0; i < len; i++) {
        if (map[i] == v) return i;
    }
    return -1;
}

}  // namespace

// --- checksum ---------------------------------------------------------------
uint8_t checksum(const uint8_t* bytes, int len) {
    uint8_t sum = 0;
    for (int i = 0; i < len; i++) sum += bytes[i];
    return static_cast<uint8_t>((0xFC - sum) & 0xFF);
}

// --- FrameParser ------------------------------------------------------------
void FrameParser::reset() {
    st_       = St::WaitStart;
    hdrCount_ = 0;
    len_      = 0;
    dataCount_ = 0;
    type_     = 0;
    std::memset(hdr_, 0, sizeof(hdr_));
    std::memset(data_, 0, sizeof(data_));
}

FrameParser::Push FrameParser::push(uint8_t byte, Frame& out) {
    switch (st_) {
        case St::WaitStart:
            if (byte == 0xFC) {
                hdr_[0]   = 0xFC;
                hdrCount_ = 1;
                st_       = St::Header;
            }
            return Push::More;

        case St::Header:
            hdr_[hdrCount_++] = byte;
            if (hdrCount_ < INFOHEADER_LEN) return Push::More;
            // Full 5-byte header captured — validate exactly as readPacket_ did.
            if (hdr_[2] != 0x01 || hdr_[3] != 0x30) { reset(); return Push::Error; }
            len_ = hdr_[4];
            if (len_ > kMaxPayload) { reset(); return Push::Error; }
            type_      = hdr_[1];
            dataCount_ = 0;
            std::memset(data_, 0, sizeof(data_));
            st_ = (len_ == 0) ? St::Checksum : St::Data;
            return Push::More;

        case St::Data:
            data_[dataCount_++] = byte;
            if (dataCount_ >= len_) st_ = St::Checksum;
            return Push::More;

        case St::Checksum: {
            int sum = 0;
            for (int i = 0; i < INFOHEADER_LEN; i++) sum += hdr_[i];
            for (int i = 0; i < len_; i++) sum += data_[i];
            uint8_t expect = static_cast<uint8_t>((0xFC - sum) & 0xFF);
            if (byte != expect) { reset(); return Push::Error; }

            out.type     = type_;
            out.len      = len_;
            out.checksum = byte;
            std::memset(out.data, 0, sizeof(out.data));
            std::memcpy(out.data, data_, len_);
            reset();
            return Push::Complete;
        }
    }
    reset();
    return Push::Error;  // unreachable
}

// --- input normalisers ------------------------------------------------------
const char* normPower(const std::string& v) {
    int i = lookupIdxStr(POWER_MAP, 2, v);
    return (i >= 0) ? POWER_MAP[i] : POWER_MAP[0];
}
const char* normMode(const std::string& v) {
    int i = lookupIdxStr(MODE_MAP, 5, v);
    return (i >= 0) ? MODE_MAP[i] : MODE_MAP[0];
}
const char* normFan(const std::string& v) {
    int i = lookupIdxStr(FAN_MAP, 6, v);
    return (i >= 0) ? FAN_MAP[i] : FAN_MAP[0];
}
const char* normVane(const std::string& v) {
    int i = lookupIdxStr(VANE_MAP, 7, v);
    return (i >= 0) ? VANE_MAP[i] : VANE_MAP[0];
}
const char* normWideVane(const std::string& v) {
    int i = lookupIdxStr(WIDEVANE_MAP, 7, v);
    return (i >= 0) ? WIDEVANE_MAP[i] : WIDEVANE_MAP[0];
}
float snapIntSetpoint(float celsius) {
    return (lookupIdxInt(TEMP_MAP, 16, static_cast<int>(celsius + 0.5f)) > -1)
               ? celsius
               : static_cast<float>(TEMP_MAP[0]);
}

// --- decoders ---------------------------------------------------------------
SettingsPayload decodeSettings(const uint8_t* data, int len) {
    SettingsPayload p{};
    p.power = lookupValStr(POWER_MAP, POWER_B, 2, data[3]);
    bool iSee = data[4] > 0x08;  // iSee sensor present -> mode byte offset
    uint8_t modeByte = iSee ? static_cast<uint8_t>(data[4] - 0x08) : data[4];
    p.mode = lookupValStr(MODE_MAP, MODE_B, 5, modeByte);

    if (data[11] != 0x00) {  // half-degree ("tempMode") encoding
        int t = static_cast<int>(data[11]) - 128;
        p.temperature = static_cast<float>(t) / 2.0f;
        p.tempMode = true;
    } else {
        p.temperature = static_cast<float>(lookupValInt(TEMP_MAP, TEMP_B, 16, data[5]));
        p.tempMode = false;
    }
    p.fan      = lookupValStr(FAN_MAP, FAN_B, 6, data[6]);
    p.vane     = lookupValStr(VANE_MAP, VANE_B, 7, data[7]);
    p.wideVane = lookupValStr(WIDEVANE_MAP, WIDEVANE_B, 7,
                              static_cast<uint8_t>(data[10] & 0x0F));
    p.wideVaneAdj = ((data[10] & 0xF0) == 0x80);

    p.hasTargetHumidity = false;
    p.targetHumidity = 0;
    if (len >= 13 && data[12] >= 1 && data[12] <= 100) {
        p.hasTargetHumidity = true;
        p.targetHumidity = static_cast<int>(data[12]);
    }
    return p;
}

RoomTempPayload decodeRoomTemp(const uint8_t* data, int len) {
    RoomTempPayload p{};
    if (data[6] != 0x00) {
        int t = static_cast<int>(data[6]) - 128;
        p.roomTemperature = static_cast<float>(t) / 2.0f;
    } else {
        p.roomTemperature = static_cast<float>(
            lookupValInt(ROOM_TEMP_MAP, ROOM_TEMP_B, 32, data[3]));
    }

    p.hasOutsideTemp = false;
    p.outsideTemp = 0.0f;
    if (len >= 6 && data[5] > 1) {
        p.hasOutsideTemp = true;
        p.outsideTemp = (static_cast<float>(data[5]) - 128.0f) / 2.0f;
    }

    p.hasRuntimeHours = false;
    p.runtimeHours = 0.0f;
    if (len >= 14) {
        uint32_t rawMin = (static_cast<uint32_t>(data[11]) << 16) |
                          (static_cast<uint32_t>(data[12]) << 8) |
                          static_cast<uint32_t>(data[13]);
        p.hasRuntimeHours = true;
        p.runtimeHours = static_cast<float>(rawMin) / 60.0f;
    }
    return p;
}

OperatingPayload decodeOperating(const uint8_t* data, int len) {
    OperatingPayload p{};
    p.operating = data[4] != 0;
    p.compressorFrequency = data[3];

    p.hasInputPower = false;
    p.inputPowerW = 0;
    if (len >= 7) {
        p.hasInputPower = true;
        p.inputPowerW = (static_cast<int>(data[5]) << 8) | static_cast<int>(data[6]);
    }

    p.hasEnergy = false;
    p.energyKwh = 0.0f;
    if (len >= 9) {
        p.hasEnergy = true;
        p.energyKwh = static_cast<float>(
                          (static_cast<int>(data[7]) << 8) |
                          static_cast<int>(data[8])) / 10.0f;
    }
    return p;
}

StandbyPayload decodeStandby(const uint8_t* data, int len) {
    StandbyPayload p{};
    p.hasSubMode = false;
    p.subMode = "";
    p.hasStage = false;
    p.stage = "";
    if (len >= 5) {
        for (int i = 0; i < 6; i++)
            if (SUB_MODE_B[i] == data[3]) { p.subMode = SUB_MODE_MAP[i]; p.hasSubMode = true; break; }
        for (int i = 0; i < 7; i++)
            if (STAGE_B[i] == data[4]) { p.stage = STAGE_MAP[i]; p.hasStage = true; break; }
    }
    return p;
}

ErrorPayload decodeError(const uint8_t* data, int len) {
    ErrorPayload p{};
    p.text[0] = '\0';
    if (len >= 6) {
        uint8_t ecode = data[4] & 0x7F;  // bit7 is a status flag, not error
        uint8_t esub  = data[5];
        if (ecode == 0x00 && esub == 0x00) {
            std::snprintf(p.text, sizeof(p.text), "No Error");
        } else {
            std::snprintf(p.text, sizeof(p.text), "Error 0x%02X sub 0x%02X", ecode, esub);
        }
    }
    return p;
}

// --- encoders ---------------------------------------------------------------
void buildSetPacket(uint8_t* out, const Settings& w, const Settings& current,
                    bool tempMode, bool wideVaneAdj) {
    std::memset(out, 0, kPacketLen);
    std::memcpy(out, HEADER_SET, sizeof(HEADER_SET));

    if (w.power != current.power) {
        int i = lookupIdxStr(POWER_MAP, 2, w.power);
        if (i >= 0) { out[8] = POWER_B[i]; out[6] += CONTROL_PACKET_1[0]; }
    }
    if (w.mode != current.mode) {
        int i = lookupIdxStr(MODE_MAP, 5, w.mode);
        if (i >= 0) { out[9] = MODE_B[i]; out[6] += CONTROL_PACKET_1[1]; }
    }
    if (!tempMode && w.temperature != current.temperature) {
        int i = lookupIdxInt(TEMP_MAP, 16, static_cast<int>(w.temperature + 0.5f));
        if (i >= 0) { out[10] = TEMP_B[i]; out[6] += CONTROL_PACKET_1[2]; }
    } else if (tempMode && w.temperature != current.temperature) {
        float temp = (w.temperature * 2) + 128;
        out[19] = static_cast<uint8_t>(static_cast<int>(temp));
        out[6] += CONTROL_PACKET_1[2];
    }
    if (w.fan != current.fan) {
        int i = lookupIdxStr(FAN_MAP, 6, w.fan);
        if (i >= 0) { out[11] = FAN_B[i]; out[6] += CONTROL_PACKET_1[3]; }
    }
    if (w.vane != current.vane) {
        int i = lookupIdxStr(VANE_MAP, 7, w.vane);
        if (i >= 0) { out[12] = VANE_B[i]; out[6] += CONTROL_PACKET_1[4]; }
    }
    if (w.wideVane != current.wideVane) {
        int i = lookupIdxStr(WIDEVANE_MAP, 7, w.wideVane);
        if (i >= 0) {
            out[18] = static_cast<uint8_t>(WIDEVANE_B[i] | (wideVaneAdj ? 0x80 : 0x00));
            out[7] += CONTROL_PACKET_2[0];
        }
    }
    out[21] = checksum(out, 21);
}

void buildRemoteTempPacket(uint8_t* out, float celsius) {
    std::memset(out, 0, kPacketLen);
    std::memcpy(out, HEADER_SET, sizeof(HEADER_SET));
    out[5] = 0x07;
    if (celsius > 0) {
        out[6] = 0x01;
        celsius = roundf(celsius * 2.0f) / 2.0f;  // snap to 0.5C
        float temp1 = 3 + ((celsius - 10) * 2);
        out[7] = static_cast<uint8_t>(static_cast<int>(temp1));
        float temp2 = (celsius * 2) + 128;
        out[8] = static_cast<uint8_t>(static_cast<int>(temp2));
    } else {
        out[6] = 0x00;
        out[8] = 0x80;  // MHK1 sends 0x80 even though control byte is 0x00
    }
    out[21] = checksum(out, 21);
}

void buildInfoPacket(uint8_t* out, uint8_t infoModeByte) {
    std::memset(out, 0, kPacketLen);
    for (int i = 0; i < INFOHEADER_LEN; i++) out[i] = INFOHEADER[i];
    out[5] = infoModeByte;
    out[21] = checksum(out, 21);
}

}  // namespace codec
}  // namespace cn105
