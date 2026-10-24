#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t radio_init(void);
void radio_toggle(void);
void radio_next(void);
void radio_select_mode(void);
bool radio_select_steps(int steps);
bool radio_select_confirm(void);
int radio_station(void);
// 仅由内部RAM栈的dial任务周期调用，合并保存已确认的频道。
void radio_poll_storage(void);
bool radio_enabled(void);
// AI先设置暂停并等待输出停止，再访问喇叭；恢复不重置睡眠定时。
bool radio_pause(bool pause);
bool radio_voice_command(const char *text);
