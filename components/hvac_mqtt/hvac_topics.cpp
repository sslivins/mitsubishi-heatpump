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

}  // namespace hvac_mqtt
