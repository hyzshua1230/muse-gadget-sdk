#include "radio.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdatomic.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "minimp3.h"
#include "voice_board.h"
#include "led_status.h"

#define INPUT_BYTES 16384
// 本工具链的MP3解码函数单独占用16608字节栈，另留调用链和运行余量。
#define RADIO_STACK_BYTES (32 * 1024)
#define SESSION_US (60LL * 60 * 1000000)
#define FADE_US (5LL * 60 * 1000000)
static const char *TAG = "radio";
static const char *urls[2][2] = {
    {"http://lhttp.qingting.fm/live/267/64k.mp3", "https://lhttp.qingting.fm/live/267/64k.mp3"},
    {"https://ice5.somafm.com/dronezone-128-mp3", "https://ice5.somafm.com/dronezone-128-mp3"}
};
extern const uint8_t classical_start[] asm("_binary_classical_pcm_start");
extern const uint8_t classical_end[] asm("_binary_classical_pcm_end");
extern const uint8_t ambient_start[] asm("_binary_ambient_pcm_start");
extern const uint8_t ambient_end[] asm("_binary_ambient_pcm_end");
typedef struct {
    bool enabled, blocked, output, selecting, ready;
    unsigned epoch, preview;
    int station, candidate;
    int64_t deadline, select_until, preview_at;
} state_t;
static state_t s;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static state_t snapshot(void) {
    portENTER_CRITICAL(&s_lock); state_t copy = s; portEXIT_CRITICAL(&s_lock); return copy;
}
bool radio_enabled(void) { state_t st=snapshot(); return st.enabled || st.selecting; }
static void set_enabled(bool enabled) {
    portENTER_CRITICAL(&s_lock);
    if (s.ready && s.enabled != enabled) {
        s.enabled = enabled; ++s.epoch; s.selecting = false;
        if (enabled) s.deadline = 0;
    }
    portEXIT_CRITICAL(&s_lock);
}
void radio_toggle(void) {
    portENTER_CRITICAL(&s_lock);
    if (s.ready) {
        s.enabled=!s.enabled; ++s.epoch; s.selecting=false;
        if (s.enabled) s.deadline=0;
    }
    portEXIT_CRITICAL(&s_lock);
}
void radio_next(void) {
    portENTER_CRITICAL(&s_lock);
    if (s.ready) { s.station = (s.station + 1) % 2; ++s.epoch; s.selecting = false; }
    portEXIT_CRITICAL(&s_lock);
}
void radio_select_mode(void) {
    portENTER_CRITICAL(&s_lock);
    if (s.ready) {
        s.selecting = true; s.candidate = s.station;
        s.select_until = esp_timer_get_time() + 5000000;
        s.preview_at = esp_timer_get_time() + 800000; ++s.preview;
    }
    portEXIT_CRITICAL(&s_lock);
}
bool radio_select_steps(int steps) {
    bool handled;
    portENTER_CRITICAL(&s_lock);
    handled = s.selecting && esp_timer_get_time() < s.select_until;
    if (handled && steps) {
        s.candidate = (s.candidate + steps % 2 + 2) % 2;
        s.select_until = esp_timer_get_time() + 5000000;
        s.preview_at = esp_timer_get_time() + 800000; ++s.preview;
    }
    if (!handled) s.selecting = false;
    portEXIT_CRITICAL(&s_lock); return handled;
}
bool radio_select_confirm(void) {
    bool handled;
    portENTER_CRITICAL(&s_lock);
    handled = s.selecting && esp_timer_get_time() < s.select_until;
    if (handled) { s.station = s.candidate; ++s.epoch; }
    s.selecting = false;
    portEXIT_CRITICAL(&s_lock); return handled;
}
bool radio_pause(bool pause) {
    portENTER_CRITICAL(&s_lock);
    if (s.blocked != pause) { s.blocked = pause; ++s.epoch; }
    if (pause) s.selecting = false;
    portEXIT_CRITICAL(&s_lock);
    if (!pause) return true;
    int64_t until = esp_timer_get_time() + 4000000;
    while (snapshot().output && esp_timer_get_time() < until) vTaskDelay(pdMS_TO_TICKS(10));
    return !snapshot().output;
}

// 精确匹配有限命令；普通问题仍交给Muse，不用模糊子串猜测电台。
bool radio_voice_command(const char *text) {
    char clean[128]; size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p && n < sizeof(clean)-1;) {
        if (*p < 128 && strchr(" ,.!?\r\n\t", *p)) { ++p; continue; }
        if (!strncmp((const char *)p, "，", 3) || !strncmp((const char *)p, "。", 3) ||
            !strncmp((const char *)p, "！", 3) || !strncmp((const char *)p, "？", 3)) { p += 3; continue; }
        clean[n++] = *p++;
    }
    clean[n] = 0;
    if (!strcmp(clean,"打开收音机") || !strcmp(clean,"打开电台")) set_enabled(true);
    else if (!strcmp(clean,"关闭收音机") || !strcmp(clean,"关闭电台")) set_enabled(false);
    else if (!strcmp(clean,"下一台") || !strcmp(clean,"上一台")) radio_next();
    else {
        int station = !strcmp(clean,"播放上海经典947") || !strcmp(clean,"播放上海经典九四七") ? 0 :
            !strcmp(clean,"切换到环境音乐台") || !strcmp(clean,"播放环境音乐台") ? 1 : -1;
        if (station < 0) return false;
        portENTER_CRITICAL(&s_lock); s.station = station; ++s.epoch; portEXIT_CRITICAL(&s_lock);
        set_enabled(true);
    }
    ESP_LOGI(TAG,"local voice command accepted"); return true;
}

static bool current(unsigned epoch);
typedef struct {
    int interval, left;
    unsigned epoch;
    esp_http_client_handle_t client;
    StreamBufferHandle_t buffer;
    atomic_bool stop, finished;
    size_t received;
} stream_t;
static esp_err_t http_event(esp_http_client_event_t *event) {
    stream_t *stream = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(event->header_key,"icy-metaint")) {
        stream->interval = atoi(event->header_value); stream->left = stream->interval;
    }
    return ESP_OK;
}
static int read_audio(stream_t *stream, uint8_t *buffer, int cap) {
    if (stream->interval > 0 && stream->left == 0) {
        uint8_t length, discard[256];
        if (esp_http_client_read(stream->client,(char *)&length,1) != 1) return -1;
        int bytes = length * 16;
        while (bytes) {
            if (!current(stream->epoch) || atomic_load(&stream->stop)) return -1;
            int got = esp_http_client_read(stream->client,(char *)discard,bytes > 256 ? 256 : bytes);
            if (got <= 0) return -1;
            bytes -= got;
        }
        stream->left = stream->interval;
    }
    if (stream->interval > 0 && cap > stream->left) cap = stream->left;
    int got = esp_http_client_read(stream->client,(char *)buffer,cap);
    if (got > 0 && stream->interval > 0) stream->left -= got;
    return got;
}

typedef struct {
    mp3dec_t decoder;
    uint8_t input[INPUT_BYTES];
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int32_t out[192*2];
    uint8_t download[65536];
    StaticStreamBuffer_t download_control;
    double phase;
    int16_t previous[2];
    bool previous_valid;
    unsigned preview;
    const int16_t *prompt;
    size_t prompt_samples, prompt_pos;
    float duck;
} audio_t;
static void preview_update(audio_t *audio) {
    state_t st = snapshot(); int64_t now = esp_timer_get_time();
    if (!st.selecting || now >= st.select_until) {
        audio->prompt = NULL;
        if (st.selecting) {
            portENTER_CRITICAL(&s_lock);
            if (s.select_until == st.select_until) s.selecting = false;
            portEXIT_CRITICAL(&s_lock);
        }
    } else if (st.preview != audio->preview && now < st.preview_at) {
        audio->prompt = NULL;
    } else if (st.preview != audio->preview && now >= st.preview_at) {
        audio->preview = st.preview; audio->prompt_pos = 0;
        const uint8_t *start = st.candidate ? ambient_start : classical_start;
        const uint8_t *end = st.candidate ? ambient_end : classical_end;
        audio->prompt = (const int16_t *)start; audio->prompt_samples = (end-start)/2;
    }
}
static float gain_now(int64_t started) {
    state_t st = snapshot(); int64_t now = esp_timer_get_time();
    float gain = (now-started)/1000000.0f;
    if (gain > 1) gain = 1;
    if (st.deadline && st.deadline-now < FADE_US) gain *= (float)(st.deadline-now)/FADE_US;
    if (gain < 0) gain = 0;
    return gain;
}
static esp_err_t output(audio_t *audio, size_t count, float gain, bool prompts) {
    for (size_t i=0;i<count;++i) {
        int32_t prompt = 0;
        bool speaking = prompts && audio->prompt && audio->prompt_pos/3 < audio->prompt_samples;
        if (speaking)
            prompt = (int32_t)audio->prompt[audio->prompt_pos++/3] * 65536;
        float target=speaking ? 0.2f : 1.0f;
        float difference=target-audio->duck;
        audio->duck += difference>1.0f/960 ? 1.0f/960 : difference < -1.0f/960 ? -1.0f/960 : difference;
        for (int ch=0;ch<2;++ch) {
            int64_t value = (int64_t)(audio->out[2*i+ch] * gain * audio->duck) + prompt;
            audio->out[2*i+ch] = value > INT32_MAX ? INT32_MAX : value < INT32_MIN ? INT32_MIN : value;
        }
    }
    return voice_board_speaker_write(audio->out,count);
}
static bool current(unsigned epoch) {
    state_t st = snapshot();
    return st.enabled && !st.blocked && st.epoch == epoch &&
        (!st.deadline || esp_timer_get_time() < st.deadline);
}

static void downloader(void *arg) {
    stream_t *stream=arg;
    uint8_t block[2048];
    for (int redirects=0;redirects<4;++redirects) {
        if (atomic_load(&stream->stop) || !current(stream->epoch)) goto done;
        esp_err_t err = esp_http_client_open(stream->client,0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG,"stream open failed: %s",esp_err_to_name(err)); goto done;
        }
        if (esp_http_client_fetch_headers(stream->client)<0) {
            ESP_LOGW(TAG,"stream headers failed"); goto done;
        }
        int status=esp_http_client_get_status_code(stream->client);
        if (status < 300 || status >= 400) break;
        if (esp_http_client_set_redirection(stream->client) != ESP_OK) goto done;
        esp_http_client_close(stream->client);
        stream->interval=stream->left=0;
    }
    if (esp_http_client_get_status_code(stream->client) != 200) {
        ESP_LOGW(TAG,"stream HTTP status=%d",esp_http_client_get_status_code(stream->client)); goto done;
    }
    while (current(stream->epoch) && !atomic_load(&stream->stop)) {
        int got=read_audio(stream,block,sizeof(block));
        if (got<=0) {
            if (current(stream->epoch) && !atomic_load(&stream->stop))
                ESP_LOGW(TAG,"stream read ended: result=%d errno=%d bytes=%u",got,
                         esp_http_client_get_errno(stream->client),(unsigned)stream->received);
            break;
        }
        stream->received += got;
        size_t offset=0;
        while (offset<(size_t)got && current(stream->epoch) && !atomic_load(&stream->stop))
            offset+=xStreamBufferSend(stream->buffer,block+offset,got-offset,pdMS_TO_TICKS(20));
    }
done:
    esp_http_client_close(stream->client);
    atomic_store(&stream->finished,true);
    vTaskDeleteWithCaps(NULL);
}

static bool play(audio_t *audio, state_t st, int url_index) {
    stream_t stream = {.epoch=st.epoch};
    esp_http_client_config_t cfg = {
        .url = urls[st.station][url_index], .timeout_ms = 1000,
        .crt_bundle_attach = esp_crt_bundle_attach, .buffer_size = 2048,
        .event_handler = http_event, .user_data = &stream,
    };
    stream.client = esp_http_client_init(&cfg);
    if (!stream.client) return false;
    stream.buffer=xStreamBufferCreateStatic(sizeof(audio->download),1,audio->download,&audio->download_control);
    if (!stream.buffer) { esp_http_client_cleanup(stream.client); return false; }
    esp_http_client_set_header(stream.client,"Icy-MetaData","0");
    bool ok = false, sounding = false;
    unsigned underflows = 0;
    size_t low_water = sizeof(audio->download);
    if (xTaskCreateWithCaps(downloader,"radio_http",6144,&stream,3,NULL,MALLOC_CAP_SPIRAM) != pdPASS) {
        atomic_store(&stream.finished,true); goto close;
    }
    while (current(st.epoch) && !atomic_load(&stream.finished) &&
        xStreamBufferBytesAvailable(stream.buffer)<4096) vTaskDelay(pdMS_TO_TICKS(20));
    mp3dec_init(&audio->decoder); audio->phase = 0; audio->previous_valid = false;
    size_t used = 0; int64_t started = 0, progress = esp_timer_get_time(), stop_at = 0, stop_duration = 200000;
    for (;;) {
        if (!current(st.epoch) && !stop_at) {
            if (!sounding) break;
            stop_duration=snapshot().blocked ? 20000 : 200000;
            stop_at=esp_timer_get_time()+stop_duration;
            atomic_store(&stream.stop,true);
        }
        if (stop_at && esp_timer_get_time()>=stop_at) break;
        if (used < 4096) {
            // 已有压缩音频时只取当前可用数据，不阻塞完整帧的解码输出。
            int got = xStreamBufferReceive(stream.buffer,audio->input+used,INPUT_BYTES-used,0);
            if (!got && atomic_load(&stream.finished) && !used) break;
            used += got;
        }
        mp3dec_frame_info_t info;
        int frames = mp3dec_decode_frame(&audio->decoder,audio->input,used,audio->pcm,&info);
        if (frames && !sounding)
            ESP_LOGI(TAG,"decoder stack minimum=%u bytes",(unsigned)uxTaskGetStackHighWaterMark(NULL));
        if (info.frame_bytes) {
            used -= info.frame_bytes; memmove(audio->input,audio->input+info.frame_bytes,used);
        } else {
            if (used == INPUT_BYTES) break;
            int got = xStreamBufferReceive(stream.buffer,audio->input+used,INPUT_BYTES-used,pdMS_TO_TICKS(20));
            if (!got && sounding && !stop_at && !atomic_load(&stream.finished)) ++underflows;
            if (!got && atomic_load(&stream.finished)) break;
            used += got;
        }
        if (!frames) { if (esp_timer_get_time()-progress > 10000000) break; continue; }
        if (info.hz < 8000 || info.hz > 48000 || info.channels < 1 || info.channels > 2) break;
        progress = esp_timer_get_time();
        size_t buffered = used + xStreamBufferBytesAvailable(stream.buffer);
        if (buffered < low_water) low_water = buffered;
        if (!sounding) {
            portENTER_CRITICAL(&s_lock);
            bool allowed=s.enabled && !s.blocked && s.epoch==st.epoch;
            if (allowed) s.output=true;
            portEXIT_CRITICAL(&s_lock);
            if (!allowed) goto finished;
            sounding = true; started = progress; voice_board_amp(true);
            led_status_radio_dark(true);
            ESP_LOGI(TAG,"station=%d decoded %dHz %dkbps channels=%d",st.station,info.hz,info.bitrate_kbps,info.channels);
        }
        preview_update(audio);
        size_t out = 0;
        // 跨帧保留前一样本及分数相位，避免44.1k到48k块边界不连续。
        if (!audio->previous_valid) {
            audio->previous[0]=audio->pcm[0]; audio->previous[1]=audio->pcm[info.channels-1];
            audio->previous_valid=true;
        }
        while (audio->phase < frames) {
            int index = (int)audio->phase; double fraction = audio->phase-index;
            for (int ch=0;ch<2;++ch) {
                int slot = info.channels == 1 ? 0 : ch;
                int a = index ? audio->pcm[(index-1)*info.channels+slot] : audio->previous[ch];
                int b = audio->pcm[index*info.channels+slot];
                audio->out[2*out+ch] = (int32_t)((a+(b-a)*fraction)*65536);
            }
            audio->phase += (double)info.hz/48000; ++out;
            if (out == 192) {
                if (!current(st.epoch) && !stop_at) {
                    stop_duration=snapshot().blocked ? 20000 : 200000;
                    stop_at=esp_timer_get_time()+stop_duration; atomic_store(&stream.stop,true);
                }
                float gain=gain_now(started);
                if (stop_at) {
                    int64_t remaining=stop_at-esp_timer_get_time();
                    if (remaining<=0) goto finished;
                    gain *= (float)remaining/stop_duration;
                }
                esp_err_t err = output(audio,out,gain,!stop_at);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG,"stream speaker failed: %s",esp_err_to_name(err)); goto finished;
                }
                portENTER_CRITICAL(&s_lock);
                if (!s.deadline && s.enabled) s.deadline = esp_timer_get_time()+SESSION_US;
                portEXIT_CRITICAL(&s_lock);
                out=0;
            }
        }
        audio->phase -= frames;
        audio->previous[0] = audio->pcm[(frames-1)*info.channels];
        audio->previous[1] = audio->pcm[frames*info.channels-1];
        float gain=gain_now(started);
        if (stop_at) { float f=(float)(stop_at-esp_timer_get_time())/stop_duration; gain*=f>0?f:0; }
        if (out) {
            esp_err_t err = output(audio,out,gain,!stop_at);
            if (err != ESP_OK) {
                ESP_LOGW(TAG,"stream speaker failed: %s",esp_err_to_name(err)); break;
            }
        }
    }
finished:
    ok = !current(st.epoch);
    if (sounding) {
        // 主动关闭沿已缓冲音乐淡出，不生成固定直流尾音；最后排空DMA。
        memset(audio->out,0,sizeof(audio->out)); voice_board_speaker_write(audio->out,192);
        vTaskDelay(pdMS_TO_TICKS(40)); voice_board_amp(false);
        led_status_radio_dark(false);
        portENTER_CRITICAL(&s_lock); s.output=false; portEXIT_CRITICAL(&s_lock);
    }
close:
    atomic_store(&stream.stop,true);
    while (!atomic_load(&stream.finished)) vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(TAG,"stream summary: station=%d received=%u buffer_min=%u wait_empty=%u cancelled=%d",
             st.station,(unsigned)stream.received,(unsigned)low_water,underflows,(int)!current(st.epoch));
    esp_http_client_cleanup(stream.client); vStreamBufferDelete(stream.buffer);
    led_status_radio_dark(false); return ok;
}
static void worker(void *arg) {
    audio_t *audio = arg;
    unsigned epoch = 0; int failures = 0;
    for (;;) {
        state_t st = snapshot();
        preview_update(audio);
        if (st.deadline && st.enabled && esp_timer_get_time() >= st.deadline) {
            portENTER_CRITICAL(&s_lock);
            bool expired=s.epoch==st.epoch && s.deadline==st.deadline && s.enabled;
            if (expired) { s.enabled=false; s.selecting=false; ++s.epoch; }
            portEXIT_CRITICAL(&s_lock);
            if (expired) ESP_LOGI(TAG,"60 minute timer expired");
            continue;
        }
        if (!st.enabled || st.blocked) {
            if (!st.blocked && st.selecting && audio->prompt && audio->prompt_pos/3<audio->prompt_samples) {
                portENTER_CRITICAL(&s_lock);
                bool allowed=!s.blocked;
                if (allowed) s.output=true;
                portEXIT_CRITICAL(&s_lock);
                if (allowed) {
                    voice_board_amp(true);
                    while (!snapshot().blocked && audio->prompt_pos/3<audio->prompt_samples && snapshot().selecting) {
                        preview_update(audio);
                        if (!audio->prompt) break;
                        memset(audio->out,0,sizeof(audio->out));
                        if (output(audio,192,1,true) != ESP_OK) break;
                    }
                    memset(audio->out,0,sizeof(audio->out)); voice_board_speaker_write(audio->out,192);
                    vTaskDelay(pdMS_TO_TICKS(40)); voice_board_amp(false);
                    portENTER_CRITICAL(&s_lock); s.output=false; portEXIT_CRITICAL(&s_lock);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(20)); continue;
        }
        if (st.epoch != epoch) { epoch=st.epoch; failures=0; }
        bool ok = play(audio,st,failures ? 1 : 0);
        portENTER_CRITICAL(&s_lock); s.output=false; portEXIT_CRITICAL(&s_lock);
        if (ok || !current(st.epoch)) continue;
        if (++failures > 4) {
            portENTER_CRITICAL(&s_lock);
            if (s.epoch == st.epoch) { s.enabled=false; s.selecting=false; ++s.epoch; }
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGW(TAG,"stream failed after four retries"); led_status_replay_notice(2);
        } else {
            ESP_LOGW(TAG,"reconnect %d/4",failures);
            int64_t until=esp_timer_get_time()+failures*1000000LL;
            while (current(st.epoch) && esp_timer_get_time()<until) vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}
esp_err_t radio_init(void) {
    audio_t *audio = heap_caps_calloc(1,sizeof(*audio),MALLOC_CAP_SPIRAM);
    if (!audio) return ESP_ERR_NO_MEM;
    audio->duck=1;
    if (xTaskCreateWithCaps(worker,"radio",RADIO_STACK_BYTES,audio,3,NULL,MALLOC_CAP_SPIRAM) != pdPASS) {
        free(audio); return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_lock); s.ready=true; portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}
