#include "muse_qwen_tts.h"
#include "muse_chat.h"
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static std::atomic<int> s_status{0}; // 0空闲，1请求中，2完成，-1失败
static std::atomic<uint32_t> s_generation{0};
static int16_t *s_pcm;
static size_t s_frames;
struct request_t { char *text; uint32_t generation; };
static constexpr size_t AUDIO_LIMIT = 4 * 1024 * 1024;

static bool current(uint32_t gen) { return s_generation.load() == gen; }
static uint32_t le32(const uint8_t *p) {
    return p[0] | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// 只接受PCM16/24k单声道，data长度以HTTP实际字节为准，兼容Qwen的占位长度。
static bool append_wav(uint8_t *wav, size_t bytes) {
    if (bytes < 12 || memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVE", 4)) return false;
    bool format = false;
    for (size_t pos = 12; pos + 8 <= bytes;) {
        size_t size = le32(wav + pos + 4), begin = pos + 8;
        if (!memcmp(wav + pos, "data", 4)) {
            size_t actual = size < bytes - begin ? size : bytes - begin;
            if (!format || !actual || actual % 2 || actual > AUDIO_LIMIT - s_frames * 2) return false;
            if (!s_frames) {
                // 首段直接接管下载缓冲区，移除WAV头，不再同时保存WAV和PCM两份音频。
                memmove(wav, wav + begin, actual);
                s_pcm = reinterpret_cast<int16_t *>(wav);
                auto *compact = static_cast<int16_t *>(heap_caps_realloc(s_pcm, actual, MALLOC_CAP_SPIRAM));
                if (compact) s_pcm = compact;
                s_frames = actual / 2;
                return actual != 0;
            }
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
            muse_hatch_replay_make_room(s_frames * 2 + actual);
#endif
            auto *next = static_cast<int16_t *>(heap_caps_realloc(s_pcm, s_frames * 2 + actual, MALLOC_CAP_SPIRAM));
            if (!next) return false;
            s_pcm = next;
            memcpy(s_pcm + s_frames, wav + begin, actual);
            s_frames += actual / 2;
            return actual != 0;
        }
        if (size > bytes - begin) return false;
        if (!memcmp(wav + pos, "fmt ", 4)) {
            format = size >= 16 && wav[begin] == 1 && wav[begin+1] == 0 &&
                wav[begin+2] == 1 && wav[begin+3] == 0 && le32(wav + begin+4) == 24000 &&
                wav[begin+14] == 16 && wav[begin+15] == 0;
        }
        pos = begin + size + (size & 1);
    }
    return false;
}

static uint8_t *fetch(const char *url, const char *body, size_t limit, uint32_t gen, size_t *length, const char *stage) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    // 合成允许服务端排队至90秒；音频下载保持10秒上限。
    cfg.timeout_ms = body ? 90000 : 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = true;
    auto client = esp_http_client_init(&cfg);
    if (!client) return nullptr;
    ESP_LOGI("qwen_tts", "%s begin", stage);
    bool ok = true;
    if (body) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        char auth[256];
        int n = snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_HOMEHUB_QWEN_API_KEY);
        ok = n > 0 && n < int(sizeof(auth));
        if (ok) esp_http_client_set_header(client, "Authorization", auth);
    }
    size_t body_size = body ? strlen(body) : 0;
    ok = ok && current(gen) && esp_http_client_open(client, body_size) == ESP_OK;
    if (ok && body) ok = esp_http_client_write(client, body, body_size) == int(body_size);
    int64_t content_bytes = -1;
    if (ok) {
        content_bytes = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI("qwen_tts", "%s HTTP status %d", stage, status);
        ok = status == 200;
        if (content_bytes > int64_t(limit)) ok = false;
    }
    uint8_t *data = nullptr;
    size_t used = 0, cap = limit < 16384 ? limit : 16384;
    if (content_bytes > 0 && content_bytes <= int64_t(limit)) cap = size_t(content_bytes);
    if (ok) {
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
        muse_hatch_replay_make_room(cap + 1);
#endif
        data = static_cast<uint8_t *>(heap_caps_malloc(cap + 1, MALLOC_CAP_SPIRAM));
    }
    ok = ok && data;
    while (ok && current(gen)) {
        if (used == cap) {
            if (cap == limit || content_bytes > 0) { ok = esp_http_client_is_complete_data_received(client); break; }
            // 分块传输没有Content-Length时按小步增长，避免向上翻倍至4MiB。
            size_t next_cap = cap + 256 * 1024 < limit ? cap + 256 * 1024 : limit;
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
            muse_hatch_replay_make_room(next_cap + 1);
#endif
            auto next = static_cast<uint8_t *>(heap_caps_realloc(data, next_cap + 1, MALLOC_CAP_SPIRAM));
            if (!next) { ok = false; break; }
            data = next; cap = next_cap;
        }
        int n = esp_http_client_read(client, reinterpret_cast<char *>(data + used), cap - used);
        if (n < 0) { ok = false; break; }
        if (!n) { ok = esp_http_client_is_complete_data_received(client); break; }
        used += n;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!ok || !current(gen)) {
        ESP_LOGW("qwen_tts", "%s %s (%u bytes)", stage, current(gen) ? "failed" : "cancelled", (unsigned)used);
        free(data); return nullptr;
    }
    ESP_LOGI("qwen_tts", "%s complete (%u bytes)", stage, (unsigned)used);
    data[used] = 0;
    *length = used;
    return data;
}

static bool synthesize(const char *text, uint32_t gen) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return false;
    cJSON_AddStringToObject(root, "model", "qwen3-tts-flash");
    auto input = cJSON_AddObjectToObject(root, "input");
    if (!input) { cJSON_Delete(root); return false; }
    cJSON_AddStringToObject(input, "text", text);
    cJSON_AddStringToObject(input, "voice", "Cherry");
    cJSON_AddStringToObject(input, "language_type", "Chinese");
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return false;
    size_t bytes = 0;
    auto json = fetch("https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation", body, 16384, gen, &bytes, "synthesis");
    cJSON_free(body);
    if (!json) return false;
    auto response = cJSON_Parse(reinterpret_cast<char *>(json));
    free(json);
    auto output = cJSON_GetObjectItem(response, "output");
    auto audio = cJSON_GetObjectItem(output, "audio");
    const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(audio, "url"));
    char *secure = nullptr;
    if (url && (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8))) {
        const char *host = strstr(url, "://") + 3;
        const char *slash = strchr(host, '/');
        const char *suffix = ".aliyuncs.com";
        size_t host_len = slash ? size_t(slash-host) : 0;
        if (host_len > strlen(suffix) && !memcmp(host + host_len - strlen(suffix), suffix, strlen(suffix))) {
            secure = static_cast<char *>(malloc(strlen(url) + 2));
            if (secure) snprintf(secure, strlen(url) + 2, "https://%s", host);
        }
    }
    cJSON_Delete(response);
    if (!secure) { ESP_LOGW("qwen_tts", "audio URL unavailable"); return false; }
    auto wav = fetch(secure, nullptr, AUDIO_LIMIT, gen, &bytes, "audio download");
    free(secure);
    bool owned = s_frames == 0;
    bool ok = wav && append_wav(wav, bytes);
    if (wav && !ok) ESP_LOGW("qwen_tts", "unsupported or invalid WAV");
    if (!ok || !owned) free(wav);
    return ok;
}

static void worker(void *arg) {
    auto req = static_cast<request_t *>(arg);
    bool ok = true;
    // UTF-8按字符切分，遵守每次请求600字符的接口限制。
    char *begin = req->text;
    while (*begin && ok && current(req->generation)) {
        char *end = begin;
        unsigned chars = 0;
        while (*end) {
            if ((uint8_t(*end) & 0xc0) != 0x80 && chars++ == 600) break;
            ++end;
        }
        char saved = *end;
        *end = 0;
        ok = synthesize(begin, req->generation);
        *end = saved;
        begin = end;
    }
    if (!ok || !current(req->generation)) {
        free(s_pcm); s_pcm = nullptr; s_frames = 0;
    }
    bool cancelled = !current(req->generation);
    free(req->text); free(req);
    s_status.store(cancelled ? 0 : ok ? 2 : -1);
    vTaskDelete(nullptr);
}

bool muse_qwen_tts_start(const char *text, uint32_t generation) {
    if (s_status.load() != 1 && s_generation.load() == 0) muse_qwen_tts_cancel();
    if (s_status.load() != 0) return false;
    auto req = static_cast<request_t *>(calloc(1, sizeof(request_t)));
    if (!req) { s_status.store(-1); return true; }
    req->text = strdup(text);
    req->generation = generation;
    s_generation.store(generation);
    s_status.store(1);
    if (!req->text || xTaskCreate(worker, "qwen_tts", 8192, req, 3, nullptr) != pdPASS) {
        free(req->text); free(req); s_status.store(-1);
    }
    return true;
}

int muse_qwen_tts_result(uint32_t generation, int16_t **pcm, size_t *frames) {
    int status = s_status.load();
    if (status == 1 || status == 0) return 0;
    if (!current(generation)) return -1;
    *pcm = s_pcm; *frames = s_frames;
    s_pcm = nullptr; s_frames = 0;
    s_status.store(0);
    return status == 2 ? 1 : -1;
}

void muse_qwen_tts_cancel(void) {
    s_generation.store(0);
    if (s_status.load() != 1) {
        free(s_pcm); s_pcm = nullptr; s_frames = 0;
        s_status.store(0);
    }
}
