#include "dns_server.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "dns";
static TaskHandle_t s_task;
static volatile bool s_run;
static uint32_t s_ip;

/* Build the answer in place: copy question, append one A record pointing at s_ip. */
static int build_reply(uint8_t *buf, int len, int cap)
{
    if (len < 12) {
        return -1;
    }
    uint16_t qdcount = (uint16_t)((buf[4] << 8) | buf[5]);
    if ((buf[2] & 0x80) || qdcount != 1) { /* not a query, or unusual */
        return -1;
    }
    /* walk the QNAME */
    int p = 12;
    while (p < len && buf[p] != 0) {
        if (buf[p] & 0xC0) {
            return -1;
        }
        p += buf[p] + 1;
    }
    if (p + 5 > len) {
        return -1;
    }
    uint16_t qtype = (uint16_t)((buf[p + 1] << 8) | buf[p + 2]);
    int qend = p + 5;

    buf[2] = 0x84 | (buf[2] & 0x01); /* QR=1, AA=1, keep RD */
    buf[3] = 0x80;                   /* RA=1, RCODE=0 */
    buf[6] = 0;
    buf[7] = 0;                      /* ANCOUNT */
    buf[8] = buf[9] = buf[10] = buf[11] = 0;
    len = qend; /* drop any additional records (EDNS) */

    if (qtype != 1 /* A */ && qtype != 255 /* ANY */) {
        return len; /* empty answer, NOERROR */
    }
    if (len + 16 > cap) {
        return -1;
    }
    uint8_t *a = buf + len;
    a[0] = 0xC0; a[1] = 0x0C;             /* pointer to QNAME */
    a[2] = 0x00; a[3] = 0x01;             /* type A */
    a[4] = 0x00; a[5] = 0x01;             /* class IN */
    a[6] = 0; a[7] = 0; a[8] = 0; a[9] = 30; /* TTL 30 s */
    a[10] = 0; a[11] = 4;
    memcpy(a + 12, &s_ip, 4);
    buf[7] = 1;
    return len + 16;
}

static void dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket failed");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind failed");
        close(sock);
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    struct timeval tv = { .tv_sec = 1 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ESP_LOGI(TAG, "captive DNS running");
    uint8_t buf[512];
    while (s_run) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n <= 0) {
            continue;
        }
        int r = build_reply(buf, n, sizeof(buf));
        if (r > 0) {
            sendto(sock, buf, (size_t)r, 0, (struct sockaddr *)&from, fl);
        }
    }
    close(sock);
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t dns_server_start(uint32_t ip)
{
    s_ip = ip;
    if (s_task) {
        s_run = true; /* task still alive (stop requested < 1 s ago): keep it running */
        return ESP_OK;
    }
    s_run = true;
    return xTaskCreate(dns_task, "dns", 3072, NULL, 4, &s_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void dns_server_stop(void)
{
    s_run = false;
}
