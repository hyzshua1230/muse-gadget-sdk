// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// 所有控制和读取接口只由语音任务调用；后台任务只负责采音及AFE feed。
esp_err_t voice_wake_init(void);
esp_err_t voice_wake_start(void);
esp_err_t voice_wake_stop(void);
bool voice_wake_poll(void);
size_t voice_wake_read(int16_t *pcm, size_t capacity, bool *speech, int *peak);
esp_err_t voice_wake_error(void);
