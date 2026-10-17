#pragma once
#include <stddef.h>
#include <stdint.h>

#define MUSE_QWEN_ASR_CONNECT_TIMEOUT_MS 15000

// 单一hatch任务控制生命周期；实时小块上传，结束前不提交转写。
bool muse_qwen_asr_begin(uint32_t generation);
int muse_qwen_asr_ready(uint32_t generation);
bool muse_qwen_asr_audio(const uint8_t *pcm, size_t bytes, uint32_t generation);
bool muse_qwen_asr_finish(uint32_t generation);
int muse_qwen_asr_result(uint32_t generation, char **text);
void muse_qwen_asr_cancel(void);
