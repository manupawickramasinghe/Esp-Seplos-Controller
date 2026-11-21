#include "status_led.h"

#include "bms.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "sdkconfig.h"
#include "settings.h"

__attribute__((unused)) static const char *TAG = "led";

#define LED_GPIO CONFIG_SEPLOS_STATUS_LED_GPIO
#define BTN_GPIO CONFIG_SEPLOS_BOOT_BUTTON_GPIO

static void led_set(bool on)
{
#if LED_GPIO >= 0
#if CONFIG_SEPLOS_STATUS_LED_ACTIVE_LOW
    on = !on;
#endif
    gpio_set_level(LED_GPIO, on);
#else
    (void)on;
#endif
}

bool button_is_pressed(void)
{
#if BTN_GPIO >= 0
    gpio_config_t io = { .pin_bit_mask = 1ULL << BTN_GPIO, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&io);
    /* debounce: must read low for 100 ms */
    for (int i = 0; i < 10; i++) {
        if (gpio_get_level(BTN_GPIO) != 0) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
#else
    return false;
#endif
}

/*
 * Patterns (100 ms ticks):
 *   setup portal      fast blink
 *   no network        slow blink
 *   BMS not answering double flash every 2 s
 *   all good          short flash every 2 s (heartbeat)
 */
static void led_task(void *arg)
{
    (void)arg;
    unsigned tick = 0;
    unsigned held = 0;
    /* Only arm the long-press after the button was seen released once, so a stuck or
     * unpulled pin (or a button still held from boot) cannot cause a reboot loop. */
    bool armed = false;
    for (;;) {
        net_info_t ni;
        net_get_info(&ni);
        bool on;
        if (ni.portal) {
            on = tick % 2 == 0;
        } else if (!ni.connected) {
            on = (tick / 10) % 2 == 0;
        } else if (!bms_any_online()) {
            unsigned t = tick % 20;
            on = t == 0 || t == 3;
        } else {
            on = tick % 20 == 0;
        }
        led_set(on);
        tick++;

#if BTN_GPIO >= 0
        if (gpio_get_level(BTN_GPIO) != 0) {
            armed = true;
            held = 0;
        } else if (armed) {
            if (++held == 50) {
                ESP_LOGW(TAG, "setup button held 5 s -> restarting into setup portal");
                settings_request_portal();
                esp_restart();
            }
        }
#else
        (void)held;
        (void)armed;
#endif
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void status_led_start(void)
{
#if LED_GPIO >= 0
    gpio_config_t io = { .pin_bit_mask = 1ULL << LED_GPIO, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
#endif
    xTaskCreate(led_task, "led", 2560, NULL, 2, NULL);
}
