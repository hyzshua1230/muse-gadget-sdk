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

#include "easyinput_board.h"
#include <stdatomic.h>
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static atomic_bool s_muted;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cal;
static SemaphoreHandle_t s_lock;
static bool s_ready;
static atomic_int s_battery_mv;
static void battery_task(void *arg);

esp_err_t easyinput_board_init(void) {
    if (s_ready) return ESP_OK;
    /* GPIO8 是唯一共享电源控制点，音频播放结束不得关闭它。 */
    const uint64_t signals = (1ULL << 9) | (1ULL << 10) | (1ULL << 12) |
                             (1ULL << 13) | (1ULL << 14) | (1ULL << 15);
    gpio_set_level(GPIO_NUM_8, 0);
    gpio_config_t cfg = {.pin_bit_mask = 1ULL << 8, .mode = GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), "link.easyinput", "power GPIO");
    for (int pin = 9; pin <= 15; ++pin) if (pin != 11) gpio_set_level(pin, 0);
    gpio_set_level(GPIO_NUM_5, 0);
    gpio_set_level(GPIO_NUM_42, 0);
    cfg.pin_bit_mask = signals | (1ULL << 5) | (1ULL << 42);
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), "link.easyinput", "outputs");
    gpio_set_level(GPIO_NUM_5, 0);
    gpio_set_level(GPIO_NUM_42, 0);
    cfg.pin_bit_mask = 1ULL << 11;
    cfg.mode = GPIO_MODE_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), "link.easyinput", "microphone input");
    cfg.pin_bit_mask = (1ULL << 39) | (1ULL << 40);
    cfg.mode = GPIO_MODE_INPUT;
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), "link.easyinput", "power inputs");
    ESP_RETURN_ON_ERROR(gpio_set_level(GPIO_NUM_8, 1), "link.easyinput", "power on");
    /* 20ms 为首轮资格测试参数，尚非经过示波器确认的板级最短时间。 */
    vTaskDelay(pdMS_TO_TICKS(20));
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    adc_oneshot_unit_init_cfg_t unit = {.unit_id = ADC_UNIT_1};
    esp_err_t err = adc_oneshot_new_unit(&unit, &s_adc);
    adc_oneshot_chan_cfg_t chan = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    if (err == ESP_OK) err = adc_oneshot_config_channel(s_adc, ADC_CHANNEL_3, &chan);
    adc_cali_curve_fitting_config_t cal = {
        .unit_id = ADC_UNIT_1, .chan = ADC_CHANNEL_3,
        .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (err == ESP_OK) err = adc_cali_create_scheme_curve_fitting(&cal, &s_cal);
    if (err != ESP_OK) ESP_LOGW("link.easyinput", "battery voltage unavailable: %s", esp_err_to_name(err));
    s_ready = true;
    if (s_adc && s_cal && xTaskCreate(battery_task, "battery", 3072, NULL, 1, NULL) != pdPASS)
        ESP_LOGW("link.easyinput", "battery task unavailable");
    ESP_LOGI("link.easyinput", "shared rail ready; battery calibration %s", s_cal ? "available" : "unavailable");
    return ESP_OK;
}

void easyinput_board_green(bool ready) { gpio_set_level(GPIO_NUM_42, ready); }
bool easyinput_board_muted(void) { return atomic_load(&s_muted); }
void easyinput_board_set_muted(bool muted) { atomic_store(&s_muted, muted); }

void easyinput_board_power(int *millivolts, bool *external, bool *charging) {
    *millivolts = atomic_load(&s_battery_mv);
    *external = gpio_get_level(GPIO_NUM_40) == 0;
    *charging = *external && gpio_get_level(GPIO_NUM_39) != 0;
}

static void battery_task(void *arg) {
  for (;;) {
    int measured_mv = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    gpio_set_level(GPIO_NUM_5, 1);
    /* 资格测试等待值，板级模拟电路稳定时间仍须真机验证。 */
    vTaskDelay(pdMS_TO_TICKS(20));
    int total = 0, raw = 0, mv = 0;
    bool valid = true;
    for (int i = 0; i < 16; ++i) {
        if (adc_oneshot_read(s_adc, ADC_CHANNEL_3, &raw) != ESP_OK) {
            valid = false;
            break;
        }
        total += raw;
    }
    if (valid && adc_cali_raw_to_voltage(s_cal, total / 16, &mv) == ESP_OK)
        measured_mv = mv * 2;
    gpio_set_level(GPIO_NUM_5, 0);
    xSemaphoreGive(s_lock);
    atomic_store(&s_battery_mv, measured_mv);
    ESP_LOGI("link.easyinput", "battery=%dmV external=%d charging=%d (precision unverified)",
             measured_mv, gpio_get_level(GPIO_NUM_40) == 0,
             gpio_get_level(GPIO_NUM_40) == 0 && gpio_get_level(GPIO_NUM_39) != 0);
    vTaskDelay(pdMS_TO_TICKS(10000));
  }
}
