#include "net_receiver.h"
#include "audio_data.h"
#include "demo_signal.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <cstring>

static const char *TAG      = "net_rx";
static const int   UDP_PORT = 4210;
static std::atomic<bool> s_running{false};

// Validates magic byte (0xAB), version byte (1), exact packet length (1072),
// and fft_bins field (256) before the packet is accepted for queue write.
static bool packet_valid(const uint8_t *buf, int len)
{
    return len == (int)sizeof(AudioPacket)
        && buf[0] == 0xAB
        && buf[1] == 1
        && reinterpret_cast<const AudioPacket*>(buf)->fft_bins == 256;
}

// UDP receive loop — runs on Core 0 at priority 5.
// Uses xQueueOverwrite so screens always get the most recent packet.
// Drop detection compares seq field against last received seq; skips check
// when last_seq == 0 (first packet). Logs drop rate every 300 packets (~10s
// at 30 Hz).
static void udp_task(void *)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed: %d", errno);
        s_running = false;
        vTaskDelete(nullptr);
        return;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(UDP_PORT);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind() failed: %d", errno);
        close(sock);
        s_running = false;
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG, "Listening on UDP :%d", UDP_PORT);

    uint8_t    buf[sizeof(AudioPacket)];
    uint32_t   last_seq   = 0;
    uint32_t   drop_count = 0;
    uint32_t   pkt_count  = 0;

    while (true) {
        struct sockaddr_in src = {};
        socklen_t src_len = sizeof(src);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr*)&src, &src_len);

        if (len < 0) {
            ESP_LOGW(TAG, "recvfrom error %d", errno);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (!packet_valid(buf, len)) {
            ESP_LOGD(TAG, "invalid packet (len=%d magic=0x%02X)", len, buf[0]);
            continue;
        }

        const AudioPacket *pkt = reinterpret_cast<const AudioPacket*>(buf);
        if (pkt->seq != last_seq + 1 && last_seq != 0)
            drop_count++;
        last_seq = pkt->seq;

        xQueueOverwrite(g_audio_queue, pkt);
        demo_signal_notify_packet();  // suppress demo auto-mode while real signal present

        // Log drop rate every ~10 seconds (300 packets at 30 Hz)
        if (++pkt_count % 300 == 0) {
            ESP_LOGI(TAG, "pkts=%lu drops=%lu (%.1f%%)",
                     pkt_count, drop_count,
                     100.0f * drop_count / pkt_count);
        }
    }
}

// Idempotent: only starts the task if it is not already running.
// Safe to call from WiFi event handler on reconnect — s_running prevents
// spawning a second task if one is already blocked in recvfrom().
void net_receiver_start()
{
    if (s_running.exchange(true)) return;
    xTaskCreatePinnedToCore(udp_task, "udp_rx", 4096, nullptr, 5, nullptr, 0);
}
