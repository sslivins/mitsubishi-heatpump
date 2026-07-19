/// @file hvac_topics.cpp
/// @brief Implementation of the pure HA topic helpers declared in hvac_topics.h.
/// No ESP-IDF dependencies — host-testable.

#include "hvac_topics.h"

namespace hvac_mqtt {

std::string slugify(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    bool prev_sep = false;
    for (char c : in) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (ok) { out.push_back(c); prev_sep = false; }
        else if (!prev_sep) { out.push_back('_'); prev_sep = true; }
    }
    const size_t b = out.find_first_not_of('_');
    if (b == std::string::npos) return "heatpump";  // nothing usable -> safe default
    const size_t e = out.find_last_not_of('_');
    return out.substr(b, e - b + 1);
}

std::string discovery_object_id(const std::string& device_uid) {
    return "mitsubishi-heatpump-" + device_uid;
}

namespace {
std::string to_lower(const std::string& in) {
    std::string out(in);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}
std::string to_upper(const std::string& in) {
    std::string out(in);
    for (char& c : out)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}
}  // namespace

std::string fan_device_to_ha(const std::string& device_fan) {
    const std::string f = to_lower(device_fan);
    if (f == "quiet") return "diffuse";
    if (f == "1")     return "low";
    if (f == "2")     return "middle";
    if (f == "3")     return "medium";
    if (f == "4")     return "high";
    if (f.empty())    return "auto";
    return f;  // "auto" and anything unexpected pass through lower-cased
}

std::string fan_ha_to_device(const std::string& ha_fan) {
    const std::string f = to_lower(ha_fan);
    if (f == "diffuse") return "QUIET";
    if (f == "low")     return "1";
    if (f == "middle")  return "2";
    if (f == "medium")  return "3";
    if (f == "high")    return "4";
    if (f == "auto")    return "AUTO";
    return to_upper(ha_fan);  // already-native value passes through upper-cased
}

}  // namespace hvac_mqtt
