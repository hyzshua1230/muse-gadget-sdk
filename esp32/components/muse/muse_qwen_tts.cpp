#include "muse_qwen_tts.h"
#include "muse_chat.h"
#include "muse_qwen_error.h"
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tls_errors.h"
#include <cerrno>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static std::atomic<int> s_status{0}; // 0空闲，1请求中，2完成，-1失败
static std::atomic<uint32_t> s_generation{0};
static std::atomic<const char *> s_error{"service"};
const char *muse_qwen_tts_error(void) { return s_error.load(); }
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
            if (!next) { s_error.store("memory"); return false; }
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

static esp_err_t transport_event(esp_http_client_event_t *event) {
    // TLS连接保留10秒预算；建立连接后，每次读写最多等待250ms。
    if (event->event_id == HTTP_EVENT_ON_CONNECTED)
        esp_http_client_set_timeout_ms(event->client, 250);
    return ESP_OK;
}

static uint8_t *fetch(const char *url, const char *body, size_t limit, uint32_t gen, size_t *length, const char *stage) {
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.is_async = true;
    cfg.timeout_ms = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = true;
    cfg.max_authorization_retries = -1;
    cfg.event_handler = transport_event;
    auto client = esp_http_client_init(&cfg);
    if (!client) return nullptr;
    ESP_LOGI("qwen_tts", "%s begin", stage);
    bool ok = true, timed_out = false, buffer_error = false;
    esp_err_t result = ESP_OK;
    int64_t started = esp_timer_get_time(), progress = started;
    auto active = [&]() {
        if (!current(gen)) return false;
        int64_t now = esp_timer_get_time();
        timed_out = body ? now - started >= 90000000 : now - progress >= 10000000;
        return !timed_out;
    };
    if (body) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        char auth[256];
        int n = snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_HOMEHUB_QWEN_API_KEY);
        ok = n > 0 && n < int(sizeof(auth));
        if (ok) ok = esp_http_client_set_header(client, "Authorization", auth) == ESP_OK;
        memset(auth, 0, sizeof(auth));
    }
    size_t body_size = body ? strlen(body) : 0;
    // 只继续尚未完成的连接/传输，不重发请求，不增加付费重试。
    if (ok) {
        do {
            if (!active()) { ok = false; break; }
            result = esp_http_client_open(client, body_size);
            if (result == ESP_ERR_HTTP_EAGAIN) vTaskDelay(pdMS_TO_TICKS(20));
        } while (result == ESP_ERR_HTTP_EAGAIN);
        ok = ok && result == ESP_OK;
    }
    size_t sent = 0;
    while (ok && sent < body_size && active()) {
        errno = 0;
        int n = esp_http_client_write(client, body + sent, body_size - sent);
        if (n > 0) sent += n;
        else if (n == 0 || errno == EAGAIN || errno == EWOULDBLOCK) vTaskDelay(pdMS_TO_TICKS(20));
        else ok = false;
    }
    int64_t content_bytes = -1;
    while (ok && active()) {
        content_bytes = esp_http_client_fetch_headers(client);
        if (content_bytes >= 0) break;
        if (content_bytes == -ESP_ERR_HTTP_EAGAIN || errno == EAGAIN || errno == EWOULDBLOCK)
            vTaskDelay(pdMS_TO_TICKS(20));
        else { ok = false; break; }
    }
    int status = esp_http_client_get_status_code(client);
    // 错误正文只在内存中提取固定错误类别，最多接收4KiB，不打印正文。
    if (status != 200 && limit > 4096) limit = 4096;
    if (content_bytes > int64_t(limit)) { ok = false; buffer_error = true; }
    uint8_t *data = nullptr;
    size_t used = 0, capacity = 0;
    char chunk[1024];
    while (ok && active()) {
        int n = esp_http_client_read(client, chunk, sizeof(chunk));
        if (n == -ESP_ERR_HTTP_EAGAIN) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (n < 0) { ok = false; break; }
        if (!n) { ok = esp_http_client_is_complete_data_received(client); break; }
        if (size_t(n) > limit - used) { ok = false; buffer_error = true; break; }
        if (used + size_t(n) > capacity) {
            size_t next_capacity = used + size_t(n) + 16384;
            if (next_capacity > limit) next_capacity = limit;
#if CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2
            muse_hatch_replay_make_room(next_capacity + 1);
#endif
            auto *next = static_cast<uint8_t *>(heap_caps_realloc(data, next_capacity + 1, MALLOC_CAP_SPIRAM));
            if (!next) { ok = false; buffer_error = true; break; }
            data = next;
            capacity = next_capacity;
        }
        memcpy(data + used, chunk, n);
        used += n;
        progress = esp_timer_get_time();
    }
    esp_err_t tls = esp_http_client_get_and_clear_last_tls_error(client, nullptr, nullptr);
    int network_error = esp_http_client_get_errno(client);
    ok = ok && current(gen) && !timed_out && status == 200 && data && used;
    if (data) data[used] = 0;
    if (!ok) {
        const char *category = "transport";
        if (!current(gen)) category = "cancelled";
        else if (tls == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) category = "dns";
        else if (timed_out || result == ESP_ERR_HTTP_READ_TIMEOUT || network_error == ETIMEDOUT ||
                 tls == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT) category = "timeout";
        else if (status >= 400) {
            auto *error = data ? cJSON_Parse(reinterpret_cast<char *>(data)) : nullptr;
            const char *code = cJSON_GetStringValue(cJSON_GetObjectItem(error, "code"));
            category = muse_qwen_service_error(status, code);
            cJSON_Delete(error);
        } else if (buffer_error) category = "buffer_limit";
        s_error.store(category);
        ESP_LOGW("qwen_tts", "%s failed: category=%s http=%d bytes=%u", stage, category, status, unsigned(used));
    } else ESP_LOGI("qwen_tts", "%s complete (%u bytes)", stage, unsigned(used));
    // 句柄与接收缓冲区由本任务独占；取消只改变generation，不跨任务close。
    esp_http_client_cleanup(client);
    if (!ok) { free(data); return nullptr; }
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
    if (wav) s_error.store("format");
    bool ok = wav && append_wav(wav, bytes);
    if (wav && !ok) {
        ESP_LOGW("qwen_tts", "unsupported or invalid WAV");
    }
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
    if (!text || !*text || !generation) return false;
    if (s_status.load() != 1 && s_generation.load() == 0) muse_qwen_tts_cancel();
    if (s_status.load() != 0) return false;
    s_error.store("service");
    auto req = static_cast<request_t *>(calloc(1, sizeof(request_t)));
    if (!req) { s_error.store("memory"); s_status.store(-1); return true; }
    req->text = strdup(text);
    req->generation = generation;
    s_generation.store(generation);
    s_status.store(1);
    if (!req->text || xTaskCreate(worker, "qwen_tts", 8192, req, 3, nullptr) != pdPASS) {
        s_error.store("memory");
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
