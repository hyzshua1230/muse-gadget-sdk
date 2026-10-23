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
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2 && CONFIG_HOMEHUB_QWEN_TTS && !CONFIG_MUSE_ENABLED
#include "story_prompt.h"
#include "briefing_prompt.h"
#endif
#if CONFIG_HOMEHUB_RADIO
#include "radio.h"
#endif
#if CONFIG_HOMEHUB_OFFLINE_WAKE
#include "voice_wake.h"
#include "esp_bt.h"
static bool s_auto_record;
#endif
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
#include "easyinput_board.h"
#include "network_feedback.h"
static atomic_uint s_cancel_epoch;
static atomic_uint s_brightness_clicks;
void voice_toggle_brightness_mode(void) { atomic_fetch_add(&s_brightness_clicks, 1); }
static atomic_uint s_pause_epoch;
static atomic_bool s_reply_active, s_paused;
static atomic_bool s_resume_requested;
static size_t s_resume_at;
static unsigned s_turn_epoch;
static const char *s_record_error;
// 临时诊断只记录阶段编号和错误码，不改变任务准入或音频策略。
static atomic_uint s_voice_diag_phase;
static atomic_int s_voice_diag_error;
#define VOICE_DIAG_STAGE(stage) atomic_store(&s_voice_diag_phase, (stage))
#else
#define VOICE_DIAG_STAGE(stage) ((void)0)
#endif

enum {
    VOICE_DIAG_AUDIO = 1, VOICE_DIAG_PLAYER, VOICE_DIAG_PROMPT,
    VOICE_DIAG_IDLE, VOICE_DIAG_NOTICE, VOICE_DIAG_WAKE_INIT,
    VOICE_DIAG_WAKE_LISTEN, VOICE_DIAG_EVENT, VOICE_DIAG_TURN,
    VOICE_DIAG_HALTED,
};

#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2

void voice_cancel(void) {
    // 已暂停时重复按S4不清除缓存；朗读前的S4仍取消任务。
#if CONFIG_HOMEHUB_QWEN_TTS
    if (atomic_load(&s_paused)) {
        atomic_store(&s_resume_requested, false);
        led_status_set_voice(LED_VOICE_IDLE);
        return;
    }
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
    if (muted) {
        atomic_store(&s_resume_requested, false);
        voice_cancel();
    }
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

#if CONFIG_HOMEHUB_QWEN_ASR
#define CAPTURE_MAX_MS      120000
#else
#define CAPTURE_MAX_MS      15000
#endif
#define CAPTURE_MIN_MS      300
#define CAPTURE_CHUNK       (VOICE_MIC_RATE / 50)      // 20 ms
#define CAPTURE_MAX         (VOICE_MIC_RATE * CAPTURE_MAX_MS / 1000)
#if CONFIG_HOMEHUB_OFFLINE_WAKE
// 候选阶段继续采音；结束门限从最后一段人声起算，不叠加候选时长。
#define END_CANDIDATE_MS    800
#define END_SILENCE_MS      2200
#define AUTO_CAPTURE_MAX_MS 60000
#endif
// Keep listening briefly after release so the last word is not clipped.
#define RELEASE_TAIL_MS     250
#define REPLY_CHUNK         (VOICE_PLAYER_RATE / 50)   // 20 ms

typedef enum { EVT_PRESS, EVT_RELEASE, EVT_REPLAY,
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2 && CONFIG_HOMEHUB_QWEN_TTS && !CONFIG_MUSE_ENABLED
    EVT_STORY, EVT_BRIEFING,
#endif
} voice_evt_t;
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
#if CONFIG_HOMEHUB_EASYINPUT_OLED
int voice_volume(void) { return atomic_load(&s_volume); }
bool voice_playback_paused(void) { return atomic_load(&s_paused); }
#endif

void voice_replay(void) {
#if CONFIG_HOMEHUB_QWEN_TTS
    bool idle = false;
    if (!atomic_load(&s_ready) || easyinput_board_muted() ||
        !atomic_compare_exchange_strong(&s_busy, &idle, true)) {
        if (!easyinput_board_muted() && atomic_load(&s_paused) && atomic_load(&s_busy)) {
            atomic_store(&s_resume_requested, true);
            led_status_set_voice(LED_VOICE_REPLAY_WAIT);
            ESP_LOGI("link.voice", "resume requested; waiting for complete audio cache");
        }
        return;
    }
    voice_event_t evt = {.kind = EVT_REPLAY, .cancel_epoch = atomic_load(&s_cancel_epoch)};
    if (xQueueSend(s_events, &evt, 0) != pdTRUE) atomic_store(&s_busy, false);
#endif
}
#if CONFIG_HOMEHUB_QWEN_TTS && !CONFIG_MUSE_ENABLED
void voice_story(void) {
    if (!atomic_load(&s_ready) || easyinput_board_muted()) return;
    bool idle=false;
    if (!atomic_compare_exchange_strong(&s_busy,&idle,true)) {
        ESP_LOGI(TAG,"story request ignored: voice busy");
        return;
    }
    if (!muse_hatch_ready()) {
        atomic_store(&s_busy,false);
        network_feedback_request();
        return;
    }
    voice_event_t evt={.kind=EVT_STORY,.cancel_epoch=atomic_load(&s_cancel_epoch)};
    if (xQueueSend(s_events,&evt,0)!=pdTRUE) atomic_store(&s_busy,false);
}

void voice_briefing(void) {
    if (!atomic_load(&s_ready) || easyinput_board_muted()) return;
    bool idle = false;
    if (!atomic_compare_exchange_strong(&s_busy, &idle, true)) return;
    if (!config_setup_complete() || !network_feedback_ready() || !muse_hatch_ready()) {
        atomic_store(&s_busy, false);
        network_feedback_request();
        return;
    }
    voice_event_t evt = {.kind = EVT_BRIEFING, .cancel_epoch = atomic_load(&s_cancel_epoch)};
    if (xQueueSend(s_events, &evt, 0) != pdTRUE) atomic_store(&s_busy, false);
}
#endif
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
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
    bool brightness_mode = false;
    int64_t mode_until = 0, brightness_save_at = 0;
    int64_t diagnostic_at = 0;
#endif
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(DIAL_POLL_MS));
        int64_t now = esp_timer_get_time();
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
        bool quiet=easyinput_board_muted() || atomic_load(&s_busy) || atomic_load(&s_paused);
#if CONFIG_HOMEHUB_RADIO
        quiet=quiet || radio_enabled();
#endif
        network_feedback_poll(quiet);
        if (now >= diagnostic_at) {
            diagnostic_at = now + 10000000;
            ESP_LOGI(TAG, "voice diag: phase=%u error=%d ready=%d mute=%d busy=%d paused=%d queued=%u",
                atomic_load(&s_voice_diag_phase), atomic_load(&s_voice_diag_error),
                atomic_load(&s_ready), easyinput_board_muted(), atomic_load(&s_busy),
                atomic_load(&s_paused), (unsigned)uxQueueMessagesWaiting(s_events));
        }
        if (atomic_exchange(&s_brightness_clicks, 0) & 1) {
#if CONFIG_HOMEHUB_RADIO
            if (radio_select_confirm()) brightness_mode = false;
            else
#endif
            {
            brightness_mode = !brightness_mode;
            mode_until = now + 5000000;
            if (brightness_mode) led_status_show_brightness();
            ESP_LOGI(TAG, "dial mode: %s", brightness_mode ? "brightness" : "volume");
            }
        }
        if (brightness_mode && now >= mode_until) {
            brightness_mode = false;
            ESP_LOGI(TAG, "dial mode: volume (idle timeout)");
        }
#endif
        int steps = voice_board_dial_steps();
#if CONFIG_HOMEHUB_RADIO
        if (radio_select_steps(steps)) { steps = 0; brightness_mode = false; }
#endif
        if (steps) {
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
            if (brightness_mode) {
                int before = easyinput_board_brightness();
                easyinput_board_set_brightness(before + steps * 5);
                if (easyinput_board_brightness() != before) brightness_save_at = now + 2000000;
                mode_until = now + 5000000;
                led_status_show_brightness();
            } else
#endif
            {
            int volume = atomic_load(&s_volume) + steps * VOLUME_STEP;
            apply_volume(volume < 0 ? 0 : volume > 100 ? 100 : volume);
            save_at = now + VOLUME_SAVE_MS * 1000LL;
            }
        }
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
        if (brightness_save_at && now >= brightness_save_at) {
            brightness_save_at = 0;
            char value[8];
            snprintf(value, sizeof(value), "%d", easyinput_board_brightness());
            if (config_set_str("led_brightness", value)) ESP_LOGI(TAG, "brightness saved: %s", value);
            else ESP_LOGW(TAG, "brightness could not be stored");
        }
#endif
        if (save_at && now >= save_at) {
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
#if CONFIG_HOMEHUB_OFFLINE_WAKE
            s_auto_record = false; // S1打断后，下一轮恢复按住录音。
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
    const char *end_reason = "sample_limit";
    size_t max_samples = CAPTURE_MAX;
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
    s_record_error = NULL;
    int capture_peak = 0;
    uint64_t energy = 0;
    size_t clipped = 0;
#endif
#if CONFIG_HOMEHUB_OFFLINE_WAKE
    const bool automatic = s_auto_record;
    bool heard_speech = false;
    bool end_candidate = false;
    size_t silence_samples = 0;
    int64_t speech_deadline = esp_timer_get_time() + 5000000;
    if (automatic) max_samples = VOICE_MIC_RATE * AUTO_CAPTURE_MAX_MS / 1000;
    int64_t capture_deadline = esp_timer_get_time() + AUTO_CAPTURE_MAX_MS * 1000LL;
    if (!automatic && voice_board_mic_start() != ESP_OK) return 0;
#else
    if (voice_board_mic_start() != ESP_OK) return 0;
#endif
    size_t samples = 0;
    int64_t stop_at = 0;
    while (samples < max_samples) {
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
        if (cancelled()) { end_reason = "cancelled"; break; }
#if CONFIG_HOMEHUB_QWEN_ASR
        // 流式连接/发送失败立即停止，不等缓冲积压后才发现。
        static char upstream_error[128];
        muse_hatch_ev_t event;
        while ((event = muse_hatch_turn_event(upstream_error, sizeof(upstream_error))) != MUSE_HATCH_EV_NONE) {
            if (event == MUSE_HATCH_EV_ERROR) {
                s_record_error = upstream_error;
                end_reason = "upstream_error";
                break;
            }
        }
        if (s_record_error) break;
#endif
#endif
        voice_event_t evt;
#if CONFIG_HOMEHUB_OFFLINE_WAKE
        if (automatic && esp_timer_get_time() >= capture_deadline) {
            end_reason = "time_limit";
            s_record_error = "RECORDING TIME LIMIT";
            break;
        }
        if (automatic && xQueuePeek(s_events, &evt, 0) == pdTRUE && evt.kind == EVT_PRESS &&
            evt.cancel_epoch == atomic_load(&s_cancel_epoch)) {
            // 留在队列中让语音主循环接管，不提交自动录音的半句。
            unsigned takeover_epoch = atomic_fetch_add(&s_cancel_epoch, 1) + 1;
            xQueueReceive(s_events, &evt, 0);
            evt.cancel_epoch = takeover_epoch;
            xQueueSendToFront(s_events, &evt, 0);
            end_reason = "manual_takeover";
            break;
        }
#endif
        if (!stop_at && xQueueReceive(s_events, &evt, 0) == pdTRUE && evt.kind == EVT_RELEASE) {
            stop_at = esp_timer_get_time() + RELEASE_TAIL_MS * 1000LL;
        }
        if (stop_at && esp_timer_get_time() >= stop_at) { end_reason = "button_release"; break; }
        int peak = 0;
#if CONFIG_HOMEHUB_OFFLINE_WAKE
        bool speech = false;
        size_t got = automatic ? voice_wake_read(chunk, CAPTURE_CHUNK, &speech, &peak)
                                  : voice_board_mic_read(chunk, CAPTURE_CHUNK, &peak);
#else
        size_t got = voice_board_mic_read(chunk, CAPTURE_CHUNK, &peak);
#endif
        if (!got) {
            end_reason = "microphone_error";
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
            s_record_error = "MICROPHONE READ FAILED";
#endif
            break;
        }
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
        if (cancelled()) { end_reason = "cancelled"; break; }
#endif
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
#if CONFIG_HOMEHUB_OFFLINE_WAKE
        if (automatic) {
            if (!heard_speech && !speech) {
                if (esp_timer_get_time() >= speech_deadline) {
                    ESP_LOGI(TAG, "wake had no question; not submitted");
                    end_reason = "no_question";
                    break;
                }
                continue;
            }
            heard_speech = heard_speech || speech;
            silence_samples = speech ? 0 : silence_samples + got;
            if (speech && end_candidate) {
                end_candidate = false;
                led_status_set_voice(LED_VOICE_LISTENING);
                ESP_LOGI(TAG, "recording continued after pause");
            } else if (!end_candidate && silence_samples >= VOICE_MIC_RATE * END_CANDIDATE_MS / 1000) {
                end_candidate = true;
                led_status_set_voice(LED_VOICE_END_CANDIDATE);
                ESP_LOGI(TAG, "recording end candidate; still listening");
            }
        }
#endif
        if (muse_hatch_turn_audio_wait(chunk, got, 100) != got) {
            s_record_error = "MICROPHONE BACKLOG FULL";
            end_reason = "backlog_full";
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
#if CONFIG_HOMEHUB_OFFLINE_WAKE
        if (automatic && silence_samples >= VOICE_MIC_RATE * END_SILENCE_MS / 1000) {
            end_reason = "silence_end";
            break;
        }
#endif
    }
#if CONFIG_HOMEHUB_QWEN_ASR && CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
    if (samples >= max_samples) s_record_error = "RECORDING TIME LIMIT";
#endif
#if CONFIG_HOMEHUB_OFFLINE_WAKE
    if (automatic) {
        if (voice_wake_stop() != ESP_OK) s_record_error = "MICROPHONE STOP FAILED";
    } else voice_board_mic_stop();
#else
    voice_board_mic_stop();
#endif
    ESP_LOGI(TAG, "recording end: reason=%s samples=%u", end_reason, (unsigned)samples);
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
    ESP_LOGI(TAG, "mic samples=%u peak=%d mean_square=%llu clipped=%u", (unsigned)samples,
             capture_peak, (unsigned long long)(samples ? energy / samples : 0), (unsigned)clipped);
    if (s_record_error) return 0; // 不提交已经缺失音频的半句录音。
#endif
    if (samples >= max_samples) ESP_LOGI(TAG, "capture limit reached");
    return samples;
}

static bool fail(const char *why) {
    ESP_LOGW(TAG, "turn failed: %s", why);
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
#if CONFIG_HOMEHUB_QWEN_TTS
    muse_hatch_replay_failed();
    atomic_store(&s_paused, false);
    atomic_store(&s_resume_requested, false);
    s_resume_at = 0;
#endif
    unsigned lights = strstr(why, "TTS") ? 2 : strstr(why, "SPEAKER") ? 3 :
        strstr(why, "ASR") || strstr(why, "MICROPHONE") || strstr(why, "CATCH") ||
        strstr(why, "RECORDING") ? 1 : 5;
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
        led_status_set_voice(LED_VOICE_IDLE);
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

#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2 && CONFIG_HOMEHUB_QWEN_TTS && !CONFIG_MUSE_ENABLED
static bool run_story(void) {
    atomic_store(&s_reply_active,false);
    atomic_store(&s_resume_requested,false);
    if (cancelled()) return false;
    voice_hatch_refresh();
    if (!muse_hatch_ready()) {
        network_feedback_request();
        led_status_set_voice(LED_VOICE_IDLE);
        return false;
    }
    voice_player_stop();
    if (!voice_player_wait(1000)) return fail("SPEAKER STOP TIMEOUT");
    atomic_store(&s_paused,false);
    s_resume_at=0;
    if (cancelled()) return false;
    if (!muse_hatch_prompt_turn(STORY_PROMPT)) return fail("STORY START FAILED");
    led_status_set_story(true);
    led_status_set_voice(LED_VOICE_THINKING);
    ESP_LOGI(TAG,"story request submitted: age=6-9, generated by Muse");
    return reply();
}

static bool run_briefing(void) {
    atomic_store(&s_reply_active, false);
    atomic_store(&s_resume_requested, false);
    if (cancelled()) return false;
    atomic_store(&s_paused, false);
    s_resume_at = 0;
    voice_hatch_refresh();
    if (!config_setup_complete() || !network_feedback_ready() || !muse_hatch_ready()) {
        network_feedback_request();
        led_status_set_voice(LED_VOICE_IDLE);
        return false;
    }
    voice_player_stop();
    if (!voice_player_wait(1000)) return fail("SPEAKER STOP TIMEOUT");
    if (cancelled()) return false;
    if (!muse_briefing_turn(BRIEFING_PROMPT)) return fail("BRIEFING START FAILED");
    led_status_set_story(false);
    led_status_set_voice(LED_VOICE_THINKING);
    ESP_LOGI(TAG, "briefing submitted: live search/date supplied by Muse");
    return reply();
}
#endif

// Returns true if a new press interrupted the turn.
static bool run_turn(void) {
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2 && CONFIG_HOMEHUB_QWEN_TTS && !CONFIG_MUSE_ENABLED
    led_status_set_story(false);
#endif
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
#if CONFIG_HOMEHUB_OFFLINE_WAKE
            if (s_auto_record && !s_record_error) {
                led_status_set_voice(LED_VOICE_IDLE);
                return false;
            }
#endif
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
        bool audio_ready = atomic_load(&s_ready);
        bool muted = audio_ready && voice_board_muted();
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
        ESP_LOGI(TAG, "S1 gate: audio=%d mute=%d phase=%u error=%d", audio_ready, muted,
                 atomic_load(&s_voice_diag_phase), atomic_load(&s_voice_diag_error));
#endif
        if (!audio_ready || muted) return false;
        voice_hatch_refresh();
        bool chat_ready = muse_hatch_ready();
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
        ESP_LOGI(TAG, "S1 gate: chat_ready=%d provisioned=%d network_ready=%d", chat_ready,
                 config_is_provisioned(), network_feedback_ready());
#endif
        if (!chat_ready) return false;
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
        was_busy = atomic_exchange(&s_busy, true);
#endif
    }
    voice_event_t evt = {.kind = pressed ? EVT_PRESS : EVT_RELEASE};
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
    evt.cancel_epoch = atomic_load(&s_cancel_epoch);
#endif
    BaseType_t queued = xQueueSend(s_events, &evt, 0);
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
    ESP_LOGI(TAG, "S1 queue: pressed=%d accepted=%d phase=%u pending=%u", pressed, queued == pdTRUE,
             atomic_load(&s_voice_diag_phase), (unsigned)uxQueueMessagesWaiting(s_events));
#endif
    if (queued != pdTRUE) {
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
        muse_replay_status_t status = muse_hatch_replay_status();
        ESP_LOGI(TAG, "NO REPLAY AUDIO: status=%d", (int)status);
        atomic_store(&s_paused, false);
        s_resume_at = 0;
        if (status == MUSE_REPLAY_FAILED) led_status_set_voice(LED_VOICE_ERROR);
        else led_status_replay_notice(status == MUSE_REPLAY_TOO_LARGE ? 5 :
                                      status == MUSE_REPLAY_NO_MEMORY ? 3 : 1);
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

#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
extern const uint8_t net_wifi_start[] asm("_binary_wifi_pcm_start");
extern const uint8_t net_wifi_end[] asm("_binary_wifi_pcm_end");
extern const uint8_t net_service_start[] asm("_binary_service_pcm_start");
extern const uint8_t net_service_end[] asm("_binary_service_pcm_end");
extern const uint8_t net_pair_start[] asm("_binary_pair_pcm_start");
extern const uint8_t net_pair_end[] asm("_binary_pair_pcm_end");
extern const uint8_t net_ready_start[] asm("_binary_ready_pcm_start");
extern const uint8_t net_ready_end[] asm("_binary_ready_pcm_end");

// 仅在语音任务空闲时播放；与回复使用同一播放器，S1/S2/S4能及时打断。
static bool play_network_notice(network_notice_t notice) {
    if (!notice || easyinput_board_muted() || atomic_load(&s_busy) || atomic_load(&s_paused)
        || uxQueueMessagesWaiting(s_events) || !voice_player_wait(0)) return true;
#if CONFIG_HOMEHUB_RADIO
    if (radio_enabled()) return true;
    if (!radio_pause(true)) { radio_pause(false); return true; }
#endif
#if CONFIG_HOMEHUB_OFFLINE_WAKE
    if (voice_wake_stop()!=ESP_OK) {
        ESP_LOGE(TAG,"network notice skipped: microphone did not stop");
#if CONFIG_HOMEHUB_RADIO
        radio_pause(false);
#endif
        return true;
    }
#endif
    const uint8_t *start=NULL, *end=NULL;
    if (notice==NET_NOTICE_WIFI) { start=net_wifi_start; end=net_wifi_end; }
    if (notice==NET_NOTICE_SERVICE) { start=net_service_start; end=net_service_end; }
    if (notice==NET_NOTICE_PAIR) { start=net_pair_start; end=net_pair_end; }
    if (notice==NET_NOTICE_READY) { start=net_ready_start; end=net_ready_end; }
    size_t samples=start ? (size_t)(end-start)/2 : VOICE_PLAYER_RATE/2;
    unsigned epoch=atomic_load(&s_cancel_epoch);
    ESP_LOGI(TAG,"local network notice=%d",notice);
    voice_player_begin();
    int16_t chunk[REPLY_CHUNK];
    for (size_t offset=0;offset<samples;offset+=REPLY_CHUNK) {
        if (easyinput_board_muted() || atomic_load(&s_busy) || uxQueueMessagesWaiting(s_events)
            || atomic_load(&s_cancel_epoch)!=epoch || !network_feedback_notice_valid(notice)) break;
#if CONFIG_HOMEHUB_RADIO
        if (radio_enabled()) break;
#endif
        size_t n=samples-offset<REPLY_CHUNK ? samples-offset : REPLY_CHUNK;
        for (size_t i=0;i<n;++i) {
            size_t pos=offset+i;
            float envelope=1;
            if (pos<320) envelope=(float)pos/320;
            if (samples-pos<320) envelope=(float)(samples-pos)/320;
            if (start) chunk[i]=(int16_t)(((const int16_t *)start)[pos]*envelope/2);
            else {
                // 失败双音；恢复单个上行双音，总长不足半秒，包络防爆音。
                size_t p=pos%(VOICE_PLAYER_RATE/4);
                float e=p<320 ? (float)p/320 : p>2880 ? (float)(3200-p)/320 : 1;
                float frequency=notice==NET_NOTICE_RECOVERED ? (pos<4000 ? 660 : 880) : (pos<4000 ? 660 : 440);
                chunk[i]=p>=3200 ? 0 : (int16_t)(1200*e*sinf(2*(float)M_PI*frequency*pos/VOICE_PLAYER_RATE));
            }
        }
        if (voice_player_write(chunk,n)!=ESP_OK) break;
    }
    voice_player_end();
    while (!voice_player_wait(20)) {
        bool interrupted=easyinput_board_muted() || atomic_load(&s_busy) || uxQueueMessagesWaiting(s_events)
            || atomic_load(&s_cancel_epoch)!=epoch || !network_feedback_notice_valid(notice);
#if CONFIG_HOMEHUB_RADIO
        interrupted=interrupted || radio_enabled();
#endif
        if (interrupted) { voice_player_stop(); break; }
    }
    bool idle=voice_player_wait(1000);
#if CONFIG_HOMEHUB_RADIO
    if (idle) radio_pause(false);
#endif
    return idle;
}
#endif

static void voice_task(void *arg) {
    VOICE_DIAG_STAGE(VOICE_DIAG_AUDIO);
    esp_err_t init_error = voice_board_init();
    if (init_error == ESP_OK) {
        VOICE_DIAG_STAGE(VOICE_DIAG_PLAYER);
        init_error = voice_player_init();
    }
    if (init_error != ESP_OK) {
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
        atomic_store(&s_voice_diag_error, init_error);
        ESP_LOGE(TAG, "voice init failed: phase=%u error=%s", atomic_load(&s_voice_diag_phase), esp_err_to_name(init_error));
#endif
        VOICE_DIAG_STAGE(VOICE_DIAG_HALTED);
        ESP_LOGE(TAG, "audio hardware unavailable; voice chat disabled");
        vTaskSuspend(NULL);
        return;
    }
    voice_board_set_volume(atomic_load(&s_volume));
    VOICE_DIAG_STAGE(VOICE_DIAG_PROMPT);
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
#if CONFIG_HOMEHUB_OFFLINE_WAKE
    // 配对阶段保留内部RAM，等联网及蓝牙控制器释放后再加载模型。
    bool wake_initialized = false;
#endif
    atomic_store(&s_ready, true);
    if (xTaskCreate(dial_task, "dial", 3072, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no memory for the dial");
    }
    ESP_LOGI(TAG, "ready");
#if CONFIG_HOMEHUB_RADIO
    if (radio_init() != ESP_OK) ESP_LOGE(TAG,"radio initialization failed");
    bool radio_release_pending = false;
#if CONFIG_HOMEHUB_QWEN_TTS && !CONFIG_MUSE_ENABLED
    bool briefing_audio = false, radio_briefing_hold = false;
#endif
#endif

    for (;;) {
        VOICE_DIAG_STAGE(VOICE_DIAG_IDLE);
#if CONFIG_HOMEHUB_RADIO
#if CONFIG_HOMEHUB_QWEN_TTS && !CONFIG_MUSE_ENABLED
        // 暂停简报在语音任务两次执行之间仍持有电台；重置/静音会释放所有权。
        if (radio_briefing_hold && (!atomic_load(&s_paused) || easyinput_board_muted())) {
            if (!voice_player_wait(0)) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
            radio_pause(false);
            radio_briefing_hold = false;
        }
#endif
        // 超时只延后交还；确认旧播放器退出前，不接管录音或恢复电台。
        if (radio_release_pending) {
            if (!voice_player_wait(0)) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            radio_pause(false);
            radio_release_pending = false;
        }
#endif
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
        VOICE_DIAG_STAGE(VOICE_DIAG_NOTICE);
        if (!play_network_notice(network_feedback_take())) {
#if CONFIG_HOMEHUB_RADIO
            radio_release_pending=true;
#endif
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        voice_event_t evt;
#if CONFIG_HOMEHUB_OFFLINE_WAKE
        s_auto_record = false;
        if (xQueueReceive(s_events, &evt, 0) != pdTRUE) {
            if (easyinput_board_muted() || atomic_load(&s_paused) ||
#if CONFIG_HOMEHUB_RADIO
                radio_enabled() ||
#endif
                atomic_load(&s_busy) || !muse_hatch_ready() ||
                esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE) {
                if (voice_wake_stop() != ESP_OK) {
                    fail("MICROPHONE STOP FAILED");
                    vTaskSuspend(NULL);
                }
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            if (!wake_initialized) {
                VOICE_DIAG_STAGE(VOICE_DIAG_WAKE_INIT);
                esp_err_t wake_err = voice_wake_init();
                if (wake_err != ESP_OK) {
                    atomic_store(&s_voice_diag_error, wake_err);
                    VOICE_DIAG_STAGE(VOICE_DIAG_HALTED);
                    ESP_LOGE(TAG, "offline wake initialization failed: %s", esp_err_to_name(wake_err));
                    led_status_set_voice_error(1);
                    vTaskSuspend(NULL);
                    continue;
                }
                wake_initialized = true;
            }
            VOICE_DIAG_STAGE(VOICE_DIAG_WAKE_LISTEN);
            esp_err_t err = voice_wake_start();
            if (err != ESP_OK || voice_wake_error() != ESP_OK) {
                atomic_store(&s_voice_diag_error, err != ESP_OK ? err : voice_wake_error());
                VOICE_DIAG_STAGE(VOICE_DIAG_HALTED);
                fail("MICROPHONE WAKE FAILED");
                vTaskSuspend(NULL);
                continue;
            }
            if (!voice_wake_poll()) continue;
            bool idle = false;
            if (easyinput_board_muted() || !muse_hatch_ready() ||
#if CONFIG_HOMEHUB_RADIO
                radio_enabled() ||
#endif
                !atomic_compare_exchange_strong(&s_busy, &idle, true)) {
                voice_wake_stop();
                continue;
            }
            s_auto_record = true;
            evt = (voice_event_t){.kind = EVT_PRESS, .cancel_epoch = atomic_load(&s_cancel_epoch)};
        } else if (evt.kind != EVT_RELEASE && voice_wake_stop() != ESP_OK) {
            fail("MICROPHONE STOP FAILED");
            vTaskSuspend(NULL);
            continue;
        }
#else
        if (xQueueReceive(s_events, &evt, pdMS_TO_TICKS(20)) != pdTRUE) continue;
#endif
        VOICE_DIAG_STAGE(VOICE_DIAG_EVENT);
        ESP_LOGI(TAG, "voice event consumed: kind=%d epoch_current=%d mute=%d", evt.kind,
                 evt.cancel_epoch == atomic_load(&s_cancel_epoch), easyinput_board_muted());
        if (evt.kind == EVT_RELEASE) continue;
        if (evt.cancel_epoch != atomic_load(&s_cancel_epoch) || easyinput_board_muted()) {
#if CONFIG_HOMEHUB_OFFLINE_WAKE
            if (voice_wake_stop() != ESP_OK) {
                fail("MICROPHONE STOP FAILED");
                vTaskSuspend(NULL);
            }
#endif
            atomic_store(&s_busy, false);
            continue;
        }
        s_turn_epoch = evt.cancel_epoch;
#if CONFIG_HOMEHUB_RADIO
        if (!radio_pause(true)) {
            fail("RADIO STOP TIMEOUT");
            // 尚未启动AI播放器，可直接撤销本次暂停请求。
            radio_pause(false);
#if CONFIG_HOMEHUB_OFFLINE_WAKE
            if (voice_wake_stop() != ESP_OK) {
                fail("MICROPHONE STOP FAILED");
                vTaskSuspend(NULL);
            }
#endif
            atomic_store(&s_busy,false);
            continue;
        }
#endif
        bool talk = evt.kind == EVT_PRESS;
        VOICE_DIAG_STAGE(VOICE_DIAG_TURN);
#if CONFIG_HOMEHUB_QWEN_TTS && !CONFIG_MUSE_ENABLED
#if CONFIG_HOMEHUB_RADIO
        if (evt.kind != EVT_REPLAY) briefing_audio = evt.kind == EVT_BRIEFING;
#endif
        if (evt.kind == EVT_STORY) talk = run_story();
        if (evt.kind == EVT_BRIEFING) talk = run_briefing();
#endif
#if CONFIG_HOMEHUB_QWEN_TTS
        if (evt.kind == EVT_REPLAY) talk = replay();
#endif
        if (talk) {
#if CONFIG_HOMEHUB_RADIO && CONFIG_HOMEHUB_QWEN_TTS && !CONFIG_MUSE_ENABLED
            briefing_audio = false; // S1打断后属于普通问答。
#endif
            while (run_turn()) {}
        }
#if CONFIG_HOMEHUB_OFFLINE_WAKE
        // 包括录音前取消或扬声器停止失败，都必须交还麦克风。
        if (voice_wake_stop() != ESP_OK) {
            fail("MICROPHONE STOP FAILED");
            vTaskSuspend(NULL);
        }
#endif
        atomic_store(&s_reply_active, false);
        atomic_store(&s_busy, false);
#if CONFIG_HOMEHUB_RADIO
#if CONFIG_HOMEHUB_QWEN_TTS && !CONFIG_MUSE_ENABLED
        if (briefing_audio && atomic_load(&s_paused) &&
            muse_hatch_replay_status() != MUSE_REPLAY_READY) {
            // 缓存已失效时无法续播，不得一直持有电台。
            atomic_store(&s_paused, false);
            s_resume_at = 0;
        }
        radio_briefing_hold = briefing_audio && atomic_load(&s_paused) && !easyinput_board_muted();
        if (!radio_briefing_hold) {
#endif
        // 取消路径的播放器仍可能排空DMA；完成后才交还喇叭。
        if (voice_player_wait(1000)) radio_pause(false);
        else {
            radio_release_pending = true;
            fail("SPEAKER STOP TIMEOUT");
        }
#if CONFIG_HOMEHUB_QWEN_TTS && !CONFIG_MUSE_ENABLED
        }
#endif
#endif
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
#if CONFIG_HOMEHUB_RADIO
    muse_hatch_set_local_command_handler(radio_voice_command);
#endif
    s_events = xQueueCreate(8, sizeof(voice_event_t));
    if (!s_events) {
        ESP_LOGE(TAG, "no memory for voice chat");
        return;
    }
    atomic_store(&s_volume, load_volume());
    voice_hatch_refresh();
    muse_hatch_start();
    // 模型初始化会读取Flash并关闭缓存，执行该操作的任务栈必须在内部RAM。
#if CONFIG_HOMEHUB_OFFLINE_WAKE
    const uint32_t stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#else
    // 未启用模型时继续用PSRAM栈，保留配对TLS任务所需的内部RAM。
    const uint32_t stack_caps = MALLOC_CAP_SPIRAM;
#endif
    if (xTaskCreateWithCaps(voice_task, "voice", 4096, NULL, 4, NULL, stack_caps) != pdPASS) {
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
