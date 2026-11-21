#include "bms.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "seplos_proto.h"

static const char *TAG = "bms";

#define UART_PORT ((uart_port_t)CONFIG_SEPLOS_UART_PORT)
#define RX_BUF 1024
#define ALARM_EVERY_N_CYCLES 2
#define MAX_ATTEMPTS 3

typedef struct {
    seplos_pack_state_t st;
    int64_t last_ok_us;
} pack_slot_t;

static pack_slot_t s_packs[BMS_MAX_PACKS];
static size_t s_pack_count;
static uint16_t s_poll_ms;
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_shutdown_q;
static volatile uint32_t s_cycle;
static TaskHandle_t s_waiters[4];
static char s_shutdown_result[96];

static char s_rx[SEPLOS_MAX_FRAME_CHARS + 32];

/* Send a request and collect one response frame into s_rx. Returns the frame length (0 = none). */
static size_t transact(const char *req, size_t req_len)
{
    uart_flush_input(UART_PORT);
    uart_write_bytes(UART_PORT, req, req_len);
    uart_wait_tx_done(UART_PORT, pdMS_TO_TICKS(200));

    size_t len = 0;
    int64_t deadline = esp_timer_get_time() + (int64_t)CONFIG_SEPLOS_RESPONSE_TIMEOUT_MS * 1000;
    while (esp_timer_get_time() < deadline && len < sizeof(s_rx) - 1) {
        int n = uart_read_bytes(UART_PORT, (uint8_t *)s_rx + len, sizeof(s_rx) - 1 - len, pdMS_TO_TICKS(20));
        if (n <= 0) {
            continue;
        }
        len += (size_t)n;
        s_rx[len] = '\0';
        /* Auto-direction transceivers without RE control echo our own request: skip it. */
        size_t start = 0;
        if (len >= req_len && memcmp(s_rx, req, req_len) == 0) {
            start = req_len;
        }
        const char *soi = memchr(s_rx + start, '~', len - start);
        if (soi && memchr(soi, '\r', len - (size_t)(soi - s_rx))) {
            size_t off = (size_t)(soi - s_rx);
            const char *eoi = memchr(soi, '\r', len - off);
            size_t flen = (size_t)(eoi - soi) + 1;
            memmove(s_rx, soi, flen);
            s_rx[flen] = '\0';
            return flen;
        }
    }
    if (len) {
        ESP_LOGD(TAG, "incomplete response (%u bytes): %.*s", (unsigned)len, (int)len, s_rx);
    }
    return 0;
}

static seplos_err_t query(uint8_t addr, uint8_t cid2, seplos_frame_t *frame)
{
    char req[32];
    size_t req_len = cid2 == SEPLOS_CID2_TELEMETRY ? seplos_build_telemetry_request(req, sizeof req, addr)
                                                   : seplos_build_alarm_request(req, sizeof req, addr);
    seplos_err_t err = SEPLOS_ERR_NO_DATA;
    for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
        size_t n = transact(req, req_len);
        if (n == 0) {
            err = SEPLOS_ERR_NO_DATA;
            continue;
        }
        ESP_LOGD(TAG, "rx %.*s", (int)n - 1, s_rx);
        err = seplos_parse_frame(s_rx, n, frame);
        if (err == SEPLOS_OK && frame->addr != addr) {
            err = SEPLOS_ERR_ADDRESS;
        }
        if (err == SEPLOS_OK) {
            return SEPLOS_OK;
        }
        if (err == SEPLOS_ERR_RTN) {
            ESP_LOGW(TAG, "pack %u: BMS returned RTN 0x%02X for CID2 0x%02X", addr, frame->rtn, cid2);
            return err; /* the BMS understood us and said no: retrying will not help */
        }
        ESP_LOGD(TAG, "pack %u: attempt %d: %s", addr, attempt + 1, seplos_err_str(err));
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    return err;
}

static void do_shutdown(uint8_t addr)
{
    char req[40];
    size_t n = seplos_build_shutdown_request(req, sizeof req, addr);
    ESP_LOGW(TAG, "sending SHUTDOWN to pack %u: %.*s", addr, (int)n - 1, req);
    size_t rn = transact(req, n);
    seplos_frame_t f;
    const char *res;
    if (rn == 0) {
        res = "sent, no answer (BMS may already be off)";
    } else {
        seplos_err_t err = seplos_parse_frame(s_rx, rn, &f);
        res = err == SEPLOS_OK ? "acknowledged by BMS" : seplos_err_str(err);
    }
    snprintf(s_shutdown_result, sizeof s_shutdown_result, "pack %u: %s", addr, res);
    ESP_LOGW(TAG, "shutdown %s", s_shutdown_result);
}

static void handle_shutdown_queue(void)
{
    uint8_t addr;
    while (xQueueReceive(s_shutdown_q, &addr, 0) == pdTRUE) {
        do_shutdown(addr);
    }
}

static void notify_waiters(void)
{
    for (size_t i = 0; i < sizeof(s_waiters) / sizeof(s_waiters[0]); i++) {
        TaskHandle_t t = s_waiters[i];
        if (t) {
            xTaskNotifyGive(t);
        }
    }
}

static void poll_pack(pack_slot_t *slot, bool with_alarms)
{
    static seplos_frame_t frame; /* only used from the BMS task */
    seplos_telemetry_t tele;
    seplos_alarms_t alarms;
    uint8_t addr = slot->st.address;

    seplos_err_t err = query(addr, SEPLOS_CID2_TELEMETRY, &frame);
    if (err == SEPLOS_OK) {
        err = seplos_decode_telemetry(&frame, &tele);
    }
    bool alarms_ok = false;
    if (err == SEPLOS_OK && with_alarms) {
        seplos_err_t aerr = query(addr, SEPLOS_CID2_ALARMS, &frame);
        alarms_ok = aerr == SEPLOS_OK && seplos_decode_alarms(&frame, &alarms) == SEPLOS_OK;
        if (!alarms_ok) {
            ESP_LOGD(TAG, "pack %u: alarm query failed: %s", addr, seplos_err_str(aerr));
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool was_online = slot->st.online;
    if (err == SEPLOS_OK) {
        slot->st.tele = tele;
        slot->st.online = true;
        slot->st.has_data = true;
        slot->st.ok_count++;
        slot->last_ok_us = esp_timer_get_time();
        if (alarms_ok) {
            slot->st.alarms = alarms;
            slot->st.has_alarms = true;
        }
    } else {
        slot->st.online = false;
        slot->st.err_count++;
        slot->st.last_err = err;
    }
    xSemaphoreGive(s_lock);

    if (was_online != (err == SEPLOS_OK)) {
        if (err == SEPLOS_OK) {
            ESP_LOGI(TAG, "pack %u online: %u cells, %u.%02u V, SOC %u.%u %%", addr, tele.cell_count,
                     tele.voltage_cv / 100, tele.voltage_cv % 100, tele.soc_pm / 10, tele.soc_pm % 10);
        } else {
            ESP_LOGW(TAG, "pack %u offline: %s", addr, seplos_err_str(err));
        }
    }
}

static void bms_task(void *arg)
{
    (void)arg;
    uint32_t cycle = 0;
    for (;;) {
        int64_t start = esp_timer_get_time();
        for (size_t i = 0; i < s_pack_count; i++) {
            handle_shutdown_queue();
            poll_pack(&s_packs[i], cycle % ALARM_EVERY_N_CYCLES == 0);
        }
        handle_shutdown_queue();
        cycle++;
        s_cycle = cycle;
        notify_waiters();

        int64_t elapsed_ms = (esp_timer_get_time() - start) / 1000;
        int64_t wait_ms = (int64_t)s_poll_ms - elapsed_ms;
        if (wait_ms < 50) {
            wait_ms = 50;
        }
        /* Wake early for a shutdown request. */
        uint8_t addr;
        if (xQueuePeek(s_shutdown_q, &addr, pdMS_TO_TICKS(wait_ms)) == pdTRUE) {
            handle_shutdown_queue();
        }
    }
}

esp_err_t bms_start(uint8_t first_addr, uint8_t pack_count, uint16_t poll_ms)
{
    if (pack_count == 0 || pack_count > BMS_MAX_PACKS) {
        return ESP_ERR_INVALID_ARG;
    }
    s_lock = xSemaphoreCreateMutex();
    s_shutdown_q = xQueueCreate(4, sizeof(uint8_t));
    s_pack_count = pack_count;
    s_poll_ms = poll_ms;
    for (size_t i = 0; i < pack_count; i++) {
        memset(&s_packs[i], 0, sizeof(s_packs[i]));
        s_packs[i].st.address = (uint8_t)(first_addr + i);
        s_packs[i].st.last_err = SEPLOS_ERR_NO_DATA;
    }

    const uart_config_t cfg = {
        .baud_rate = CONFIG_SEPLOS_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_driver_install(UART_PORT, RX_BUF, 0, 0, NULL, 0);
    if (err == ESP_OK) err = uart_param_config(UART_PORT, &cfg);
    if (err == ESP_OK) {
        err = uart_set_pin(UART_PORT, CONFIG_SEPLOS_UART_TX_GPIO, CONFIG_SEPLOS_UART_RX_GPIO,
                           CONFIG_SEPLOS_RS485_DE_GPIO, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK && CONFIG_SEPLOS_RS485_DE_GPIO >= 0) {
        /* The UART drives RTS (wired to DE+RE) high while transmitting. */
        err = uart_set_mode(UART_PORT, UART_MODE_RS485_HALF_DUPLEX);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "RS485 on UART%d TX=%d RX=%d DE=%d @%d baud, packs %u..%u, every %u ms", CONFIG_SEPLOS_UART_PORT,
             CONFIG_SEPLOS_UART_TX_GPIO, CONFIG_SEPLOS_UART_RX_GPIO, CONFIG_SEPLOS_RS485_DE_GPIO,
             CONFIG_SEPLOS_UART_BAUD, first_addr, first_addr + pack_count - 1, poll_ms);

    if (xTaskCreate(bms_task, "bms", 4096, NULL, 6, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

size_t bms_snapshot(seplos_pack_state_t *out, size_t max)
{
    if (!s_lock) {
        return 0;
    }
    size_t n = s_pack_count < max ? s_pack_count : max;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < n; i++) {
        out[i] = s_packs[i].st;
        out[i].age_ms = s_packs[i].last_ok_us ? (uint32_t)((now - s_packs[i].last_ok_us) / 1000) : 0;
    }
    xSemaphoreGive(s_lock);
    return n;
}

uint32_t bms_wait_cycle(uint32_t last_seen, uint32_t timeout_ms)
{
    if (s_cycle != last_seen) {
        return s_cycle;
    }
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    size_t slot = SIZE_MAX;
    for (size_t i = 0; i < sizeof(s_waiters) / sizeof(s_waiters[0]); i++) {
        if (s_waiters[i] == NULL || s_waiters[i] == me) {
            s_waiters[i] = me;
            slot = i;
            break;
        }
    }
    if (slot == SIZE_MAX) {
        vTaskDelay(pdMS_TO_TICKS(timeout_ms));
        return s_cycle;
    }
    if (s_cycle == last_seen) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(timeout_ms));
    }
    return s_cycle;
}

esp_err_t bms_request_shutdown(uint8_t addr)
{
    bool known = false;
    for (size_t i = 0; i < s_pack_count; i++) {
        known |= s_packs[i].st.address == addr;
    }
    if (!known || !s_shutdown_q) {
        return ESP_ERR_NOT_FOUND;
    }
    snprintf(s_shutdown_result, sizeof s_shutdown_result, "pack %u: queued", addr);
    return xQueueSend(s_shutdown_q, &addr, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

void bms_last_shutdown_result(char *out, size_t size)
{
    strlcpy(out, s_shutdown_result, size);
}

bool bms_any_online(void)
{
    bool any = false;
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < s_pack_count; i++) {
        any |= s_packs[i].st.online;
    }
    xSemaphoreGive(s_lock);
    return any;
}
