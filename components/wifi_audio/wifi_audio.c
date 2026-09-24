#include "wifi_audio.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "lwip/sockets.h"

static const char *TAG = "WIFI_AUDIO";

#define RA_SIZE 8192
#define RA_MASK (RA_SIZE - 1)
#define AUDIO_PORT_DEFAULT 5004
#define DISCOVER_PORT 5005
#define DISCOVER_MAGIC "ESPLED"

static int16_t s_ring[RA_SIZE];
static volatile uint32_t s_head = 0;
static volatile uint32_t s_tail = 0;
static volatile uint32_t s_last_rx_us = 0;
static int s_sock = -1;
static bool s_started = false;
static uint16_t s_audio_port = AUDIO_PORT_DEFAULT;

static bool wifi_audio_sta_ip(esp_ip4_addr_t *out)
{
    esp_netif_t *st = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ii;
    if (st && esp_netif_get_ip_info(st, &ii) == ESP_OK && ii.ip.addr != 0) {
        if (out) *out = ii.ip;
        return true;
    }
    return false;
}

static void wifi_audio_discover_task(void *arg)
{
    int sock = (int)(intptr_t)arg;
    char buf[64];
    while (1) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int len = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &flen);
        if (len <= 0) continue;
        buf[len] = 0;
        if (strncmp(buf, DISCOVER_MAGIC, sizeof(DISCOVER_MAGIC) - 1) != 0) continue;
        esp_ip4_addr_t ip;
        if (!wifi_audio_sta_ip(&ip)) continue;
        char reply[48];
        int rn = snprintf(reply, sizeof(reply), "%s " IPSTR " %d",
                          DISCOVER_MAGIC, IP2STR(&ip), (int)s_audio_port);
        sendto(sock, reply, rn, 0, (struct sockaddr *)&from, flen);
    }
}

bool wifi_audio_get_ip_str(char *out, int out_len)
{
    esp_ip4_addr_t ip;
    if (!wifi_audio_sta_ip(&ip)) return false;
    snprintf(out, out_len, IPSTR, IP2STR(&ip));
    return true;
}

int wifi_audio_get_port(void)
{
    return (int)s_audio_port;
}

static void wifi_audio_rx_task(void *arg)
{
    int sock = (int)(intptr_t)arg;
    uint8_t buf[1024];
    while (1) {
        int len = recv(sock, buf, sizeof(buf), 0);
        if (len <= 0) {
            continue;
        }
        int samples = len / 2;
        const int16_t *pcm = (const int16_t *)buf;
        uint32_t head = s_head;
        for (int i = 0; i < samples; i++) {
            s_ring[head & RA_MASK] = pcm[i];
            head++;
        }
        if (head - s_tail > RA_SIZE) {
            s_tail = head - RA_SIZE;
        }
        s_head = head;
        s_last_rx_us = (uint32_t)(esp_timer_get_time() / 1000);
    }
}

esp_err_t wifi_audio_init(uint16_t port)
{
    if (s_started) {
        return ESP_OK;
    }
    s_audio_port = port;

    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket create failed");
        return ESP_FAIL;
    }

    int reuse = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind failed on port %u", (unsigned)port);
        close(s_sock);
        s_sock = -1;
        return ESP_FAIL;
    }

    s_started = true;
    BaseType_t ok = xTaskCreate(wifi_audio_rx_task, "wifi_audio_rx", 3072,
                                (void *)(intptr_t)s_sock, 12, NULL);
    if (ok != pdPASS) {
        s_started = false;
        return ESP_ERR_NO_MEM;
    }

    int dsock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (dsock >= 0) {
        struct sockaddr_in daddr;
        memset(&daddr, 0, sizeof(daddr));
        daddr.sin_family = AF_INET;
        daddr.sin_port = htons(DISCOVER_PORT);
        daddr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(dsock, (struct sockaddr *)&daddr, sizeof(daddr)) == 0) {
            xTaskCreate(wifi_audio_discover_task, "wifi_audio_dsc", 3072,
                        (void *)(intptr_t)dsock, 11, NULL);
        } else {
            close(dsock);
        }
    }
    ESP_LOGI(TAG, "WiFi audio UDP receiver on port %u (discover on %u)",
             (unsigned)port, (unsigned)DISCOVER_PORT);
    return ESP_OK;
}

int wifi_audio_read(int16_t *out, int n, int timeout_ms)
{
    int waited = 0;
    while ((int)(s_head - s_tail) < n) {
        if (waited >= timeout_ms) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
        waited += 2;
    }

    uint32_t tail = s_tail;
    uint32_t head = s_head;
    int avail = (int)(head - tail);
    if (avail > n) avail = n;

    for (int i = 0; i < avail; i++) {
        out[i] = s_ring[tail & RA_MASK];
        tail++;
    }
    for (int i = avail; i < n; i++) {
        out[i] = 0;
    }
    s_tail = tail;
    return n;
}

bool wifi_audio_streaming(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    return s_started && (now - s_last_rx_us) < 1000000u;
}
