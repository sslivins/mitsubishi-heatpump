// Host-side unit tests for the pure CN105 wire codec (cn105_codec.cpp).
//
// These run on the build host with a plain C++17 compiler — no ESP-IDF, no
// hardware, no UART. They lock down the safety-critical wire boundary of the
// CN105 protocol that was previously untested and tangled with device state:
//   * the frame checksum (known-answer against a real captured packet),
//   * the incremental FrameParser (framing, validation, adversarial streams,
//     fragmentation at every split point, recovery after malformed input),
//   * the typed payload decoders (golden vectors with hand-verified values,
//     enum fallbacks, both temp encodings, extended-telemetry length guards,
//     and out-of-bounds safety on short frames),
//   * the packet encoders (byte-for-byte expected SET / remote-temp / info
//     packets incl. the control-mask accumulation).
//
// Build & run (see .github/workflows/host-tests.yml): one g++ invocation with
//   -std=c++17 -I components/cn105/include
//   components/cn105/cn105_codec.cpp test/host/test_cn105_codec.cpp

#include "cn105_codec.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;

#define CHECK_EQ(actual, expected)                                            \
    do {                                                                      \
        const std::string a_ = (actual);                                      \
        const std::string e_ = (expected);                                    \
        if (a_ != e_) {                                                       \
            std::printf("FAIL %s:%d  %s\n      got: \"%s\"\n expected: \"%s\"\n", \
                        __FILE__, __LINE__, #actual, a_.c_str(), e_.c_str()); \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

#define CHECK_INT(actual, expected)                                           \
    do {                                                                      \
        const long a_ = (long)(actual);                                       \
        const long e_ = (long)(expected);                                     \
        if (a_ != e_) {                                                       \
            std::printf("FAIL %s:%d  %s\n      got: %ld\n expected: %ld\n",   \
                        __FILE__, __LINE__, #actual, a_, e_);                 \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

#define CHECK_FLOAT(actual, expected)                                         \
    do {                                                                      \
        const double a_ = (double)(actual);                                   \
        const double e_ = (double)(expected);                                 \
        const double d_ = a_ - e_;                                            \
        if (d_ > 1e-4 || d_ < -1e-4) {                                        \
            std::printf("FAIL %s:%d  %s\n      got: %f\n expected: %f\n",     \
                        __FILE__, __LINE__, #actual, a_, e_);                 \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

#define CHECK_TRUE(cond)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("FAIL %s:%d  expected true: %s\n",                    \
                        __FILE__, __LINE__, #cond);                           \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

#define CHECK_FALSE(cond)                                                     \
    do {                                                                      \
        if (cond) {                                                           \
            std::printf("FAIL %s:%d  expected false: %s\n",                   \
                        __FILE__, __LINE__, #cond);                           \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

using namespace cn105;
using codec::FrameParser;

// --- helpers ----------------------------------------------------------------

// Serialise a full CN105 frame: [0xFC][type][0x01][0x30][len][payload][cksum].
// The checksum is computed by codec::checksum, which is independently pinned by
// a known-answer test below, so decode assertions never trust an unverified sum.
static std::vector<uint8_t> makeFrame(uint8_t type, const uint8_t* payload, int len) {
    std::vector<uint8_t> f;
    f.push_back(0xFC);
    f.push_back(type);
    f.push_back(0x01);
    f.push_back(0x30);
    f.push_back(static_cast<uint8_t>(len));
    for (int i = 0; i < len; i++) f.push_back(payload[i]);
    f.push_back(codec::checksum(f.data(), static_cast<int>(f.size())));
    return f;
}

// Feed a byte stream through a persistent parser, collecting every completed
// frame and counting Error results. This exercises fragmentation at every split
// point automatically because the parser is driven one byte at a time.
static int feedStream(FrameParser& fp, const std::vector<uint8_t>& bytes,
                      std::vector<FrameParser::Frame>& out, int* errors = nullptr) {
    int completes = 0;
    if (errors) *errors = 0;
    for (uint8_t b : bytes) {
        FrameParser::Frame fr;
        FrameParser::Push r = fp.push(b, fr);
        if (r == FrameParser::Push::Complete) { out.push_back(fr); ++completes; }
        else if (r == FrameParser::Push::Error && errors) { ++(*errors); }
    }
    return completes;
}

// ============================================================================
// checksum
// ============================================================================
static void test_checksum_known_answer() {
    // The real CONNECT handshake packet: its 8th byte (0xa8) is the checksum of
    // the first 7 bytes. This pins codec::checksum to a hardware-captured value.
    const uint8_t CONNECT[8] = {0xfc, 0x5a, 0x01, 0x30, 0x02, 0xca, 0x01, 0xa8};
    CHECK_INT(codec::checksum(CONNECT, 7), 0xa8);

    // Trivial: checksum of nothing is 0xFC.
    CHECK_INT(codec::checksum(CONNECT, 0), 0xFC);

    // Overflow wrap: 0xFC - 0xFC == 0.
    const uint8_t one[1] = {0xFC};
    CHECK_INT(codec::checksum(one, 1), 0x00);
}

// ============================================================================
// FrameParser
// ============================================================================
static void test_frame_basic() {
    const uint8_t payload[] = {0x02, 0x00, 0x00, 0x01};
    std::vector<uint8_t> f = makeFrame(0x62, payload, 4);

    FrameParser fp;
    std::vector<FrameParser::Frame> frames;
    int errs = 0;
    CHECK_INT(feedStream(fp, f, frames, &errs), 1);
    CHECK_INT(errs, 0);
    CHECK_INT(frames.size(), 1);
    CHECK_INT(frames[0].type, 0x62);
    CHECK_INT(frames[0].len, 4);
    CHECK_INT(frames[0].data[0], 0x02);
    CHECK_INT(frames[0].data[3], 0x01);
    CHECK_INT(frames[0].checksum, f.back());
    // Bytes past len must be zero-filled (decoders rely on this for OOB safety).
    CHECK_INT(frames[0].data[4], 0x00);
    CHECK_INT(frames[0].data[31], 0x00);
}

static void test_frame_zero_length() {
    // A len==0 frame is valid (no payload, checksum immediately follows).
    std::vector<uint8_t> f = makeFrame(0x61, nullptr, 0);
    FrameParser fp;
    std::vector<FrameParser::Frame> frames;
    CHECK_INT(feedStream(fp, f, frames), 1);
    CHECK_INT(frames[0].len, 0);
    CHECK_INT(frames[0].type, 0x61);
}

static void test_frame_max_length() {
    // len==32 is the largest accepted payload.
    uint8_t payload[32];
    for (int i = 0; i < 32; i++) payload[i] = static_cast<uint8_t>(i);
    std::vector<uint8_t> f = makeFrame(0x62, payload, 32);
    FrameParser fp;
    std::vector<FrameParser::Frame> frames;
    CHECK_INT(feedStream(fp, f, frames), 1);
    CHECK_INT(frames[0].len, 32);
    CHECK_INT(frames[0].data[31], 31);
}

static void test_frame_len_too_big() {
    // Declared length 33 (>32) must be rejected the moment the header completes.
    std::vector<uint8_t> f = {0xFC, 0x62, 0x01, 0x30, 33};
    FrameParser fp;
    std::vector<FrameParser::Frame> frames;
    int errs = 0;
    CHECK_INT(feedStream(fp, f, frames, &errs), 0);
    CHECK_INT(errs, 1);
    CHECK_FALSE(fp.inFrame());  // parser reset itself after the error
}

static void test_frame_bad_header() {
    // header[2]/[3] must be 0x01/0x30. A corrupt fixed header is rejected only
    // after all 5 header bytes are consumed (matches the original readPacket_).
    std::vector<uint8_t> f = {0xFC, 0x62, 0x99, 0x30, 0x04};
    FrameParser fp;
    std::vector<FrameParser::Frame> frames;
    int errs = 0;
    CHECK_INT(feedStream(fp, f, frames, &errs), 0);
    CHECK_INT(errs, 1);
}

static void test_frame_bad_checksum_and_recovery() {
    const uint8_t payload[] = {0x03, 0x00, 0x00, 0x0c};
    std::vector<uint8_t> bad = makeFrame(0x62, payload, 4);
    bad.back() ^= 0xFF;  // corrupt the checksum

    // Good frame that should still parse after the bad one — proves recovery.
    std::vector<uint8_t> good = makeFrame(0x62, payload, 4);

    std::vector<uint8_t> stream;
    stream.insert(stream.end(), bad.begin(), bad.end());
    stream.insert(stream.end(), good.begin(), good.end());

    FrameParser fp;
    std::vector<FrameParser::Frame> frames;
    int errs = 0;
    int completes = feedStream(fp, stream, frames, &errs);
    CHECK_INT(completes, 1);   // only the good frame
    CHECK_INT(errs, 1);        // the bad checksum
    CHECK_INT(frames.size(), 1);
    CHECK_INT(frames[0].data[3], 0x0c);
}

static void test_frame_garbage_before_start() {
    // Random noise before the 0xFC start byte must be skipped.
    const uint8_t payload[] = {0x06, 0x00, 0x2a, 0x01};
    std::vector<uint8_t> f = makeFrame(0x62, payload, 4);
    std::vector<uint8_t> stream = {0x00, 0xAB, 0x11, 0x30, 0x01};  // no 0xFC here
    stream.insert(stream.end(), f.begin(), f.end());

    FrameParser fp;
    std::vector<FrameParser::Frame> frames;
    CHECK_INT(feedStream(fp, stream, frames), 1);
    CHECK_INT(frames[0].data[2], 0x2a);
}

static void test_frame_back_to_back() {
    const uint8_t p1[] = {0x02, 0x00, 0x00, 0x01};
    const uint8_t p2[] = {0x03, 0x00, 0x00, 0x0c};
    std::vector<uint8_t> f1 = makeFrame(0x62, p1, 4);
    std::vector<uint8_t> f2 = makeFrame(0x62, p2, 4);
    std::vector<uint8_t> stream;
    stream.insert(stream.end(), f1.begin(), f1.end());
    stream.insert(stream.end(), f2.begin(), f2.end());

    FrameParser fp;
    std::vector<FrameParser::Frame> frames;
    CHECK_INT(feedStream(fp, stream, frames), 2);
    CHECK_INT(frames[0].data[0], 0x02);
    CHECK_INT(frames[1].data[0], 0x03);
}

static void test_frame_embedded_fc_in_payload() {
    // A 0xFC byte inside the payload is data, NOT a new start-of-frame, because
    // the parser is in the Data state. This must round-trip intact.
    const uint8_t payload[] = {0x02, 0xFC, 0xFC, 0x01};
    std::vector<uint8_t> f = makeFrame(0x62, payload, 4);
    FrameParser fp;
    std::vector<FrameParser::Frame> frames;
    CHECK_INT(feedStream(fp, f, frames), 1);
    CHECK_INT(frames[0].data[1], 0xFC);
    CHECK_INT(frames[0].data[2], 0xFC);
}

static void test_frame_fc_as_type_then_recover() {
    // 0xFC appearing right after the start byte is consumed as the *type* field.
    // {FC,FC,01,30,00} is a structurally valid zero-length header (type 0xFC),
    // so the next byte is treated as its checksum. We give it a deliberately
    // wrong checksum (0x00) to force an Error, then a clean frame that must
    // still parse — proving the parser re-syncs after a bogus 0xFC-typed frame.
    std::vector<uint8_t> stream = {0xFC, 0xFC, 0x01, 0x30, 0x00, 0x00 /*bad cksum*/};
    const uint8_t payload[] = {0x09, 0x08, 0x00};
    std::vector<uint8_t> f = makeFrame(0x62, payload, 3);
    stream.insert(stream.end(), f.begin(), f.end());

    FrameParser fp;
    std::vector<FrameParser::Frame> frames;
    int errs = 0;
    feedStream(fp, stream, frames, &errs);
    CHECK_TRUE(errs >= 1);
    bool sawClean = false;
    for (auto& fr : frames) if (fr.type == 0x62 && fr.data[0] == 0x09) sawClean = true;
    CHECK_TRUE(sawClean);
}

static void test_frame_split_across_pushes() {
    // Explicitly split a single frame between two feedStream calls to prove the
    // parser preserves state across byte boundaries.
    const uint8_t payload[] = {0x02, 0x00, 0x00, 0x01, 0x03, 0x07};
    std::vector<uint8_t> f = makeFrame(0x62, payload, 6);
    FrameParser fp;
    std::vector<FrameParser::Frame> frames;
    for (size_t split = 1; split < f.size(); split++) {
        fp.reset();
        frames.clear();
        std::vector<uint8_t> a(f.begin(), f.begin() + split);
        std::vector<uint8_t> b(f.begin() + split, f.end());
        int c = feedStream(fp, a, frames);
        c += feedStream(fp, b, frames);
        CHECK_INT(c, 1);
        CHECK_INT(frames.size(), 1);
        CHECK_INT(frames[0].data[5], 0x07);
    }
}

// ============================================================================
// decoders — golden vectors with hand-verified expected values
// ============================================================================
static void test_decode_settings_golden() {
    // power ON, mode COOL(0x03), temp 24 via TEMP_B[7]=0x07 (data[11]==0),
    // fan "2"(0x03), vane "3"(0x03), wideVane "|"(0x03, adj off).
    uint8_t d[16] = {};
    d[0] = 0x02;
    d[3] = 0x01;  // power ON
    d[4] = 0x03;  // mode COOL
    d[5] = 0x07;  // temp 24
    d[6] = 0x03;  // fan "2"
    d[7] = 0x03;  // vane "3"
    d[10] = 0x03; // wideVane "|"
    d[11] = 0x00; // integer temp encoding
    codec::SettingsPayload p = codec::decodeSettings(d, 16);
    CHECK_EQ(p.power, "ON");
    CHECK_EQ(p.mode, "COOL");
    CHECK_FLOAT(p.temperature, 24.0);
    CHECK_FALSE(p.tempMode);
    CHECK_EQ(p.fan, "2");
    CHECK_EQ(p.vane, "3");
    CHECK_EQ(p.wideVane, "|");
    CHECK_FALSE(p.wideVaneAdj);
    CHECK_FALSE(p.hasTargetHumidity);
}

static void test_decode_settings_isee_offset() {
    // iSee sensor present: mode byte carries a +0x08 offset that must be removed.
    uint8_t d[16] = {};
    d[0] = 0x02;
    d[4] = 0x03 + 0x08;  // COOL with iSee bit
    codec::SettingsPayload p = codec::decodeSettings(d, 16);
    CHECK_EQ(p.mode, "COOL");
}

static void test_decode_settings_half_degree() {
    // Half-degree ("tempMode") encoding: data[11] = temp*2 + 128.
    uint8_t d[16] = {};
    d[0] = 0x02;
    d[11] = 171;  // (171-128)/2 = 21.5C
    codec::SettingsPayload p = codec::decodeSettings(d, 16);
    CHECK_FLOAT(p.temperature, 21.5);
    CHECK_TRUE(p.tempMode);
}

static void test_decode_settings_widevane_adj() {
    // High nibble 0x80 means "wide vane adjust" — low nibble is the position.
    uint8_t d[16] = {};
    d[0] = 0x02;
    d[10] = 0x80 | 0x05;  // adj + ">>"
    codec::SettingsPayload p = codec::decodeSettings(d, 16);
    CHECK_EQ(p.wideVane, ">>");
    CHECK_TRUE(p.wideVaneAdj);
}

static void test_decode_settings_target_humidity() {
    uint8_t d[16] = {};
    d[0] = 0x02;
    d[12] = 45;  // target humidity 45% (len>=13 required)
    codec::SettingsPayload p = codec::decodeSettings(d, 16);
    CHECK_TRUE(p.hasTargetHumidity);
    CHECK_INT(p.targetHumidity, 45);

    // len < 13 must suppress the extended field even if the byte is present.
    codec::SettingsPayload p2 = codec::decodeSettings(d, 12);
    CHECK_FALSE(p2.hasTargetHumidity);

    // Out-of-range humidity (0 or >100) is ignored.
    d[12] = 200;
    codec::SettingsPayload p3 = codec::decodeSettings(d, 16);
    CHECK_FALSE(p3.hasTargetHumidity);
}

static void test_decode_settings_unknown_bytes_fallback() {
    // Unknown enum bytes fall back to the first map entry (never out of range).
    uint8_t d[16] = {};
    d[0] = 0x02;
    d[3] = 0x55;  // not a valid power byte -> "OFF"
    d[4] = 0x55;  // not a valid mode byte  -> "HEAT"
    d[6] = 0x55;  // fan -> "AUTO"
    codec::SettingsPayload p = codec::decodeSettings(d, 16);
    CHECK_EQ(p.power, "OFF");
    CHECK_EQ(p.mode, "HEAT");
    CHECK_EQ(p.fan, "AUTO");
}

static void test_decode_settings_short_frame_oob_safe() {
    // A pathologically short settings frame must not read past the buffer. The
    // codec operates on the zero-filled 32-wide frame buffer, so high offsets
    // read zeros and yield safe defaults.
    uint8_t d[32] = {};
    d[0] = 0x02;
    codec::SettingsPayload p = codec::decodeSettings(d, 1);
    CHECK_EQ(p.power, "OFF");
    CHECK_EQ(p.mode, "HEAT");
    CHECK_FLOAT(p.temperature, 31.0);  // TEMP_MAP[data[5]=0] -> index0 = 31
    CHECK_FALSE(p.hasTargetHumidity);
}

static void test_decode_room_temp_golden() {
    // Integer room temp via ROOM_TEMP_B[12]=0x0c -> 22C (data[6]==0).
    uint8_t d[16] = {};
    d[0] = 0x03;
    d[3] = 0x0c;
    codec::RoomTempPayload p = codec::decodeRoomTemp(d, 6);
    CHECK_FLOAT(p.roomTemperature, 22.0);
    CHECK_FALSE(p.hasOutsideTemp);  // data[5]==0 (<=1)
    CHECK_FALSE(p.hasRuntimeHours); // len<14
}

static void test_decode_room_temp_half_degree_and_extended() {
    // Half-degree room temp: data[6] = temp*2+128. Outside temp: data[5]>1.
    // Runtime minutes packed big-endian in data[11..13].
    uint8_t d[16] = {};
    d[0] = 0x03;
    d[6] = 171;   // 21.5C
    d[5] = 142;   // (142-128)/2 = 7.0C outside
    d[11] = 0x00; d[12] = 0x17; d[13] = 0x70;  // 0x1770 = 6000 min = 100h
    codec::RoomTempPayload p = codec::decodeRoomTemp(d, 14);
    CHECK_FLOAT(p.roomTemperature, 21.5);
    CHECK_TRUE(p.hasOutsideTemp);
    CHECK_FLOAT(p.outsideTemp, 7.0);
    CHECK_TRUE(p.hasRuntimeHours);
    CHECK_FLOAT(p.runtimeHours, 100.0);
}

static void test_decode_operating_golden() {
    // compressor 42Hz, operating, 1400W input, 123.0 kWh.
    uint8_t d[16] = {};
    d[0] = 0x06;
    d[3] = 42;
    d[4] = 0x01;
    d[5] = 0x05; d[6] = 0x78;  // 0x0578 = 1400
    d[7] = 0x04; d[8] = 0xCE;  // 0x04CE = 1230 -> /10 = 123.0
    codec::OperatingPayload p = codec::decodeOperating(d, 9);
    CHECK_TRUE(p.operating);
    CHECK_INT(p.compressorFrequency, 42);
    CHECK_TRUE(p.hasInputPower);
    CHECK_INT(p.inputPowerW, 1400);
    CHECK_TRUE(p.hasEnergy);
    CHECK_FLOAT(p.energyKwh, 123.0);

    // Short frame: only the base operating fields, no extended telemetry.
    codec::OperatingPayload p2 = codec::decodeOperating(d, 5);
    CHECK_TRUE(p2.operating);
    CHECK_INT(p2.compressorFrequency, 42);
    CHECK_FALSE(p2.hasInputPower);
    CHECK_FALSE(p2.hasEnergy);
}

static void test_decode_standby_golden() {
    uint8_t d[8] = {};
    d[0] = 0x09;
    d[3] = 0x08;  // STANDBY
    d[4] = 0x00;  // IDLE
    codec::StandbyPayload p = codec::decodeStandby(d, 5);
    CHECK_TRUE(p.hasSubMode);
    CHECK_EQ(p.subMode, "STANDBY");
    CHECK_TRUE(p.hasStage);
    CHECK_EQ(p.stage, "IDLE");

    // Unknown sub-mode byte -> not reported (has* false), matching the driver's
    // "only update on a recognised value" behaviour.
    d[3] = 0x7F;
    codec::StandbyPayload p2 = codec::decodeStandby(d, 5);
    CHECK_FALSE(p2.hasSubMode);
    CHECK_TRUE(p2.hasStage);

    // len<5 -> nothing decoded.
    codec::StandbyPayload p3 = codec::decodeStandby(d, 4);
    CHECK_FALSE(p3.hasSubMode);
    CHECK_FALSE(p3.hasStage);
}

static void test_decode_error_golden() {
    uint8_t d[8] = {};
    d[0] = 0x04;
    d[4] = 0x00; d[5] = 0x00;
    codec::ErrorPayload p = codec::decodeError(d, 6);
    CHECK_EQ(std::string(p.text), "No Error");

    d[4] = 0x81;  // bit7 is a status flag -> masked to 0x01
    d[5] = 0x02;
    codec::ErrorPayload p2 = codec::decodeError(d, 6);
    CHECK_EQ(std::string(p2.text), "Error 0x01 sub 0x02");
}

// End-to-end: bytes on the wire -> FrameParser -> typed decode, mirroring the
// exact dispatch the driver performs for a 0x62 info reply.
static void test_end_to_end_wire_to_value() {
    uint8_t payload[16] = {};
    payload[0] = 0x02;
    payload[3] = 0x01;   // ON
    payload[4] = 0x02;   // DRY
    payload[5] = 0x07;   // 24C
    std::vector<uint8_t> f = makeFrame(0x62, payload, 16);

    FrameParser fp;
    std::vector<FrameParser::Frame> frames;
    CHECK_INT(feedStream(fp, f, frames), 1);
    CHECK_INT(frames[0].type, 0x62);
    codec::SettingsPayload p = codec::decodeSettings(frames[0].data, frames[0].len);
    CHECK_EQ(p.power, "ON");
    CHECK_EQ(p.mode, "DRY");
    CHECK_FLOAT(p.temperature, 24.0);
}

// ============================================================================
// encoders — byte-for-byte expected packets
// ============================================================================
static Settings seededCurrent() {
    Settings s;
    s.power = "OFF"; s.mode = "HEAT"; s.temperature = 21.0f;
    s.fan = "AUTO"; s.vane = "AUTO"; s.wideVane = "|"; s.connected = true;
    return s;
}

static void test_build_set_power_only() {
    Settings cur = seededCurrent();
    Settings w = cur;
    w.power = "ON";
    uint8_t out[codec::kPacketLen];
    codec::buildSetPacket(out, w, cur, /*tempMode=*/false, /*wideVaneAdj=*/false);
    // Header
    CHECK_INT(out[0], 0xfc); CHECK_INT(out[1], 0x41); CHECK_INT(out[2], 0x01);
    CHECK_INT(out[3], 0x30); CHECK_INT(out[4], 0x10); CHECK_INT(out[5], 0x01);
    // Only the power control bit (0x01) is set; power value ON = 0x01 at [8].
    CHECK_INT(out[6], 0x01);
    CHECK_INT(out[7], 0x00);
    CHECK_INT(out[8], 0x01);
    CHECK_INT(out[9], 0x00);   // mode untouched
    CHECK_INT(out[21], codec::checksum(out, 21));
}

static void test_build_set_multi_field() {
    Settings cur = seededCurrent();
    Settings w = cur;
    w.power = "ON"; w.mode = "COOL"; w.temperature = 24.0f; w.fan = "4";
    uint8_t out[codec::kPacketLen];
    codec::buildSetPacket(out, w, cur, false, false);
    // control byte 1 = power|mode|temp|fan = 0x01|0x02|0x04|0x08 = 0x0F
    CHECK_INT(out[6], 0x0F);
    CHECK_INT(out[8], 0x01);   // power ON
    CHECK_INT(out[9], 0x03);   // mode COOL
    CHECK_INT(out[10], 0x07);  // temp 24 -> TEMP_B[7]
    CHECK_INT(out[11], 0x06);  // fan "4" -> FAN_B[5]=0x06
    CHECK_INT(out[21], codec::checksum(out, 21));
}

static void test_build_set_widevane() {
    Settings cur = seededCurrent();
    Settings w = cur;
    w.wideVane = ">>";
    uint8_t out[codec::kPacketLen];
    codec::buildSetPacket(out, w, cur, false, /*wideVaneAdj=*/true);
    CHECK_INT(out[6], 0x00);          // no CONTROL_PACKET_1 bits
    CHECK_INT(out[7], 0x01);          // CONTROL_PACKET_2 wide-vane bit
    CHECK_INT(out[18], 0x05 | 0x80);  // WIDEVANE_B[">>"]=0x05 OR adjust flag
}

static void test_build_set_tempmode() {
    Settings cur = seededCurrent();
    Settings w = cur;
    w.temperature = 21.5f;
    uint8_t out[codec::kPacketLen];
    codec::buildSetPacket(out, w, cur, /*tempMode=*/true, false);
    CHECK_INT(out[6], 0x04);   // temp control bit
    CHECK_INT(out[10], 0x00);  // integer temp slot unused in tempMode
    CHECK_INT(out[19], 171);   // 21.5*2 + 128
}

static void test_build_set_no_change() {
    // wanted == current: no fields differ, so no control bits and a bare header.
    Settings cur = seededCurrent();
    uint8_t out[codec::kPacketLen];
    codec::buildSetPacket(out, cur, cur, false, false);
    CHECK_INT(out[6], 0x00);
    CHECK_INT(out[7], 0x00);
    CHECK_INT(out[8], 0x00);
    CHECK_INT(out[21], codec::checksum(out, 21));
}

static void test_build_remote_temp() {
    uint8_t out[codec::kPacketLen];
    codec::buildRemoteTempPacket(out, 21.0f);
    CHECK_INT(out[0], 0xfc); CHECK_INT(out[1], 0x41);
    CHECK_INT(out[5], 0x07);
    CHECK_INT(out[6], 0x01);
    CHECK_INT(out[7], 0x19);  // 3 + (21-10)*2 = 25
    CHECK_INT(out[8], 0xAA);  // 21*2 + 128 = 170
    CHECK_INT(out[21], codec::checksum(out, 21));

    // Snap-to-0.5: 21.24 rounds to 21.0.
    codec::buildRemoteTempPacket(out, 21.24f);
    CHECK_INT(out[8], 0xAA);
    // 21.30 rounds to 21.5.
    codec::buildRemoteTempPacket(out, 21.30f);
    CHECK_INT(out[8], 0xAB);  // 21.5*2 + 128 = 171
}

static void test_build_remote_temp_clear() {
    // celsius <= 0 clears the remote sensor: control byte 0, sentinel 0x80.
    uint8_t out[codec::kPacketLen];
    codec::buildRemoteTempPacket(out, 0.0f);
    CHECK_INT(out[5], 0x07);
    CHECK_INT(out[6], 0x00);
    CHECK_INT(out[8], 0x80);
    CHECK_INT(out[21], codec::checksum(out, 21));
}

static void test_build_info_packet() {
    uint8_t out[codec::kPacketLen];
    codec::buildInfoPacket(out, 0x02);
    CHECK_INT(out[0], 0xfc); CHECK_INT(out[1], 0x42); CHECK_INT(out[2], 0x01);
    CHECK_INT(out[3], 0x30); CHECK_INT(out[4], 0x10);
    CHECK_INT(out[5], 0x02);
    CHECK_INT(out[6], 0x00);
    CHECK_INT(out[21], 0x7b);  // hand-computed: 0xFC - (fc+42+01+30+10+02)
    CHECK_INT(out[21], codec::checksum(out, 21));
}

// ============================================================================
// input normalisers
// ============================================================================
static void test_normalisers() {
    CHECK_EQ(codec::normPower("on"), "ON");        // case-insensitive
    CHECK_EQ(codec::normPower("bogus"), "OFF");    // default = first entry
    CHECK_EQ(codec::normMode("cool"), "COOL");
    CHECK_EQ(codec::normMode("xyz"), "HEAT");
    CHECK_EQ(codec::normFan("quiet"), "QUIET");
    CHECK_EQ(codec::normVane("swing"), "SWING");
    CHECK_EQ(codec::normWideVane("<>"), "<>");
    CHECK_EQ(codec::normWideVane("nope"), "<<");

    CHECK_FLOAT(codec::snapIntSetpoint(24.0f), 24.0);  // in-table -> unchanged
    CHECK_FLOAT(codec::snapIntSetpoint(24.4f), 24.4);  // rounds into table -> kept as-is
    CHECK_FLOAT(codec::snapIntSetpoint(5.0f), 31.0);   // out of range -> default (TEMP_MAP[0])
}

int main() {
    test_checksum_known_answer();

    test_frame_basic();
    test_frame_zero_length();
    test_frame_max_length();
    test_frame_len_too_big();
    test_frame_bad_header();
    test_frame_bad_checksum_and_recovery();
    test_frame_garbage_before_start();
    test_frame_back_to_back();
    test_frame_embedded_fc_in_payload();
    test_frame_fc_as_type_then_recover();
    test_frame_split_across_pushes();

    test_decode_settings_golden();
    test_decode_settings_isee_offset();
    test_decode_settings_half_degree();
    test_decode_settings_widevane_adj();
    test_decode_settings_target_humidity();
    test_decode_settings_unknown_bytes_fallback();
    test_decode_settings_short_frame_oob_safe();
    test_decode_room_temp_golden();
    test_decode_room_temp_half_degree_and_extended();
    test_decode_operating_golden();
    test_decode_standby_golden();
    test_decode_error_golden();
    test_end_to_end_wire_to_value();

    test_build_set_power_only();
    test_build_set_multi_field();
    test_build_set_widevane();
    test_build_set_tempmode();
    test_build_set_no_change();
    test_build_remote_temp();
    test_build_remote_temp_clear();
    test_build_info_packet();

    test_normalisers();

    if (g_failures == 0) {
        std::printf("OK - all host CN105 codec tests passed\n");
        return 0;
    }
    std::printf("%d host CN105 codec test(s) FAILED\n", g_failures);
    return 1;
}
