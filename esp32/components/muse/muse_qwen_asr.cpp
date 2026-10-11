#include "muse_qwen_asr.h"
#include "muse_chat_priv.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static std::atomic<int> s_status{0}; // 0空闲，1请求中，2成功，-1失败
static std::atomic<uint32_t> s_generation{0};
static char *s_text;
struct request_t { char *body; uint32_t generation; };
static bool current(uint32_t gen) { return s_generation.load() == gen; }
static void le32(uint8_t *p, uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(n >> (i * 8));
}

static void worker(void *arg) {
    auto req = static_cast<request_t *>(arg);
    esp_http_client_config_t cfg = {};
    cfg.url = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions";
    cfg.timeout_ms = 15000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_redirect = true;
    auto client = esp_http_client_init(&cfg);
    bool ok = client && current(req->generation);
    constexpr size_t response_capacity = 16384;
    char *response = static_cast<char *>(heap_caps_malloc(response_capacity, MALLOC_CAP_SPIRAM));
    ok = ok && response;
    size_t used = 0;
    if (ok) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        char auth[256];
        int n = snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_HOMEHUB_QWEN_API_KEY);
        ok = n > 0 && n < int(sizeof(auth));
        if (ok) esp_http_client_set_header(client, "Authorization", auth);
        size_t len = strlen(req->body);
        ok = ok && esp_http_client_open(client, len) == ESP_OK;
        size_t off = 0;
        while (ok && off < len && current(req->generation)) {
            size_t part = len - off < 4096 ? len - off : 4096;
            n = esp_http_client_write(client, req->body + off, part);
            ok = n > 0;
            if (ok) off += n;
        }
        ok = ok && off == len && current(req->generation);
        if (ok) {
            esp_http_client_fetch_headers(client);
            int status = esp_http_client_get_status_code(client);
            ESP_LOGI("qwen_asr", "HTTP status %d", status);
            ok = status == 200;
        }
        while (ok && current(req->generation) && used < response_capacity - 1) {
            n = esp_http_client_read(client, response + used, response_capacity - 1 - used);
            if (n < 0) { ok = false; break; }
            if (!n) break;
            used += n;
        }
        ok = ok && esp_http_client_is_complete_data_received(client);
    }
    if (client) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    char *text = nullptr;
    if (ok && current(req->generation)) {
        response[used] = 0;
        auto root = cJSON_Parse(response);
        auto choice = cJSON_GetArrayItem(cJSON_GetObjectItem(root, "choices"), 0);
        auto message = cJSON_GetObjectItem(choice, "message");
        const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(message, "content"));
        const char *reason = cJSON_GetStringValue(cJSON_GetObjectItem(choice, "finish_reason"));
        if (content && *content && reason && !strcmp(reason, "stop")) text = strdup(content);
        cJSON_Delete(root);
    }
    bool cancelled = !current(req->generation);
    free(response);
    free(req->body); free(req);
    if (cancelled) free(text);
    else s_text = text;
    s_status.store(cancelled ? 0 : text ? 2 : -1);
    vTaskDelete(nullptr);
}

bool muse_qwen_asr_start(const uint8_t *pcm, size_t bytes, uint32_t generation) {
    if (s_status.load() != 1 && s_generation.load() == 0) muse_qwen_asr_cancel();
    if (s_status.load() != 0 || !generation || bytes < 9600 || bytes > 640000 || bytes % 2) return false;
    static const char head[] = "{\"model\":\"qwen3-asr-flash\",\"stream\":false,\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"data:audio/wav;base64,";
    static const char tail[] = "\"}}]}],\"asr_options\":{\"language\":\"zh\",\"enable_itn\":false}}";
    size_t wav_bytes = bytes + MUSE_HATCH_WAV_HEADER;
    auto wav = static_cast<uint8_t *>(heap_caps_malloc(wav_bytes, MALLOC_CAP_SPIRAM));
    auto req = static_cast<request_t *>(calloc(1, sizeof(request_t)));
    size_t capacity = sizeof(head) + (wav_bytes + 2) / 3 * 4 + sizeof(tail);
    char *body = static_cast<char *>(heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM));
    if (!wav || !req || !body) { free(wav); free(req); free(body); return false; }
    muse_hatch_wav_header(wav, 16000);
    le32(wav + 4, wav_bytes - 8);
    le32(wav + 40, bytes);
    memcpy(wav + MUSE_HATCH_WAV_HEADER, pcm, bytes);
    memcpy(body, head, sizeof(head) - 1);
    size_t end = sizeof(head) - 1 + muse_hatch_base64(wav, wav_bytes, body + sizeof(head) - 1);
    memcpy(body + end, tail, sizeof(tail));
    free(wav);
    req->body = body; req->generation = generation;
    s_generation.store(generation); s_status.store(1);
    if (xTaskCreate(worker, "qwen_asr", 8192, req, 3, nullptr) != pdPASS) {
        free(body); free(req); s_status.store(0); return false;
    }
    return true;
}

int muse_qwen_asr_result(uint32_t generation, char **text) {
    int status = s_status.load();
    if (status == 0 || status == 1) return 0;
    if (!current(generation)) return -1;
    *text = s_text; s_text = nullptr; s_status.store(0);
    return status == 2 ? 1 : -1;
}

void muse_qwen_asr_cancel(void) {
    s_generation.store(0);
    if (s_status.load() != 1) {
        free(s_text); s_text = nullptr; s_status.store(0);
    }
}

bool muse_qwen_asr_busy(void) { return s_status.load() == 1; }
