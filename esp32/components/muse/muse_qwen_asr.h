#pragma once
#include <stddef.h>
#include <stdint.h>

// 启动时复制PCM；结果字符串由调用者释放，取消后不再交付旧结果。
bool muse_qwen_asr_start(const uint8_t *pcm, size_t bytes, uint32_t generation);
int muse_qwen_asr_result(uint32_t generation, char **text);
bool muse_qwen_asr_busy(void);
void muse_qwen_asr_cancel(void);
