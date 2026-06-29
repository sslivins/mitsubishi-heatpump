/// @file dns_server.cpp
/// @brief Minimal captive-portal DNS server (implementation).
///
/// Listens on UDP :53 and replies to every A-record query with 192.168.4.1.
/// The implementation copies the query ID + question section into the reply
/// and appends a single A record pointing at the AP IP. Ported verbatim in
/// spirit from arctic-sniffer.

#include "dns_server.h"

#include <string.h>

#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "dns";

// 192.168.4.1 — the default SoftAP gateway.
static const uint8_t AP_IP[4] = {192, 168, 4, 1};

static void dns_task(void* param) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket");
        vTaskDelete(nullptr);
        return;
    }

    struct sockaddr_in saddr = {};
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(53);
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "DNS server listening on :53");

    uint8_t rx[512];
    uint8_t tx[512];

    while (true) {
        struct sockaddr_in client = {};
        socklen_t clen = sizeof(client);
        int n = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr*)&client, &clen);
        if (n < 12) continue;  // too short for a DNS header

        memset(tx, 0, sizeof(tx));

        // Transaction ID
        tx[0] = rx[0];
        tx[1] = rx[1];
        // Flags: standard response, authoritative, no error
        tx[2] = 0x81;  // QR=1, Opcode=0, AA=1
        tx[3] = 0x80;  // RA=1, RCODE=0
        // Question count = 1, Answer count = 1
        tx[4] = 0; tx[5] = 1;
        tx[6] = 0; tx[7] = 1;
        // Authority / Additional = 0
        tx[8] = tx[9] = tx[10] = tx[11] = 0;

        // Copy the question section from the request.
        int pos = 12;
        while (pos < n && rx[pos] != 0) {
            pos += rx[pos] + 1;
        }
        pos++;     // QNAME null terminator
        pos += 4;  // QTYPE + QCLASS
        int qlen = pos - 12;

        if (12 + qlen + 16 > (int)sizeof(tx)) continue;  // safety

        memcpy(tx + 12, rx + 12, qlen);

        // Answer: pointer to question name, type A, class IN, TTL 60, RDATA = IP.
        int apos = 12 + qlen;
        tx[apos++] = 0xC0;  // name pointer to offset 12
        tx[apos++] = 0x0C;
        tx[apos++] = 0x00; tx[apos++] = 0x01;  // Type A
        tx[apos++] = 0x00; tx[apos++] = 0x01;  // Class IN
        tx[apos++] = 0x00; tx[apos++] = 0x00;
        tx[apos++] = 0x00; tx[apos++] = 0x3C;  // TTL 60s
        tx[apos++] = 0x00; tx[apos++] = 0x04;  // RDLENGTH 4
        tx[apos++] = AP_IP[0];
        tx[apos++] = AP_IP[1];
        tx[apos++] = AP_IP[2];
        tx[apos++] = AP_IP[3];

        sendto(sock, tx, apos, 0, (struct sockaddr*)&client, clen);
    }

    close(sock);
    vTaskDelete(nullptr);
}

void start_dns_server() {
    xTaskCreate(dns_task, "dns_srv", 4096, nullptr, 3, nullptr);
}
