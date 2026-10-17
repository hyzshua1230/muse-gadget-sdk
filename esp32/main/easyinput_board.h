/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
#include <stdbool.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
/* 引脚依据：easyinput-board-cy 的 pinout 与两页原理图（AI Keyboard V2.1）。 */
esp_err_t easyinput_board_init(void);
void easyinput_board_green(bool ready);
int easyinput_board_brightness(void);
void easyinput_board_set_brightness(int percent);
bool easyinput_board_muted(void);
void easyinput_board_set_muted(bool muted);
/* 电压未标定时返回 0；外部供电不存在时 charging 不表示充满。 */
void easyinput_board_power(int *millivolts, bool *external, bool *charging);
#ifdef __cplusplus
}
#endif
