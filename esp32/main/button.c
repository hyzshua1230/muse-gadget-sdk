/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "button.h"
#include "stack_monitor.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#if CONFIG_HOMEHUB_RADIO
#include "radio.h"
#endif
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
#include "voice.h"
#endif


static const char *TAG = "link.button";

#define BTN_GPIO           CONFIG_HOMEHUB_BUTTON_GPIO
#define LONG_PRESS_MS      5000
#define SHORT_PRESS_MAX_MS 1000
#define DOUBLE_CLICK_MS    400
#define POLL_MS            50

static button_cb s_short_press_cb = NULL;
static button_cb s_double_press_cb = NULL;
static button_cb s_long_press_cb = NULL;
#if CONFIG_HOMEHUB_VOICE
static volatile button_press_cb s_press_cb = NULL;

void button_set_press_cb(button_press_cb cb) {
    s_press_cb = cb;
}
#endif

#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
static const int s_keys[9] = {2, 47, 38, 41, 1, 6, 7, 48, 18};
static void button_task(void *arg) {
    bool stable[9] = {0}, candidate[9] = {0}, fired = false;
    unsigned ticks[9] = {0};
    int64_t s8_start = 0, dial_start = 0;
#if CONFIG_HOMEHUB_RADIO
    int64_t s6_start = 0, s7_start = 0;
#endif
    for (;;) {
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < 9; ++i) {
            bool pressed = gpio_get_level(s_keys[i]) == 0;
            if (pressed != candidate[i]) { candidate[i] = pressed; ticks[i] = 1; }
            else if (ticks[i] < 2) ++ticks[i];
            if (ticks[i] < 2 || stable[i] == pressed) continue;
            stable[i] = pressed;
            ESP_LOGI(TAG, "input %s%d %s", i < 8 ? "S" : "encoder", i < 8 ? i+1 : 1,
                     pressed ? "down" : "up");
            if (i == 0) {
                /* S1 永远只属于语音，未认领时也不能变成配对或恢复键。 */
#if CONFIG_HOMEHUB_VOICE
                button_press_cb cb = s_press_cb;
                if (cb) cb(pressed);
#endif
            }
#if CONFIG_HOMEHUB_VOICE
            else if (i == 1 && pressed) voice_toggle_mute();
            else if (i == 2 && pressed) voice_replay();
            else if (i == 3 && pressed) voice_cancel();
            else if (i == 8) {
                if (pressed) dial_start = now;
                else if (now - dial_start <= SHORT_PRESS_MAX_MS * 1000LL)
                    voice_toggle_brightness_mode();
            }
#endif
#if CONFIG_HOMEHUB_RADIO
            else if (i == 5) {
                if (pressed) s6_start = now;
                else if (now-s6_start <= SHORT_PRESS_MAX_MS*1000LL) radio_toggle();
            }
            else if (i == 6) {
                if (pressed) s7_start = now;
                else if (now-s7_start >= 1000000) radio_select_mode();
                else radio_next();
            }
#endif
            else if (i == 7) {
                if (pressed) { s8_start = now; fired = false; }
                else if (!fired && now - s8_start <= SHORT_PRESS_MAX_MS * 1000LL) {
                    if (s_short_press_cb) s_short_press_cb();
                }
            }
        }
        if (stable[7] && !fired && now - s8_start >= LONG_PRESS_MS * 1000LL) {
            fired = true;
#if CONFIG_HOMEHUB_VOICE
            voice_abort();
#endif
            ESP_LOGI(TAG, "S8 reset setup");
            if (s_long_press_cb) s_long_press_cb();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
#else
static void button_task(void *arg) {
    stack_monitor_t stack = STACK_MONITOR_INIT;
    bool was_pressed = false;
    int64_t press_start = 0;
    bool fired = false;
    int click_count = 0;
    int64_t last_release = 0;
#if CONFIG_HOMEHUB_VOICE
    bool claimed = false;
#endif

    while (1) {
        bool pressed = (gpio_get_level(BTN_GPIO) == 0);

        if (pressed && !was_pressed) {
            press_start = esp_timer_get_time();
            fired = false;
#if CONFIG_HOMEHUB_VOICE
            button_press_cb press_cb = s_press_cb;
            claimed = press_cb && press_cb(true);
            if (claimed) {
                // Not a tap or hold, and it ends any pending double click.
                fired = true;
                click_count = 0;
            }
        } else if (!pressed && was_pressed && claimed) {
            claimed = false;
            button_press_cb press_cb = s_press_cb;
            if (press_cb) press_cb(false);
#endif
        } else if (pressed && !fired) {
            int64_t held_ms = (esp_timer_get_time() - press_start) / 1000;
            if (held_ms >= LONG_PRESS_MS) {
                ESP_LOGI(TAG, "long press detected");
                if (s_long_press_cb) s_long_press_cb();
                fired = true;
                click_count = 0;
            }
        } else if (!pressed && was_pressed && !fired) {
            int64_t held_ms = (esp_timer_get_time() - press_start) / 1000;
            if (held_ms >= 50 && held_ms <= SHORT_PRESS_MAX_MS) {
                click_count++;
                last_release = esp_timer_get_time();
            }
        }

        if (!pressed && click_count > 0) {
            int64_t since_release = (esp_timer_get_time() - last_release) / 1000;
            if (since_release >= DOUBLE_CLICK_MS) {
                if (click_count >= 2) {
                    ESP_LOGI(TAG, "double press detected");
                    if (s_double_press_cb) s_double_press_cb();
                } else {
                    ESP_LOGI(TAG, "short press detected");
                    if (s_short_press_cb) s_short_press_cb();
                }
                click_count = 0;
            }
        }

        was_pressed = pressed;
        stack_monitor_poll(&stack);
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

#endif

bool button_init(button_cb on_short_press, button_cb on_double_press,
                 button_cb on_long_press) {
    s_short_press_cb = on_short_press;
    s_double_press_cb = on_double_press;
    s_long_press_cb = on_long_press;

    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BTN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
    cfg.pin_bit_mask = 0;
    for (int i = 0; i < 9; ++i) cfg.pin_bit_mask |= 1ULL << s_keys[i];
#endif
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio config failed: %s", esp_err_to_name(err));
        return false;
    }

    if (xTaskCreate(button_task, "btn", 4096, NULL, 2, NULL) != pdPASS) return false;
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
    ESP_LOGI(TAG, "inputs ready: S1=talk S2=mute S3=resume/replay S4=pause/cancel S8=pair/reset(5s); other keys=diagnostic");
#else
    ESP_LOGI(TAG, "button ready (GPIO %d: tap=retry wifi, 2x=rescan, hold %ds=reset setup)",
             BTN_GPIO, LONG_PRESS_MS / 1000);
#endif
    return true;
}
