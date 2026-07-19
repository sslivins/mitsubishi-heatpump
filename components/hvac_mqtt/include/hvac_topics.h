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

}  // namespace hvac_mqtt
