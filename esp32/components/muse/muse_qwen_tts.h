#pragma once
#include <stddef.h>
#include <stdint.h>

// 单个后台请求；会话任务独占启动、结果领取和取消。
bool muse_qwen_tts_start(const char *text, uint32_t generation);
int muse_qwen_tts_result(uint32_t generation, int16_t **pcm, size_t *frames);
void muse_qwen_tts_cancel(void);
