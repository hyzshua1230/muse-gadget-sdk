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

#include "voice_board.h"
#include "easyinput_board.h"
#include <stdatomic.h>
#include <limits.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 硬件依据：AI Keyboard V2.1 原理图 U12(ZTS6672 SELECT=VDD)，
 * U13(MAX98357A /SD=V_SPK)。独立 I2S 主机，右槽，GPIO8 由板级模块持有。
 * 首轮资格测试使用 48kHz / 32bit stereo，右槽高16位 -> 16kHz PCM16。
 * 原生采样参数和格式必须在实机验收中确认，不自动切换失败配置。 */
#define PIN_DIAL_A 17
#define PIN_DIAL_B 16
#define DIAL_HW_LIMIT INT16_MAX
#define DIAL_LIMIT 1000
#define DIAL_COUNTS 4
#define MIC_FRAMES 960
static pcnt_unit_handle_t s_dial;
static int s_dial_base;
static i2s_chan_handle_t s_mic, s_spk;
static int32_t *s_raw;
static int32_t s_out[192 * 2];
static atomic_int s_volume;
static atomic_bool s_amp;
static size_t s_written_frames;
static int s_output_peak, s_last_gain = -1;
static const char *TAG = "link.voice_board";
static esp_err_t dial_init(void) {
    pcnt_unit_config_t unit_cfg = {
        .low_limit = -DIAL_HW_LIMIT,
        .high_limit = DIAL_HW_LIMIT,
    };
    pcnt_unit_handle_t unit;
    esp_err_t err = pcnt_new_unit(&unit_cfg, &unit);
    if (err != ESP_OK) return err;
    pcnt_glitch_filter_config_t filter = {.max_glitch_ns = 1000};
    pcnt_chan_config_t a_cfg = {.edge_gpio_num = PIN_DIAL_A, .level_gpio_num = PIN_DIAL_B};
    pcnt_chan_config_t b_cfg = {.edge_gpio_num = PIN_DIAL_B, .level_gpio_num = PIN_DIAL_A};
    pcnt_channel_handle_t a, b;
    err = pcnt_unit_set_glitch_filter(unit, &filter);
    if (err == ESP_OK) err = pcnt_new_channel(unit, &a_cfg, &a);
    if (err == ESP_OK) err = pcnt_new_channel(unit, &b_cfg, &b);
    if (err == ESP_OK) {
        pcnt_channel_set_edge_action(a, PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                     PCNT_CHANNEL_EDGE_ACTION_INCREASE);
        pcnt_channel_set_level_action(a, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                      PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
        pcnt_channel_set_edge_action(b, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                     PCNT_CHANNEL_EDGE_ACTION_DECREASE);
        pcnt_channel_set_level_action(b, PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                      PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
        gpio_pullup_en(PIN_DIAL_A);
        gpio_pullup_en(PIN_DIAL_B);
        err = pcnt_unit_enable(unit);
    }
    if (err == ESP_OK) err = pcnt_unit_clear_count(unit);
    if (err == ESP_OK) err = pcnt_unit_start(unit);
    if (err == ESP_OK) s_dial = unit;
    return err;
}

int voice_board_dial_steps(void) {
    int count;
    if (!s_dial || pcnt_unit_get_count(s_dial, &count) != ESP_OK) return 0;
    // 只返回完整格数，半格余数保留到下次读取。
    int steps = (count - s_dial_base) / DIAL_COUNTS;
    s_dial_base += steps * DIAL_COUNTS;
    if (count > DIAL_LIMIT || count < -DIAL_LIMIT) {
        pcnt_unit_clear_count(s_dial);
        s_dial_base -= count;
    }
    return steps;
}


esp_err_t voice_board_init(void) {
    s_raw = heap_caps_malloc(MIC_FRAMES * 2 * sizeof(int32_t), MALLOC_CAP_SPIRAM);
    if (!s_raw) return ESP_ERR_NO_MEM;
    esp_err_t err = dial_init();
    if (err != ESP_OK) return err;
    i2s_chan_config_t mic = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    mic.dma_desc_num = 6;
    mic.dma_frame_num = 240;
    i2s_std_config_t cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = 9, .ws = 10,
                     .dout = I2S_GPIO_UNUSED, .din = 11},
    };
    err = i2s_new_channel(&mic, NULL, &s_mic);
    if (err == ESP_OK) err = i2s_channel_init_std_mode(s_mic, &cfg);
    if (err != ESP_OK) return err;
    i2s_chan_config_t spk = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    spk.auto_clear = true;
    cfg.gpio_cfg.bclk = 14;
    cfg.gpio_cfg.ws = 13;
    cfg.gpio_cfg.dout = 15;
    cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
    err = i2s_new_channel(&spk, &s_spk, NULL);
    if (err == ESP_OK) err = i2s_channel_init_std_mode(s_spk, &cfg);
    if (err == ESP_OK) err = i2s_channel_enable(s_spk);
    if (err == ESP_OK) ESP_LOGI(TAG, "audio ready: mic I2S0 right 48k/32 -> 16k/16; speaker I2S1 48k/32");
    return err;
}

/* 低通 FIR 后每3帧抽取一次；历史跨 DMA 块保留，避免块边界跳变。 */
static const int16_t s_fir[31] = {
    51, 17, -58, -146, -134, 84, 426, 555, 114, -838, -1592, -1105, 1213, 4834, 8186, 9554, 8186, 4834, 1213, -1105, -1592, -838, 114, 555, 426, 84, -134, -146, -58, 17, 51
};
static int16_t s_history[31];
static unsigned s_position, s_phase;

esp_err_t voice_board_mic_start(void) {
    memset(s_history, 0, sizeof(s_history));
    s_position = s_phase = 0;
    return i2s_channel_enable(s_mic);
}
void voice_board_mic_stop(void) { if (s_mic) i2s_channel_disable(s_mic); }
size_t voice_board_mic_read(int16_t *pcm, size_t frames, int *peak) {
    *peak = 0;
    if (frames > MIC_FRAMES / 3) frames = MIC_FRAMES / 3;
    size_t bytes = 0;
    esp_err_t err = i2s_channel_read(s_mic, s_raw, frames * 3 * 8, &bytes, 100);
    if (err != ESP_OK || bytes % 8) {
        ESP_LOGE(TAG, "microphone read failed: %s (%u bytes)", esp_err_to_name(err), (unsigned)bytes);
        return 0;
    }
    size_t out = 0;
    for (size_t i = 0; i < bytes / 8; ++i) {
        s_history[s_position] = (int16_t)(s_raw[2*i+1] >> 16);
        s_position = (s_position + 1) % 31;
        if (++s_phase != 3) continue;
        s_phase = 0;
        int64_t sum = 0;
        for (unsigned k = 0; k < 31; ++k)
            sum += (int32_t)s_history[(s_position + 30 - k) % 31] * s_fir[k];
        int value = (int)(sum / 32768);
        if (value > 32767) value = 32767;
        if (value < -32768) value = -32768;
        pcm[out++] = value;
        int amplitude = value < 0 ? -value : value;
        if (amplitude > *peak) *peak = amplitude;
    }
    return out;
}

esp_err_t voice_board_speaker_write(const int32_t *frames, size_t count) {
    while (count) {
        size_t n = count > 192 ? 192 : count, written = 0;
        int gain = atomic_load(&s_amp) && !easyinput_board_muted() ? atomic_load(&s_volume) : 0;
        if (gain != s_last_gain) {
            ESP_LOGI(TAG, "speaker gain=%d amp=%d muted=%d", gain,
                     (int)atomic_load(&s_amp), (int)easyinput_board_muted());
            s_last_gain = gain;
        }
        for (size_t i = 0; i < n*2; ++i) {
            s_out[i] = (int64_t)frames[i] * gain / 100;
            int value = s_out[i] / 65536;
            int amplitude = value < 0 ? -value : value;
            if (amplitude > s_output_peak) s_output_peak = amplitude;
        }
        esp_err_t err = i2s_channel_write(s_spk, s_out, n*8, &written, 100);
        if (err != ESP_OK || written != n*8) {
            ESP_LOGE(TAG, "speaker write failed: %s (%u/%u bytes)",
                     esp_err_to_name(err), (unsigned)written, (unsigned)(n*8));
            return err != ESP_OK ? err : ESP_ERR_TIMEOUT;
        }
        frames += n*2;
        count -= n;
        s_written_frames += n;
    }
    return ESP_OK;
}
void voice_board_amp(bool on) {
    atomic_store(&s_amp, on);
    if (on) {
        s_written_frames = 0;
        s_output_peak = 0;
        s_last_gain = -1;
        ESP_LOGI(TAG, "speaker begin: volume=%d muted=%d", (int)atomic_load(&s_volume), (int)easyinput_board_muted());
    } else {
        ESP_LOGI(TAG, "speaker end: written_frames=%u output_peak=%d", (unsigned)s_written_frames, s_output_peak);
    }
}
void voice_board_set_volume(int percent) { atomic_store(&s_volume, percent); }
bool voice_board_muted(void) { return easyinput_board_muted(); }
