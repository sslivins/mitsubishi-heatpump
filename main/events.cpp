/// @file events.cpp
/// @brief Curated device activity log — implementation.

#include "events.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "nvs.h"

namespace {

const char* TAG = "events";

// Persisted log lives on the (otherwise unused) SPIFFS `storage` partition.
constexpr const char* kBase    = "/spiffs";
constexpr const char* kPart    = "storage";
constexpr const char* kFile    = "/spiffs/events.jsonl";
constexpr const char* kFileOld = "/spiffs/events.1.jsonl";
constexpr long        kMaxBytes = 128 * 1024;  // rotate current file past this

// NVS backup for the monotonic sequence counter (survives a file clear/rotate).
constexpr const char* kNs      = "events";
constexpr const char* kSeqKey  = "seq";

// A signed epoch beyond this means the wall clock has really been set by SNTP
// (2023-11-14). Before that, timestamps are "not yet synced".
constexpr time_t kClockValidAfter = 1700000000;

// RAM ring backing the web UI's Activity view. Sized so a typical overnight of
// activity is served without touching the file. ~114 bytes each ⇒ ~11 KB.
constexpr int kRing = 96;

struct Rec {
    uint32_t seq   = 0;
    uint32_t ts    = 0;       // wall-clock epoch, or 0 if not synced at emit
    uint32_t up_ms = 0;       // uptime milliseconds at emit
    uint8_t  cat   = 0;
    uint8_t  actor = 0;
    char     msg[80]  = {0};
    char     name[32] = {0};
};

std::mutex g_mtx;
Rec        g_ring[kRing];
int        g_head    = 0;     // next write slot
int        g_count   = 0;
uint32_t   g_seq     = 0;     // last-used sequence id
bool       g_mounted = false;

uint32_t now_up_ms() { return (uint32_t)(esp_timer_get_time() / 1000); }

void persist_seq(uint32_t seq) {
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, kSeqKey, seq);
        nvs_commit(h);
        nvs_close(h);
    }
}

uint32_t load_seq() {
    uint32_t v = 0;
    nvs_handle_t h;
    if (nvs_open(kNs, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, kSeqKey, &v);
        nvs_close(h);
    }
    return v;
}

// Build a JSON object for one record. Caller owns the returned cJSON*.
cJSON* rec_to_json(const Rec& r) {
    cJSON* j = cJSON_CreateObject();
    if (!j) return nullptr;
    cJSON_AddNumberToObject(j, "seq", r.seq);
    cJSON_AddNumberToObject(j, "ts", r.ts);
    cJSON_AddNumberToObject(j, "up", (double)r.up_ms);
    cJSON_AddNumberToObject(j, "cat", r.cat);
    cJSON_AddNumberToObject(j, "act", r.actor);
    cJSON_AddStringToObject(j, "msg", r.msg);
    if (r.name[0]) cJSON_AddStringToObject(j, "name", r.name);
    return j;
}

void ring_push(const Rec& r) {
    g_ring[g_head] = r;
    g_head = (g_head + 1) % kRing;
    if (g_count < kRing) g_count++;
}

// Append one record as a JSON line to the persisted file, rotating first if the
// current file has grown past the cap. Best-effort: on any FS error we keep the
// event in the RAM ring and carry on.
void append_line(const Rec& r) {
    if (!g_mounted) return;
    struct stat st;
    if (stat(kFile, &st) == 0 && st.st_size > kMaxBytes) {
        remove(kFileOld);
        rename(kFile, kFileOld);
    }
    FILE* f = fopen(kFile, "a");
    if (!f) { ESP_LOGW(TAG, "append: fopen failed"); return; }
    cJSON* j = rec_to_json(r);
    if (j) {
        char* s = cJSON_PrintUnformatted(j);
        if (s) { fprintf(f, "%s\n", s); cJSON_free(s); }
        cJSON_Delete(j);
    }
    fclose(f);
}

// Parse one persisted JSON line into a Rec (best-effort). Returns false if the
// line isn't a usable event object.
bool parse_line(const char* line, Rec& out) {
    cJSON* j = cJSON_Parse(line);
    if (!j) return false;
    bool ok = false;
    const cJSON* seq = cJSON_GetObjectItem(j, "seq");
    if (cJSON_IsNumber(seq)) {
        out = Rec{};
        out.seq   = (uint32_t)seq->valuedouble;
        const cJSON* ts  = cJSON_GetObjectItem(j, "ts");
        const cJSON* up  = cJSON_GetObjectItem(j, "up");
        const cJSON* cat = cJSON_GetObjectItem(j, "cat");
        const cJSON* act = cJSON_GetObjectItem(j, "act");
        const cJSON* msg = cJSON_GetObjectItem(j, "msg");
        const cJSON* nm  = cJSON_GetObjectItem(j, "name");
        if (cJSON_IsNumber(ts))  out.ts    = (uint32_t)ts->valuedouble;
        if (cJSON_IsNumber(up))  out.up_ms = (uint32_t)up->valuedouble;
        if (cJSON_IsNumber(cat)) out.cat   = (uint8_t)cat->valueint;
        if (cJSON_IsNumber(act)) out.actor = (uint8_t)act->valueint;
        if (cJSON_IsString(msg) && msg->valuestring)
            strlcpy(out.msg, msg->valuestring, sizeof(out.msg));
        if (cJSON_IsString(nm) && nm->valuestring)
            strlcpy(out.name, nm->valuestring, sizeof(out.name));
        ok = true;
    }
    cJSON_Delete(j);
    return ok;
}

// Load one persisted file's lines into the ring (chronological order), tracking
// the highest sequence id seen.
void load_file(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        Rec r;
        if (parse_line(line, r)) {
            ring_push(r);
            if (r.seq > g_seq) g_seq = r.seq;
        }
    }
    fclose(f);
}

}  // namespace

namespace events {

void init() {
    esp_vfs_spiffs_conf_t conf = {};
    conf.base_path              = kBase;
    conf.partition_label        = kPart;
    conf.max_files              = 4;
    conf.format_if_mount_failed = true;
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    g_mounted = (err == ESP_OK);
    if (!g_mounted) {
        ESP_LOGW(TAG, "SPIFFS mount failed (%s); log is RAM-only this boot",
                 esp_err_to_name(err));
    }

    std::lock_guard<std::mutex> lock(g_mtx);
    g_seq = load_seq();
    if (g_mounted) {
        // Rebuild the ring from the persisted tail (old file first, then
        // current, so the newest events end up resident in the ring).
        load_file(kFileOld);
        load_file(kFile);
    }
    ESP_LOGI(TAG, "ready: %d event(s) resident, seq=%lu, persisted=%d",
             g_count, (unsigned long)g_seq, (int)g_mounted);
}

void log(Cat cat, Actor actor, const std::string& msg, const std::string& name) {
    Rec r;
    r.cat   = (uint8_t)cat;
    r.actor = (uint8_t)actor;
    r.up_ms = now_up_ms();
    time_t now = time(nullptr);
    r.ts = (now > kClockValidAfter) ? (uint32_t)now : 0;
    strlcpy(r.msg, msg.c_str(), sizeof(r.msg));
    if (!name.empty()) strlcpy(r.name, name.c_str(), sizeof(r.name));

    std::lock_guard<std::mutex> lock(g_mtx);
    r.seq = ++g_seq;
    ring_push(r);
    append_line(r);
    persist_seq(g_seq);
    ESP_LOGI(TAG, "#%lu [%u/%u] %s%s%s", (unsigned long)r.seq, r.cat, r.actor,
             r.msg, r.name[0] ? " :: " : "", r.name);
}

std::string get_json(uint32_t since, uint32_t limit) {
    if (limit == 0 || limit > (uint32_t)kRing) limit = kRing;

    cJSON* root = cJSON_CreateObject();
    cJSON* arr  = cJSON_AddArrayToObject(root, "events");

    time_t now = time(nullptr);
    bool clock_valid = now > kClockValidAfter;
    cJSON_AddNumberToObject(root, "now_epoch", clock_valid ? (double)now : 0);
    cJSON_AddBoolToObject(root, "clock_valid", clock_valid);

    {
        std::lock_guard<std::mutex> lock(g_mtx);
        cJSON_AddNumberToObject(root, "seq", (double)g_seq);
        uint32_t emitted = 0;
        // Walk newest → oldest.
        for (int i = 0; i < g_count && emitted < limit; ++i) {
            int idx = (g_head - 1 - i + kRing * 2) % kRing;
            const Rec& r = g_ring[idx];
            if (r.seq <= since) break;  // ring is ordered; nothing older matters
            cJSON* j = rec_to_json(r);
            if (j) { cJSON_AddItemToArray(arr, j); emitted++; }
        }
    }

    char* s = cJSON_PrintUnformatted(root);
    std::string out = s ? s : "{}";
    if (s) cJSON_free(s);
    cJSON_Delete(root);
    return out;
}

void clear() {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_mounted) {
        remove(kFile);
        remove(kFileOld);
    }
    g_head = 0;
    g_count = 0;
    // Keep g_seq monotonic across a clear so ids never repeat.
    persist_seq(g_seq);
    ESP_LOGI(TAG, "cleared (seq kept at %lu)", (unsigned long)g_seq);
}

const char* file_path() { return kFile; }

}  // namespace events
