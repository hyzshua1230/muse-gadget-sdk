#include "muse_qwen_asr.h"
#include "muse_chat_priv.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include "cJSON.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

// 生命周期与发送仅由hatch任务调用；回调只接收并发布状态。
static std::atomic<uint32_t> s_generation{0};
static std::atomic<int> s_status{0}; // 0空闲、1连接、2采音、3结束确认、4完成、-1失败
static std::atomic<bool> s_created{false};
static esp_websocket_client_handle_t s_client;
static char *s_rx, *s_tx, *s_text;
static size_t s_rx_used;
static int s_frame_read;
static bool s_rx_active;
static bool s_update_sent;
static uint32_t s_event;
static int64_t s_connect_deadline;
static constexpr size_t RX_BYTES = 16384, TX_BYTES = 4608;

static void receive_event(const char *json) {
    auto root = cJSON_Parse(json);
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
    if (!type) s_status.store(-1);
    else if (!strcmp(type, "session.created")) s_created.store(true);
    else if (!strcmp(type, "session.updated")) {
        auto detection = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "session"), "turn_detection");
        if (detection && !cJSON_IsNull(detection)) s_status.store(-1);
        else if (s_status.load() == 1) s_status.store(2);
    } else if (!strcmp(type, "conversation.item.input_audio_transcription.completed")) {
        const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "transcript"));
        // 只有整轮显式提交后才接受最终文字，不用片段或草稿触发Muse。
        if (s_status.load() != 3 || s_text || !text || !*text) s_status.store(-1);
        else {
            size_t bytes = strlen(text) + 1;
            s_text = static_cast<char *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
            if (s_text) memcpy(s_text, text, bytes);
            else s_status.store(-1);
        }
    } else if (!strcmp(type, "session.finished")) {
        s_status.store(s_status.load() == 3 && s_text ? 4 : -1);
    } else if (!strcmp(type, "error") ||
               !strcmp(type, "conversation.item.input_audio_transcription.failed")) {
        // 不打印服务端正文，避免凭据或用户音频内容进入错误日志。
        ESP_LOGW("qwen_asr", "realtime service rejected request");
        s_status.store(-1);
    }
    cJSON_Delete(root);
}

static void websocket_event(void *arg, esp_event_base_t, int32_t event, void *payload) {
    auto gen = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(arg));
    if (!gen || s_generation.load() != gen || s_status.load() == -1 || s_status.load() == 4) return;
    auto data = static_cast<esp_websocket_event_data_t *>(payload);
    if (event == WEBSOCKET_EVENT_ERROR || event == WEBSOCKET_EVENT_DISCONNECTED) {
        if (s_status.load() != 4) s_status.store(-1);
    } else if (event == WEBSOCKET_EVENT_DATA && (data->op_code == 1 || data->op_code == 0)) {
        // 同时处理WebSocket消息分片和组件缓冲区分块，不截断长转写。
        if (!data->payload_offset) {
            s_frame_read = 0;
            if (data->op_code == 1) {
                if (s_rx_active) { s_status.store(-1); return; }
                s_rx_active = true;
            } else if (!s_rx_active) { s_status.store(-1); return; }
        }
        if (data->data_len < 0 || data->payload_offset != s_frame_read ||
            data->data_len > data->payload_len - data->payload_offset ||
            s_rx_used + data->data_len >= RX_BYTES) { s_status.store(-1); return; }
        memcpy(s_rx + s_rx_used, data->data_ptr, data->data_len);
        s_rx_used += data->data_len;
        s_frame_read += data->data_len;
        if (data->fin && s_frame_read == data->payload_len) {
            s_rx[s_rx_used] = 0;
            receive_event(s_rx);
            s_rx_used = 0;
            s_rx_active = false;
        }
    }
}

static bool send_json(const char *text, size_t len) {
    if (!s_client || s_status.load() < 0) return false;
    int sent = esp_websocket_client_send_text(s_client, text, len, pdMS_TO_TICKS(100));
    if (sent != static_cast<int>(len)) { s_status.store(-1); return false; }
    return true;
}

bool muse_qwen_asr_begin(uint32_t gen) {
    muse_qwen_asr_cancel();
    if (!gen) return false;
    s_rx = static_cast<char *>(heap_caps_malloc(RX_BYTES, MALLOC_CAP_SPIRAM));
    s_tx = static_cast<char *>(heap_caps_malloc(TX_BYTES, MALLOC_CAP_SPIRAM));
    if (!s_rx || !s_tx) { muse_qwen_asr_cancel(); return false; }
    char headers[256];
    int len = snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", CONFIG_HOMEHUB_QWEN_API_KEY);
    if (len <= 0 || len >= static_cast<int>(sizeof(headers))) { muse_qwen_asr_cancel(); return false; }
    esp_websocket_client_config_t cfg = {};
    cfg.uri = "wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=qwen3-asr-flash-realtime";
    cfg.headers = headers;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.disable_auto_reconnect = true;
    cfg.task_stack = 8192;
    cfg.task_name = "qwen_asr_ws";
    cfg.buffer_size = 1024;
    cfg.network_timeout_ms = 10000;
    s_client = esp_websocket_client_init(&cfg);
    memset(headers, 0, sizeof(headers));
    if (!s_client) { muse_qwen_asr_cancel(); return false; }
    s_generation.store(gen);
    s_status.store(1);
    s_connect_deadline = esp_timer_get_time() + MUSE_QWEN_ASR_CONNECT_TIMEOUT_MS * 1000LL;
    if (esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, websocket_event,
            reinterpret_cast<void *>(static_cast<uintptr_t>(gen))) != ESP_OK ||
        esp_websocket_client_start(s_client) != ESP_OK) {
        muse_qwen_asr_cancel(); return false;
    }
    ESP_LOGI("qwen_asr", "realtime session opening (manual commit)");
    return true;
}

int muse_qwen_asr_ready(uint32_t gen) {
    if (s_generation.load() != gen || s_status.load() < 0) return -1;
    if (s_status.load() == 2) return 1;
    if (esp_timer_get_time() >= s_connect_deadline) { s_status.store(-1); return -1; }
    if (s_created.load() && !s_update_sent) {
        s_update_sent = true;
        const char *update = "{\"event_id\":\"config\",\"type\":\"session.update\",\"session\":{\"input_audio_format\":\"pcm\",\"sample_rate\":16000,\"input_audio_transcription\":{\"language\":\"zh\"},\"turn_detection\":null}}";
        if (!send_json(update, strlen(update))) return -1;
    }
    return 0;
}

bool muse_qwen_asr_audio(const uint8_t *pcm, size_t bytes, uint32_t gen) {
    if (s_generation.load() != gen || s_status.load() != 2 || !bytes || bytes > 3200 || bytes % 2) return false;
    int n = snprintf(s_tx, TX_BYTES, "{\"event_id\":\"audio_%lu\",\"type\":\"input_audio_buffer.append\",\"audio\":\"", static_cast<unsigned long>(++s_event));
    if (n <= 0) return false;
    size_t end = n + muse_hatch_base64(pcm, bytes, s_tx + n);
    memcpy(s_tx + end, "\"}", 2);
    return send_json(s_tx, end + 2);
}

bool muse_qwen_asr_finish(uint32_t gen) {
    if (s_generation.load() != gen || s_status.load() != 2) return false;
    s_status.store(3);
    const char *commit = "{\"event_id\":\"commit\",\"type\":\"input_audio_buffer.commit\"}";
    const char *finish = "{\"event_id\":\"finish\",\"type\":\"session.finish\"}";
    return send_json(commit, strlen(commit)) && send_json(finish, strlen(finish));
}

int muse_qwen_asr_result(uint32_t gen, char **text) {
    if (s_generation.load() != gen || s_status.load() < 0) return -1;
    if (s_status.load() != 4) return 0;
    // session.finished发布完成；停止回调后转移文字所有权。
    if (s_client) {
        esp_websocket_client_stop(s_client);
        esp_websocket_client_destroy(s_client);
        s_client = nullptr;
    }
    *text = s_text; s_text = nullptr;
    muse_qwen_asr_cancel();
    return 1;
}

void muse_qwen_asr_cancel(void) {
    s_generation.store(0);
    if (s_client) {
        esp_websocket_client_stop(s_client);
        esp_websocket_client_destroy(s_client);
        s_client = nullptr;
    }
    free(s_rx); free(s_tx); free(s_text);
    s_rx = s_tx = s_text = nullptr;
    s_rx_used = 0; s_frame_read = 0; s_event = 0; s_update_sent = false;
    s_rx_active = false;
    s_created.store(false); s_status.store(0);
}
