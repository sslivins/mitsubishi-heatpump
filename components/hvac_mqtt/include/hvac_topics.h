/// @file hvac_topics.h
/// @brief Pure (ESP-IDF-free) helpers for deriving Home Assistant MQTT topic
/// segments. Kept in their own translation unit — with no ESP-IDF dependencies —
/// so this string logic can be unit-tested on the host (see test/host).
#pragma once

#include <string>

namespace hvac_mqtt {

/// Reduce an arbitrary friendly_name to a segment safe to embed in an MQTT
/// topic. Home Assistant's MQTT discovery only accepts a node_id/object_id that
/// matches [a-zA-Z0-9_-]; any other character (notably a space) makes HA
/// *silently drop* the discovery message, so the entity never appears even
/// though the broker connection is healthy. Disallowed characters are mapped to
/// '_', consecutive separators collapse, and leading/trailing '_' are trimmed.
/// Empty or otherwise unusable input falls back to "heatpump".
std::string slugify(const std::string& in);

/// Stable Home Assistant discovery object_id for a device. Keyed on the
/// immutable hardware id (which equals the hostname), NOT the mutable
/// friendly_name: deriving the discovery config topic from the friendly slug
/// meant a rename published a second retained config at a new topic, colliding
/// on unique_id so HA dropped one and the entity disappeared. Using the stable
/// id makes renames update the entity in place.
std::string discovery_object_id(const std::string& device_uid);

/// Translate a Mitsubishi/CN105 native fan value (AUTO, QUIET, 1..4) to the
/// Home Assistant *standard* fan-mode name so the HA frontend renders a proper
/// labelled icon (fan-auto, weather-windy, etc.) instead of a bare number.
/// Mirrors the gysmo38/mitsubishi2MQTT mapping:
///   QUIET->diffuse, 1->low, 2->middle, 3->medium, 4->high, everything
///   else (AUTO, empty, unknown) -> auto. Comparison is case-insensitive; an
///   unrecognised value is passed through lower-cased.
std::string fan_device_to_ha(const std::string& device_fan);

/// Inverse of fan_device_to_ha: translate the HA fan-mode name that comes back
/// on the /fan/set command topic to the native value the heat pump expects.
///   diffuse->QUIET, low->1, middle->2, medium->3, high->4, auto->AUTO. An
///   already-native or unrecognised value is passed through upper-cased so a
///   raw "1".."4"/"AUTO"/"QUIET" still works.
std::string fan_ha_to_device(const std::string& ha_fan);

}  // namespace hvac_mqtt
