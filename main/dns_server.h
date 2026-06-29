/// @file dns_server.h
/// @brief Minimal captive-portal DNS server.
///
/// Answers every DNS A query with 192.168.4.1 (the SoftAP gateway) so phones
/// and laptops auto-detect the captive portal and open the setup page. Runs as
/// a FreeRTOS task on UDP port 53. Ported from arctic-sniffer.

#pragma once

/// Start the captive-portal DNS server (call once, after the SoftAP is up).
void start_dns_server();
