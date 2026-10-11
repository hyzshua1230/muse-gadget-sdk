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

#include "voice.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
#include <math.h>
#endif

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "button.h"
#include "config_store.h"
#include "led_status.h"
#include "muse_chat.h"
#include "voice_board.h"
#include "voice_muse_chat.h"
#include "voice_player.h"
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
#include "easyinput_board.h"
static atomic_uint s_cancel_epoch;
static atomic_uint s_pause_epoch;
static atomic_bool s_reply_active, s_paused;
static atomic_bool s_resume_requested;
static size_t s_resume_at;
static unsigned s_turn_epoch;
static const char *s_record_error;

void voice_cancel(void) {
    // 已暂停时重复按S4不清除缓存；朗读前的S4仍取消任务。
#if CONFIG_HOMEHUB_QWEN_TTS
    if (atomic_load(&s_paused)) return;
    if (atomic_load(&s_reply_active) && voice_player_started()) {
        atomic_fetch_add(&s_pause_epoch, 1);
        return;
    }
#endif
    voice_abort();
}

void voice_abort(void) {
    atomic_fetch_add(&s_cancel_epoch, 1);
    atomic_store(&s_paused, false);
    atomic_store(&s_resume_requested, false);
}

void voice_toggle_mute(void) {
    bool muted = !easyinput_board_muted();
    easyinput_board_set_muted(muted);
    if (muted) voice_cancel();
    ESP_LOGI("link.voice", "software mute %s", muted ? "on" : "off");
}

static bool cancelled(void) {
    return s_turn_epoch != atomic_load(&s_cancel_epoch) || easyinput_board_muted();
}
#endif

static const char *TAG = "link.voice";

#define KEY_VOLUME          "voice_volume"
#define DEFAULT_VOLUME      60
// Volume change per detent of the dial.
#define VOLUME_STEP         5
// Store a dialled volume once the dial has rested this long.
#define VOLUME_SAVE_MS      2000
#define DIAL_POLL_MS        20

#define CAPTURE_MAX_MS      15000
#define CAPTURE_MIN_MS      300
#define CAPTURE_CHUNK       (VOICE_MIC_RATE / 50)      // 20 ms
#define CAPTURE_MAX         (VOICE_MIC_RATE * CAPTURE_MAX_MS / 1000)
// Keep listening briefly after release so the last word is not clipped.
#define RELEASE_TAIL_MS     250
#define REPLY_CHUNK         (VOICE_PLAYER_RATE / 50)   // 20 ms

typedef enum { EVT_PRESS, EVT_RELEASE, EVT_REPLAY } voice_evt_t;
typedef struct {
    voice_evt_t kind;
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
    unsigned cancel_epoch;
#endif
} voice_event_t;

static QueueHandle_t s_events;
static atomic_bool s_ready;
static atomic_int s_volume;
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
static atomic_bool s_busy;

void voice_replay(void) {
#if CONFIG_HOMEHUB_QWEN_TTS
    bool idle = false;
    if (!atomic_load(&s_ready) || easyinput_board_muted() ||
        !atomic_compare_exchange_strong(&s_busy, &idle, true)) {
        if (!easyinput_board_muted() && atomic_load(&s_paused) && atomic_load(&s_busy)) {
            atomic_store(&s_resume_requested, true);
            ESP_LOGI("link.voice", "resume requested; waiting for complete audio cache");
        }
        return;
    }
    voice_event_t evt = {.kind = EVT_REPLAY, .cancel_epoch = atomic_load(&s_cancel_epoch)};
    if (xQueueSend(s_events, &evt, 0) != pdTRUE) atomic_store(&s_busy, false);
#endif
}
#endif

static int load_volume(void) {
    char buf[8];
    if (!config_get_str(KEY_VOLUME, buf, sizeof(buf)) || !buf[0]) return DEFAULT_VOLUME;
    int v = atoi(buf);
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

static bool store_volume(int volume) {
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", volume);
    return config_set_str(KEY_VOLUME, buf);
}

// Set the speaker volume and show it on the ring.
static void apply_volume(int volume) {
    atomic_store(&s_volume, volume);
    voice_board_set_volume(volume);
    led_status_show_volume(volume);
}

// Turns the volume with the dial. On an internal-RAM stack: storing the volume
// writes NVS.
static void dial_task(void *arg) {
    int64_t save_at = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(DIAL_POLL_MS));
        int steps = voice_board_dial_steps();
        if (steps) {
            int volume = atomic_load(&s_volume) + steps * VOLUME_STEP;
            apply_volume(volume < 0 ? 0 : volume > 100 ? 100 : volume);
            save_at = esp_timer_get_time() + VOLUME_SAVE_MS * 1000LL;
        } else if (save_at && esp_timer_get_time() >= save_at) {
            save_at = 0;
            int volume = atomic_load(&s_volume);
            if (store_volume(volume)) ESP_LOGI(TAG, "volume %d", volume);
            else ESP_LOGW(TAG, "volume could not be stored");
        }
    }
}

// Did a press arrive? Releases are dropped.
static bool pressed_again(TickType_t wait) {
    voice_event_t evt;
    while (xQueueReceive(s_events, &evt, wait) == pdTRUE) {
        if (evt.kind == EVT_PRESS) {
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
            /* 丢弃取消前排队的请求，新的按下带有新的取消代次。 */
            if (evt.cancel_epoch != atomic_load(&s_cancel_epoch) || easyinput_board_muted()) continue;
            s_turn_epoch = evt.cancel_epoch;
#endif
            return true;
        }
        wait = 0;
    }
    return false;
}

// Stream the microphone into the turn until release (plus a tail) or the
// limit. Returns the number of samples, 0 if the capture failed.
static size_t record(void) {
    static int16_t chunk[CAPTURE_CHUNK];
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
    s_record_error = NULL;
    int capture_peak = 0;
    uint64_t energy = 0;
    size_t clipped = 0;
#endif
    if (voice_board_mic_start() != ESP_OK) return 0;
    size_t samples = 0;
    int64_t stop_at = 0;
    while (samples < CAPTURE_MAX) {
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
        if (cancelled()) break;
#endif
        voice_event_t evt;
        if (!stop_at && xQueueReceive(s_events, &evt, 0) == pdTRUE && evt.kind == EVT_RELEASE) {
            stop_at = esp_timer_get_time() + RELEASE_TAIL_MS * 1000LL;
        }
        if (stop_at && esp_timer_get_time() >= stop_at) break;
        int peak = 0;
        size_t got = voice_board_mic_read(chunk, CAPTURE_CHUNK, &peak);
        if (!got) {
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
            s_record_error = "MICROPHONE READ FAILED";
#endif
            break;
        }
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
        if (cancelled()) break;
#endif
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
        if (muse_hatch_turn_audio_wait(chunk, got, 100) != got) {
            s_record_error = "MICROPHONE BACKLOG FULL";
            break;
        }
        if (peak > capture_peak) capture_peak = peak;
        for (size_t i = 0; i < got; ++i) {
            int value = chunk[i];
            energy += (uint64_t)((int64_t)value * value);
            if (value >= 32767 || value <= -32768) ++clipped;
        }
#else
        muse_hatch_turn_audio(chunk, got);
#endif
        samples += got;
        led_status_set_level(peak / 12000.0f);
    }
    voice_board_mic_stop();
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
    ESP_LOGI(TAG, "mic samples=%u peak=%d mean_square=%llu clipped=%u", (unsigned)samples,
             capture_peak, (unsigned long long)(samples ? energy / samples : 0), (unsigned)clipped);
    if (s_record_error) return 0; // 不提交已经缺失音频的半句录音。
#endif
    if (samples >= CAPTURE_MAX) ESP_LOGI(TAG, "capture limit reached");
    return samples;
}

static bool fail(const char *why) {
    ESP_LOGW(TAG, "turn failed: %s", why);
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
#if CONFIG_HOMEHUB_QWEN_TTS
    muse_hatch_replay_clear();
    atomic_store(&s_paused, false);
    atomic_store(&s_resume_requested, false);
    s_resume_at = 0;
#endif
    unsigned lights = strstr(why, "TTS") ? 2 : strstr(why, "SPEAKER") ? 3 :
        strstr(why, "ASR") || strstr(why, "MICROPHONE") || strstr(why, "CATCH") ? 1 : 5;
    led_status_set_voice_error(lights);
#else
    led_status_set_voice(LED_VOICE_ERROR);
#endif
    return false;
}

#if CONFIG_HOMEHUB_QWEN_TTS && CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
static bool pause_playback(size_t start) {
    voice_player_stop();
    // I2S为6*240帧/48k，尾部最多30ms；播放器120ms尾音等待后再取位置。
    if (!voice_player_wait(1000)) return fail("SPEAKER PAUSE TIMEOUT");
    if (voice_player_error() != ESP_OK) return fail("SPEAKER OUTPUT FAILED");
    if (s_turn_epoch != atomic_load(&s_cancel_epoch)) {
        muse_hatch_turn_cancel();
        muse_hatch_replay_clear();
        atomic_store(&s_paused, false);
        s_resume_at = 0;
        return false;
    }
    s_resume_at = start + voice_player_samples();
    atomic_store(&s_paused, true);
    led_status_set_voice(LED_VOICE_IDLE);
    ESP_LOGI(TAG, "playback paused: sample=%u (%.2fs)", (unsigned)s_resume_at,
             (double)s_resume_at / VOICE_PLAYER_RATE);
    return true;
}
#endif

// Play the reply as it arrives. Returns true if a new press interrupted it.
static bool reply(void) {
    static int16_t pcm[REPLY_CHUNK];
    char text[96];
    bool done = false;
    size_t played = 0;
    int64_t t0 = esp_timer_get_time();
    voice_player_begin();
#if CONFIG_HOMEHUB_QWEN_TTS && CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
    unsigned pause_epoch = atomic_load(&s_pause_epoch);
    bool paused = false;
    atomic_store(&s_reply_active, true);
#endif
    for (;;) {
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
#if CONFIG_HOMEHUB_QWEN_TTS
        bool abort = s_turn_epoch != atomic_load(&s_cancel_epoch);
        if (!paused && !abort && (pause_epoch != atomic_load(&s_pause_epoch) || easyinput_board_muted()) &&
            voice_player_started()) {
            if (!pause_playback(0)) return false;
            paused = true;
        }
        if (abort || (!paused && easyinput_board_muted())) {
#else
        if (cancelled()) {
#endif
            muse_hatch_turn_cancel();
            voice_player_stop();
#if CONFIG_HOMEHUB_QWEN_TTS
            muse_hatch_replay_clear();
            atomic_store(&s_paused, false);
            s_resume_at = 0;
#endif
            led_status_set_voice(LED_VOICE_IDLE);
            return false;
        }
#endif
        if (pressed_again(0)) {
            muse_hatch_turn_cancel();
            voice_player_stop();
#if CONFIG_HOMEHUB_QWEN_TTS && CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
            muse_hatch_replay_clear();
#endif
            return true;
        }
        muse_hatch_ev_t ev;
        while ((ev = muse_hatch_turn_event(text, sizeof(text))) != MUSE_HATCH_EV_NONE) {
            switch (ev) {
            case MUSE_HATCH_EV_HEARD:
                ESP_LOGI(TAG, "heard: %s", text);
                led_status_set_voice(LED_VOICE_THINKING);
                break;
            case MUSE_HATCH_EV_REPLY:
                if (!voice_player_started()
#if CONFIG_HOMEHUB_QWEN_TTS && CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
                    && !paused
#endif
                ) led_status_set_voice(LED_VOICE_BUFFERING);
                break;
            case MUSE_HATCH_EV_DONE:
                done = true;
                break;
            case MUSE_HATCH_EV_ERROR:
                voice_player_stop();
                return fail(text);
            default:
                break;
            }
        }
        // After DONE the reply's audio is all decoded; drain what's left.
        size_t n = muse_hatch_turn_read(pcm, REPLY_CHUNK, done ? 0 : 20);
#if CONFIG_HOMEHUB_QWEN_TTS && CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
        if (paused) {
            // 继续接收已有回复，让会话完成完整缓存，但不向播放器送音频。
            if (!n && done) break;
            continue;
        }
#endif
        if (n) {
            esp_err_t err = voice_player_write(pcm, n);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "player queue write failed: %s", esp_err_to_name(err));
                muse_hatch_turn_cancel();
                voice_player_stop();
                return fail("SPEAKER QUEUE FAILED");
            }
            played += n;
        } else if (done) {
            break;
        }
        if (voice_player_started()) led_status_set_voice(LED_VOICE_SPEAKING);
    }
#if CONFIG_HOMEHUB_QWEN_TTS && CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
    if (paused) {
        muse_hatch_replay_commit();
        ESP_LOGI(TAG, "paused reply complete; replay cache status logged above");
        return false;
    }
#endif
    voice_player_end();
    while (!voice_player_wait(0)) {
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
#if CONFIG_HOMEHUB_QWEN_TTS
        if (s_turn_epoch == atomic_load(&s_cancel_epoch) &&
            (pause_epoch != atomic_load(&s_pause_epoch) || easyinput_board_muted())) {
            if (!pause_playback(0)) return false;
            muse_hatch_replay_commit();
            return false;
        }
#endif
        if (cancelled()) {
            muse_hatch_turn_cancel();
            voice_player_stop();
#if CONFIG_HOMEHUB_QWEN_TTS
            muse_hatch_replay_clear();
#endif
            led_status_set_voice(LED_VOICE_IDLE);
            return false;
        }
#endif
        if (pressed_again(pdMS_TO_TICKS(40))) {
            voice_player_stop();
#if CONFIG_HOMEHUB_QWEN_TTS && CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
            muse_hatch_replay_clear();
#endif
            return true;
        }
        if (voice_player_started()) led_status_set_voice(LED_VOICE_SPEAKING);
    }
    if (voice_player_error() != ESP_OK) return fail("SPEAKER OUTPUT FAILED");
#if CONFIG_HOMEHUB_QWEN_TTS && CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
    if (!cancelled()) muse_hatch_replay_commit();
    else muse_hatch_replay_clear();
    s_resume_at = 0;
    atomic_store(&s_paused, false);
#endif
    ESP_LOGI(TAG, "reply: %.1fs of speech, %.1fs total", (double)played / VOICE_PLAYER_RATE,
             (esp_timer_get_time() - t0) / 1e6);
    led_status_set_voice(LED_VOICE_IDLE);
    return false;
}

// Returns true if a new press interrupted the turn.
static bool run_turn(void) {
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
    atomic_store(&s_reply_active, false);
    atomic_store(&s_resume_requested, false);
#if CONFIG_HOMEHUB_QWEN_TTS
    muse_hatch_replay_clear();
    atomic_store(&s_paused, false);
    s_resume_at = 0;
#endif
    if (cancelled()) return false;
#endif
    voice_player_stop();
    led_status_set_level(0);
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
    /* 等待上轮 DMA 和播放任务退出，避免新录音与旧扬声器尾音重叠。 */
    if (!voice_player_wait(1000)) return fail("speaker did not stop");
#endif
    led_status_set_voice(LED_VOICE_LISTENING);
    muse_hatch_turn_begin();
    size_t samples = record();
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
    if (cancelled()) {
        muse_hatch_turn_cancel();
        led_status_set_voice(LED_VOICE_IDLE);
        ESP_LOGI(TAG, "recording cancelled; not submitted");
        return false;
    }
#endif
    if (samples < VOICE_MIC_RATE * CAPTURE_MIN_MS / 1000) {
        muse_hatch_turn_cancel();
        if (!samples) {
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
            return fail(s_record_error ? s_record_error : "MICROPHONE UNAVAILABLE");
#else
            return fail("microphone unavailable");
#endif
        }
        ESP_LOGI(TAG, "press too short");
        led_status_set_voice(LED_VOICE_IDLE);
        return false;
    }
    ESP_LOGI(TAG, "recorded %.1fs", (double)samples / VOICE_MIC_RATE);
    led_status_set_voice(LED_VOICE_TRANSCRIBING);
    muse_hatch_turn_end();
    return reply();
}

// Runs on the button task. Claims the press only when a turn can run, so the
// button keeps its setup role otherwise.
static bool on_press(bool pressed) {
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
    bool was_busy = atomic_load(&s_busy);
#endif
    if (pressed) {
        if (!atomic_load(&s_ready) || voice_board_muted()) return false;
        voice_hatch_refresh();
        if (!muse_hatch_ready()) return false;
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
        was_busy = atomic_exchange(&s_busy, true);
#endif
    }
    voice_event_t evt = {.kind = pressed ? EVT_PRESS : EVT_RELEASE};
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
    evt.cancel_epoch = atomic_load(&s_cancel_epoch);
#endif
    if (xQueueSend(s_events, &evt, 0) != pdTRUE) {
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
        if (pressed && !was_busy) atomic_store(&s_busy, false);
#endif
        return false;
    }
    return true;
}

#if CONFIG_HOMEHUB_QWEN_TTS && CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
static bool replay(void) {
    int16_t pcm[REPLY_CHUNK];
    size_t start = atomic_load(&s_paused) ? s_resume_at : 0;
    if (!muse_hatch_replay_begin(&start)) {
        ESP_LOGI(TAG, "NO REPLAY AUDIO");
        atomic_store(&s_paused, false);
        s_resume_at = 0;
        led_status_set_voice_error(3);
        return false;
    }
    ESP_LOGI(TAG, "%s begin: sample=%u (%.2fs)", start ? "resume" : "replay", (unsigned)start,
             (double)start / VOICE_PLAYER_RATE);
    atomic_store(&s_reply_active, false);
    unsigned pause_epoch = atomic_load(&s_pause_epoch);
    voice_player_begin();
    atomic_store(&s_paused, false);
    atomic_store(&s_reply_active, true);
    size_t played = 0;
    bool ended = false;
    for (;;) {
        if (s_turn_epoch != atomic_load(&s_cancel_epoch)) {
            voice_player_stop();
            s_resume_at = 0;
            atomic_store(&s_paused, false);
            led_status_set_voice(LED_VOICE_IDLE);
            ESP_LOGI(TAG, "replay cancelled");
            return false;
        }
        if (pause_epoch != atomic_load(&s_pause_epoch) || easyinput_board_muted()) {
            pause_playback(start);
            return false;
        }
        if (pressed_again(0)) {
            voice_player_stop();
            ESP_LOGI(TAG, "replay interrupted by recording");
            return true;
        }
        if (!ended) {
            size_t n = muse_hatch_replay_read(pcm, REPLY_CHUNK);
            if (n) {
                if (voice_player_write(pcm, n) != ESP_OK) {
                    voice_player_stop();
                    ESP_LOGW(TAG, "replay failed: SPEAKER QUEUE FAILED");
                    led_status_set_voice_error(3);
                    return false;
                }
                played += n;
            } else {
                voice_player_end();
                ended = true;
            }
        }
        if (voice_player_error() != ESP_OK) {
            voice_player_stop();
            ESP_LOGW(TAG, "replay failed: SPEAKER OUTPUT FAILED");
            led_status_set_voice_error(3);
            return false;
        }
        if (ended && voice_player_wait(0)) break;
        if (voice_player_started()) led_status_set_voice(LED_VOICE_SPEAKING);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    s_resume_at = 0;
    atomic_store(&s_paused, false);
    led_status_set_voice(LED_VOICE_IDLE);
    ESP_LOGI(TAG, "replay done: %u samples, %.1fs; PSRAM free=%u largest=%u", (unsigned)played,
             (double)played / VOICE_PLAYER_RATE,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    return false;
}
#endif

static void voice_task(void *arg) {
    if (voice_board_init() != ESP_OK || voice_player_init() != ESP_OK) {
        ESP_LOGE(TAG, "audio hardware unavailable; voice chat disabled");
        vTaskSuspend(NULL);
        return;
    }
    voice_board_set_volume(atomic_load(&s_volume));
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
    /* 低电平 120ms 提示音只验证本地播放，文字回复并不等于 TTS。 */
    int16_t *tone = heap_caps_malloc(1920 * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (tone) {
        for (size_t i = 0; i < 1920; ++i)
            tone[i] = (int16_t)(1000 * sinf(2 * 3.14159265f * 800 * i / VOICE_PLAYER_RATE));
        voice_player_begin();
        voice_player_write(tone, 1920);
        voice_player_end();
        voice_player_wait(1000);
        free(tone);
        ESP_LOGI(TAG, "speaker prompt: 800Hz 120ms at volume %d", atomic_load(&s_volume));
    } else ESP_LOGW(TAG, "speaker prompt unavailable: no memory");
#endif
    atomic_store(&s_ready, true);
    if (xTaskCreate(dial_task, "dial", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no memory for the dial");
    }
    ESP_LOGI(TAG, "ready");

    for (;;) {
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
        voice_event_t evt;
        if (xQueueReceive(s_events, &evt, portMAX_DELAY) != pdTRUE) continue;
        if (evt.kind == EVT_RELEASE) continue;
        if (evt.cancel_epoch != atomic_load(&s_cancel_epoch) || easyinput_board_muted()) {
            atomic_store(&s_busy, false);
            continue;
        }
        s_turn_epoch = evt.cancel_epoch;
        bool talk = evt.kind == EVT_PRESS;
#if CONFIG_HOMEHUB_QWEN_TTS
        if (evt.kind == EVT_REPLAY) talk = replay();
#endif
        if (talk) while (run_turn()) {}
        atomic_store(&s_reply_active, false);
        atomic_store(&s_busy, false);
#if CONFIG_HOMEHUB_QWEN_TTS
        if (atomic_exchange(&s_resume_requested, false) && atomic_load(&s_paused) && !easyinput_board_muted())
            voice_replay();
#endif
#else
        if (!pressed_again(portMAX_DELAY)) continue;
        while (run_turn()) {
        }
#endif
    }
}

void voice_init(void) {
    s_events = xQueueCreate(8, sizeof(voice_event_t));
    if (!s_events) {
        ESP_LOGE(TAG, "no memory for voice chat");
        return;
    }
    atomic_store(&s_volume, load_volume());
    voice_hatch_refresh();
    muse_hatch_start();
    // The stack is in PSRAM, so the task must not touch flash (NVS): pairing
    // needs an 8 KB internal block for its TLS task.
    if (xTaskCreateWithCaps(voice_task, "voice", 4096, NULL, 4, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "failed to start voice chat");
        return;
    }
    button_set_press_cb(on_press);
}

cJSON *voice_configure_command(cJSON *params) {
    cJSON *volume = cJSON_GetObjectItem(params, "volume");
    const char *why = NULL;
    if (volume) {
        if (!cJSON_IsNumber(volume) || volume->valueint < 0 || volume->valueint > 100) {
            why = "volume must be 0-100";
        } else {
            if (!store_volume(volume->valueint)) why = "volume could not be stored";
            else if (atomic_load(&s_ready)) apply_volume(volume->valueint);
            else atomic_store(&s_volume, volume->valueint);
        }
    }

    cJSON *result = cJSON_CreateObject();
    if (why) {
        cJSON_AddBoolToObject(result, "ok", false);
        cJSON *error = cJSON_CreateObject();
        cJSON_AddStringToObject(error, "code", "invalid_params");
        cJSON_AddStringToObject(error, "message", why);
        cJSON_AddItemToObject(result, "error", error);
        return result;
    }
    cJSON_AddBoolToObject(result, "ok", true);
    cJSON_AddNumberToObject(result, "volume", atomic_load(&s_volume));
    return result;
}
