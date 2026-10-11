// SPDX-License-Identifier: Apache-2.0
#include "voice_wake.h"
#include "voice_board.h"
#include "led_status.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_afe_config.h"
#include "model_path.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

static const char *TAG = "link.wake";
// 512个16kHz采样需32ms，等待须覆盖整块采音及AFE处理调度。
enum { FETCH_WAIT_MS = 100 };
static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_data;
static srmodel_list_t *s_models;
static SemaphoreHandle_t s_feed_lock;
static TaskHandle_t s_feed_task;
static int16_t *s_feed_pcm, *s_read_pcm;
static int s_feed_frames, s_read_capacity;
static size_t s_read_frames, s_read_position;
static bool s_read_speech, s_active, s_wait_gap, s_speech_started;
static atomic_bool s_enabled;
static atomic_int s_error;

void voice_wake_link_compat(void);

// 官方加载器直接信任文件数量和偏移；先拒绝空白或不完整的模型镜像。
static esp_err_t check_model_image(void) {
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
    if (!partition) return ESP_ERR_NOT_FOUND;
    struct {
        uint32_t count;
        struct {
            char name[32];
            uint32_t count;
            struct { char name[32]; uint32_t offset, size; } files[3];
        } models[2];
    } header;
    _Static_assert(sizeof(header) == 316, "ESP-SR model header size");
    esp_err_t err = esp_partition_read(partition, 0, &header, sizeof(header));
    if (err != ESP_OK) return err;
    if (header.count != 2) return ESP_ERR_INVALID_SIZE;
    unsigned models_seen = 0;
    for (unsigned i = 0; i < 2; ++i) {
        if (header.models[i].count != 3) return ESP_ERR_INVALID_SIZE;
        int model = !strncmp(header.models[i].name, "wn9_nihaoxiaoyi_tts2", 32) ? 0 :
                    !strncmp(header.models[i].name, "vadnet1_medium", 32) ? 1 : -1;
        if (model < 0 || (models_seen & (1U << model))) return ESP_ERR_NOT_FOUND;
        models_seen |= 1U << model;
        const char *names[3] = {"_MODEL_INFO_", model ? "vadn1_index" : "wn9_index",
                              model ? "vadn1_data" : "wn9_data"};
        unsigned files_seen = 0;
        for (unsigned j = 0; j < 3; ++j) {
            uint32_t offset = header.models[i].files[j].offset;
            uint32_t size = header.models[i].files[j].size;
            if (offset < sizeof(header) || offset >= partition->size ||
                !size || size > partition->size - offset) return ESP_ERR_INVALID_SIZE;
            for (unsigned k = 0; k < 3; ++k)
                if (!strncmp(header.models[i].files[j].name, names[k], 32)) files_seen |= 1U << k;
        }
        if (files_seen != 7) return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

static void feed_task(void *arg) {
    for (;;) {
        if (!atomic_load(&s_enabled)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        xSemaphoreTake(s_feed_lock, portMAX_DELAY);
        if (atomic_load(&s_enabled)) {
            size_t filled = 0;
            while (filled < (size_t)s_feed_frames && atomic_load(&s_enabled)) {
                int peak = 0;
                size_t count = s_feed_frames - filled;
                if (count > 320) count = 320;
                size_t got = voice_board_mic_read(s_feed_pcm + filled, count, &peak);
                if (!got) {
                    atomic_store(&s_error, ESP_FAIL);
                    atomic_store(&s_enabled, false);
                    ESP_LOGE(TAG, "microphone feed failed");
                    break;
                }
                filled += got;
            }
            if (atomic_load(&s_enabled) && filled == (size_t)s_feed_frames)
                s_afe->feed(s_data, s_feed_pcm);
        }
        xSemaphoreGive(s_feed_lock);
        vTaskDelay(1);
    }
}

esp_err_t voice_wake_init(void) {
    voice_wake_link_compat();
    esp_err_t err = check_model_image();
    if (err != ESP_OK) return err;
    s_models = esp_srmodel_init("model");
    if (!s_models) return ESP_ERR_NOT_FOUND;
    char *wake_model = esp_srmodel_filter(s_models, "wn9_nihaoxiaoyi_tts2", NULL);
    char *vad_model = esp_srmodel_filter(s_models, "vadnet1_medium", NULL);
    if (!wake_model || !vad_model) return ESP_ERR_NOT_FOUND;
    afe_config_t *cfg = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!cfg) return ESP_ERR_NO_MEM;
    cfg->wakenet_model_name = wake_model;
    cfg->wakenet_model_name_2 = NULL;
    cfg->vad_model_name = vad_model;
    cfg->aec_init = false;
    cfg->se_init = false;
    cfg->ns_init = false;
    cfg->agc_init = false;
    cfg->vad_init = true;
    cfg->wakenet_init = true;
    cfg->vad_min_noise_ms = 128;
    cfg->vad_min_speech_ms = 128;
    cfg->vad_delay_ms = 128;
    cfg->afe_linear_gain = 1.0f;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    s_afe = esp_afe_handle_from_config(cfg);
    if (s_afe) s_data = s_afe->create_from_config(cfg);
    afe_config_free(cfg);
    if (!s_data) return ESP_ERR_NO_MEM;
    s_feed_frames = s_afe->get_feed_chunksize(s_data);
    int fetch_frames = s_afe->get_fetch_chunksize(s_data);
    if (s_feed_frames <= 0 || fetch_frames <= 0 ||
        s_afe->get_feed_channel_num(s_data) != 1 || s_afe->get_samp_rate(s_data) != 16000)
        return ESP_ERR_INVALID_SIZE;
    // VAD预缓存最多留256ms，超出边界报错，不截断后继续提交。
    s_read_capacity = fetch_frames + 4096;
    s_feed_pcm = heap_caps_malloc(s_feed_frames * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_read_pcm = heap_caps_malloc(s_read_capacity * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_feed_lock = xSemaphoreCreateMutex();
    if (!s_feed_pcm || !s_read_pcm || !s_feed_lock) return ESP_ERR_NO_MEM;
    if (xTaskCreateWithCaps(feed_task, "wake_feed", 4096, NULL, 4, &s_feed_task, MALLOC_CAP_SPIRAM) != pdPASS)
        return ESP_ERR_NO_MEM;
    s_afe->print_pipeline(s_data);
    ESP_LOGI(TAG, "offline wake ready: 你好小益; feed=%d fetch=%d; PSRAM free=%u largest=%u",
             s_feed_frames, fetch_frames, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}

esp_err_t voice_wake_start(void) {
    if (s_active) return voice_wake_error();
    if (!s_data) return ESP_ERR_INVALID_STATE;
    s_afe->reset_buffer(s_data);
    s_afe->reset_vad(s_data);
    s_afe->enable_wakenet(s_data);
    s_read_frames = s_read_position = 0;
    s_speech_started = s_wait_gap = false;
    atomic_store(&s_error, ESP_OK);
    esp_err_t err = voice_board_mic_start();
    if (err != ESP_OK) return err;
    s_active = true;
    atomic_store(&s_enabled, true);
    led_status_set_wake_listening(true);
    ESP_LOGI(TAG, "listening locally");
    return ESP_OK;
}

esp_err_t voice_wake_stop(void) {
    if (!s_active) return ESP_OK;
    atomic_store(&s_enabled, false);
    led_status_set_wake_listening(false);
    // 等最后一次采音退出后，才允许手动录音接管I2S。
    if (xSemaphoreTake(s_feed_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        atomic_store(&s_error, ESP_ERR_TIMEOUT);
        return ESP_ERR_TIMEOUT;
    }
    voice_board_mic_stop();
    s_active = false;
    s_read_frames = s_read_position = 0;
    s_afe->reset_buffer(s_data);
    xSemaphoreGive(s_feed_lock);
    ESP_LOGI(TAG, "local listening stopped; internal free=%u largest=%u; PSRAM free=%u largest=%u; feed stack minimum=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned)uxTaskGetStackHighWaterMark(s_feed_task));
    return ESP_OK;
}

bool voice_wake_poll(void) {
    if (!s_active || voice_wake_error() != ESP_OK) return false;
    afe_fetch_result_t *res = s_afe->fetch_with_delay(s_data, pdMS_TO_TICKS(FETCH_WAIT_MS));
    if (!res || res->ret_value != ESP_OK) return false;
    if (res->wakeup_state != WAKENET_DETECTED) return false;
    s_afe->disable_wakenet(s_data);
    s_read_frames = s_read_position = 0;
    // 先等唤醒词尾音结束，再接收问题；首版用户应在蓝灯出现后稍停再说。
    s_wait_gap = true;
    s_speech_started = false;
    ESP_LOGI(TAG, "wake detected; automatic recording");
    return true;
}

size_t voice_wake_read(int16_t *pcm, size_t capacity, bool *speech, int *peak) {
    *speech = false;
    *peak = 0;
    if (!s_active || !capacity || voice_wake_error() != ESP_OK) return 0;
    if (s_read_position == s_read_frames) {
        int64_t deadline = esp_timer_get_time() + 500000;
        afe_fetch_result_t *res = NULL;
        do {
            res = s_afe->fetch_with_delay(s_data, pdMS_TO_TICKS(FETCH_WAIT_MS));
            if (res && res->ret_value == ESP_OK && res->data_size > 0) break;
        } while (esp_timer_get_time() < deadline && voice_wake_error() == ESP_OK);
        if (!res || res->ret_value != ESP_OK || res->data_size <= 0) return 0;
        s_read_speech = res->vad_state == VAD_SPEECH;
        if (s_wait_gap) {
            if (!s_read_speech) s_wait_gap = false;
            s_read_speech = false;
        }
        if (!res->data || res->vad_cache_size < 0) {
            atomic_store(&s_error, ESP_ERR_INVALID_SIZE);
            return 0;
        }
        size_t cache = !s_speech_started && s_read_speech ? res->vad_cache_size : 0;
        size_t bytes = res->data_size;
        if (((bytes | cache) & 1) || (cache && !res->vad_cache) || cache > 8192 ||
            bytes + cache > (size_t)s_read_capacity * sizeof(int16_t)) {
            atomic_store(&s_error, ESP_ERR_INVALID_SIZE);
            return 0;
        }
        if (cache) memcpy(s_read_pcm, res->vad_cache, cache);
        memcpy((uint8_t *)s_read_pcm + cache, res->data, bytes);
        s_read_frames = (cache + bytes) / sizeof(int16_t);
        s_read_position = 0;
        if (s_read_speech) s_speech_started = true;
    }
    size_t n = s_read_frames - s_read_position;
    if (n > capacity) n = capacity;
    memcpy(pcm, s_read_pcm + s_read_position, n * sizeof(int16_t));
    s_read_position += n;
    *speech = s_read_speech;
    for (size_t i = 0; i < n; ++i) {
        int level = pcm[i] < 0 ? -(int)pcm[i] : pcm[i];
        if (level > *peak) *peak = level;
    }
    return n;
}

esp_err_t voice_wake_error(void) { return atomic_load(&s_error); }
