#include "easyinput_oled.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "easyinput_board.h"
#include "easyinput_brightness.h"
#include "led_status.h"
#include "network_feedback.h"
#include "oled_text.h"
#include "voice.h"

#if !CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_SECONDARY_UART
#error "EasyInput OLED requires USB Serial/JTAG console without secondary UART"
#endif

static const char *TAG = "link.oled";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_device;
static atomic_llong s_headpat_until;
// 仅由 OLED 任务维护；其他任务通过原子活动时间通知。
static int64_t s_last_active_time;
static bool s_is_sleeping;

void easyinput_oled_headpat(void) {
    int64_t now = esp_timer_get_time();
    atomic_store(&s_headpat_until, now + 3500000LL);
}

static esp_err_t commands(const uint8_t *data, size_t size) {
    uint8_t packet[32] = {0x00};
    if (size > sizeof(packet) - 1) return ESP_ERR_INVALID_SIZE;
    memcpy(packet + 1, data, size);
    return i2c_master_transmit(s_device, packet, size + 1, 100);
}

static esp_err_t display_frame(const uint8_t *frame) {
    for (unsigned page = 0; page < 8; ++page) {
        const uint8_t position[] = {0xb0 | page, 0x00, 0x10};
        esp_err_t err = commands(position, sizeof(position));
        if (err != ESP_OK) return err;
        uint8_t packet[129] = {0x40};
        memcpy(packet + 1, frame + page * 128, 128);
        err = i2c_master_transmit(s_device, packet, sizeof(packet), 100);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

static const char *network_short_label(int state) {
    switch (state) {
        case NET_SETUP: return "NET: SETUP";
        case NET_WIFI: return "NET: WIFI...";
        case NET_SERVICE: return "NET: MUSE...";
        case NET_PAIR: return "NET: PAIR";
        case NET_READY: return "NET: ONLINE";
        default: return "NET: CONNECT";
    }
}

// 32x32 灵动机器人萌宠单色位图（共 128 字节，4 个 Page，每 Page 32 列）
// 1. 待机/思考/空闲 (IDLE): 圆眼高光 + 浅笑
static const uint8_t s_robot_idle[128] = {
    // Page 0
    0x00, 0x00, 0x00, 0xf8, 0x04, 0x04, 0x84, 0xf8, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0xf8, 0x84, 0x04, 0x04, 0xf8, 0x00, 0x00, 0x00,
    // Page 1
    0x00, 0x00, 0xfe, 0x00, 0x01, 0x01, 0x00, 0xf0, 0x90, 0xf0, 0xf0, 0xf0, 0xf0, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xf0, 0x90, 0xf0, 0xf0, 0xf0, 0xf0, 0x00, 0x01, 0x01, 0x00, 0xfe, 0x00, 0x00,
    // Page 2
    0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x03, 0x03, 0x03, 0x03, 0x03, 0x43, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x80, 0x43, 0x03, 0x03, 0x03, 0x03, 0x03, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00,
    // Page 3
    0x00, 0x00, 0x07, 0x08, 0x08, 0x10, 0x10, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x21, 0x21,
    0x21, 0x21, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x10, 0x10, 0x08, 0x08, 0x07, 0x00, 0x00
};

// 2. 倾听中/录音 (LISTEN): 竖起耳朵天线信号 + 聚焦大圆眼 + 微张嘴
static const uint8_t s_robot_listen[128] = {
    // Page 0
    0x00, 0x00, 0x00, 0xf8, 0x06, 0x05, 0x86, 0xf8, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0xf8, 0x86, 0x05, 0x06, 0xf8, 0x00, 0x00, 0x00,
    // Page 1
    0x00, 0x00, 0xfe, 0x00, 0x01, 0x01, 0xf8, 0xf8, 0x18, 0x18, 0x18, 0xf8, 0xf8, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xf8, 0xf8, 0x18, 0x18, 0x18, 0xf8, 0xf8, 0x01, 0x01, 0x00, 0xfe, 0x00, 0x00,
    // Page 2
    0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x00, 0x80, 0x40,
    0x40, 0x80, 0x00, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00,
    // Page 3
    0x00, 0x00, 0x07, 0x08, 0x08, 0x10, 0x10, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x21,
    0x21, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x10, 0x10, 0x08, 0x08, 0x07, 0x00, 0x00
};

// 3. 讲话中/播报 (SPEAK): 弯弯笑眼 (^ ^) + 开合嘴
static const uint8_t s_robot_speak[128] = {
    // Page 0
    0x00, 0x00, 0x00, 0xf8, 0x04, 0x04, 0x84, 0xf8, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0xf8, 0x84, 0x04, 0x04, 0xf8, 0x00, 0x00, 0x00,
    // Page 1
    0x00, 0x00, 0xfe, 0x00, 0x01, 0x01, 0x80, 0xc0, 0x60, 0x60, 0x60, 0xc0, 0x80, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x80, 0xc0, 0x60, 0x60, 0x60, 0xc0, 0x80, 0x01, 0x01, 0x00, 0xfe, 0x00, 0x00,
    // Page 2
    0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00,
    // Page 3
    0x00, 0x00, 0x07, 0x08, 0x08, 0x10, 0x10, 0x20, 0x20, 0x20, 0x20, 0x20, 0x21, 0x22, 0x22, 0x23,
    0x23, 0x22, 0x22, 0x21, 0x20, 0x20, 0x20, 0x20, 0x20, 0x10, 0x10, 0x08, 0x08, 0x07, 0x00, 0x00
};

// 4. 打瞌睡/睡眠 (SLEEP): 闭眼安睡 (u u) + 漂浮微梦 Zzz
static const uint8_t s_robot_sleep[128] = {
    // Page 0
    0x00, 0x00, 0x00, 0xf8, 0x04, 0x04, 0x84, 0xf8, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,
    0x40, 0x40, 0x40, 0x62, 0x72, 0x6a, 0x66, 0x62, 0xf8, 0x84, 0x04, 0x04, 0xf8, 0x00, 0x00, 0x00,
    // Page 1
    0x00, 0x00, 0xfe, 0x00, 0x01, 0x01, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x8d, 0x0b, 0x09, 0x00, 0xfe, 0x00, 0x00,
    // Page 2
    0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x80,
    0x80, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00,
    // Page 3
    0x00, 0x00, 0x07, 0x08, 0x08, 0x10, 0x10, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x10, 0x10, 0x08, 0x08, 0x07, 0x00, 0x00
};

// 5. 饥饿/委屈求充 (HUNGRY): 泪光大眼 + 下垂扁嘴
static const uint8_t s_robot_hungry[128] = {
    // Page 0
    0x00, 0x00, 0x00, 0xf8, 0x04, 0x04, 0x84, 0xf8, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0xf8, 0x84, 0x04, 0x04, 0xf8, 0x00, 0x00, 0x00,
    // Page 1
    0x00, 0x00, 0xfe, 0x00, 0x01, 0x01, 0x00, 0xe0, 0xe0, 0xe0, 0xe0, 0xc0, 0x80, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x80, 0xc0, 0xe0, 0xe0, 0xe0, 0xe0, 0x00, 0x01, 0x01, 0x00, 0xfe, 0x00, 0x00,
    // Page 2
    0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x07, 0x27, 0x77, 0x27, 0x07, 0x07, 0x00, 0x80, 0x80,
    0x80, 0x80, 0x00, 0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00,
    // Page 3
    0x00, 0x00, 0x07, 0x08, 0x08, 0x10, 0x10, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x21, 0x20, 0x20,
    0x20, 0x20, 0x21, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x10, 0x10, 0x08, 0x08, 0x07, 0x00, 0x00
};

// 6. 充能饱食/大喜 (CHARGING): 闪电额头 + 星星眼 (★ ★) + 大笑嘴
static const uint8_t s_robot_charging[128] = {
    // Page 0
    0x00, 0x00, 0x00, 0xf8, 0x04, 0x04, 0x84, 0xf8, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0xf8, 0x84, 0x04, 0x04, 0xf8, 0x00, 0x00, 0x00,
    // Page 1
    0x00, 0x00, 0xfe, 0x00, 0x01, 0x01, 0x00, 0x80, 0xc0, 0xe0, 0xc0, 0x80, 0x00, 0x00, 0x0c, 0x14,
    0x06, 0x07, 0x00, 0x00, 0x80, 0xc0, 0xe0, 0xc0, 0x80, 0x00, 0x01, 0x01, 0x00, 0xfe, 0x00, 0x00,
    // Page 2
    0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x03, 0x01, 0x00, 0x80, 0x40, 0x40, 0x40,
    0x40, 0x40, 0x40, 0x80, 0x00, 0x01, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00,
    // Page 3
    0x00, 0x00, 0x07, 0x08, 0x08, 0x10, 0x10, 0x20, 0x20, 0x20, 0x20, 0x20, 0x21, 0x20, 0x22, 0x22,
    0x22, 0x22, 0x20, 0x21, 0x20, 0x20, 0x20, 0x20, 0x20, 0x10, 0x10, 0x08, 0x08, 0x07, 0x00, 0x00
};

// 7. 摸摸头/撒娇 (LOVE): 漂浮爱心 (<3) + 害羞红晕 (///) + 眯眯笑眼
static const uint8_t s_robot_love[128] = {
    // Page 0
    0x00, 0x00, 0x00, 0xf8, 0x04, 0x04, 0x84, 0xf8, 0x40, 0x40, 0x40, 0x40, 0x40, 0x42, 0x45, 0x49,
    0x52, 0x49, 0x45, 0x42, 0x40, 0x40, 0x40, 0x40, 0xf8, 0x84, 0x04, 0x04, 0xf8, 0x00, 0x00, 0x00,
    // Page 1
    0x00, 0x00, 0xfe, 0x00, 0x01, 0x01, 0x80, 0xc0, 0x60, 0x20, 0x60, 0xc0, 0x80, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x80, 0xc0, 0x60, 0x20, 0x60, 0xc0, 0x80, 0x01, 0x01, 0x00, 0xfe, 0x00, 0x00,
    // Page 2
    0x00, 0x00, 0xff, 0x00, 0x00, 0x04, 0x02, 0x00, 0x04, 0x02, 0x00, 0x00, 0x40, 0x80, 0x00, 0x00,
    0x00, 0x00, 0x80, 0x40, 0x00, 0x00, 0x04, 0x02, 0x00, 0x04, 0x02, 0x00, 0x00, 0xff, 0x00, 0x00,
    // Page 3
    0x00, 0x00, 0x07, 0x08, 0x08, 0x10, 0x10, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x21, 0x21,
    0x21, 0x21, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x10, 0x10, 0x08, 0x08, 0x07, 0x00, 0x00
};

// 暂定电压估算刻度，非已标定的剩余容量；4215mV来自本机满电采样。
// 只用于带 ~ 标记的显示，不参与充电、低电量阈值或关机控制。
static int battery_percent_estimate(int mv) {
    static const int points[][2] = {
        {3300, 0}, {3500, 5}, {3600, 10}, {3700, 20}, {3800, 40},
        {3900, 60}, {4000, 80}, {4100, 90}, {4215, 100},
    };
    if (mv <= 0) return -1;
    if (mv <= points[0][0]) return 0;
    for (unsigned i = 1; i < sizeof(points) / sizeof(points[0]); ++i) {
        if (mv <= points[i][0]) {
            int span = points[i][0] - points[i-1][0];
            return points[i-1][1] + ((mv - points[i-1][0]) *
                (points[i][1] - points[i-1][1]) + span/2) / span;
        }
    }
    return 100;
}

static void render(uint8_t *frame, led_voice_t voice) {
    int state = network_feedback_state();
    int mv; bool external, charging;
    easyinput_board_power(&mv, &external, &charging);
    memset(frame, 0, 1024);

    int64_t now = esp_timer_get_time();
    int64_t headpat_until = atomic_load(&s_headpat_until);
    bool headpat = now < headpat_until;
    bool muted = easyinput_board_muted();
    bool paused = voice_playback_paused();
    bool halted = voice_halted();
    bool hungry = (!charging && !external && mv > 0 && mv < 3550);

    // 即使电台熄屏期间的活动已结束，也不丢失最后一次活动时间。
    int64_t activity = headpat_until - 3500000LL;
    if (activity > s_last_active_time) s_last_active_time = activity;
    if (voice != LED_VOICE_IDLE || headpat || charging) {
        s_last_active_time = now;
    }
    if (s_last_active_time == 0) s_last_active_time = now;

    bool sleeping = (voice == LED_VOICE_IDLE && state == NET_READY &&
                     !muted && !paused && !halted && !headpat && !charging && !hungry &&
                     (now - s_last_active_time > 45000000LL));
    s_is_sleeping = sleeping;

    // 功能状态优先于桌宠心情；右侧每行最多 15 个 ASCII 字符。
    const uint8_t *avatar = s_robot_idle;
    const char *status = "AI: READY";
    const char *hint = "S1 HOLD TO TALK";
    const char *shortcut = "S5 NEWS  S8 STORY";

    if (muted) {
        status = "MIC: MUTED";
        hint = "S2 TO UNMUTE";
        shortcut = "S2 UNMUTE";
    } else if (halted) {
        avatar = s_robot_hungry;
        status = "VOICE: STOPPED";
        hint = "CHECK FIRMWARE";
    } else if (voice == LED_VOICE_ERROR) {
        avatar = s_robot_hungry;
        led_status_oled_failure(&status, &hint);
    } else if (voice == LED_VOICE_REPLAY_WAIT) {
        status = "REPLAY: WAIT";
        hint = "AUTO RESUME";
        shortcut = "S4 CANCEL WAIT";
    } else if (paused) {
        status = "AUDIO: PAUSED";
        hint = "S3 TO RESUME";
        shortcut = "S3 RESUME  S4 CANCEL";
    } else if (voice == LED_VOICE_LISTENING || voice == LED_VOICE_END_CANDIDATE) {
        avatar = s_robot_listen;
        status = voice == LED_VOICE_LISTENING ? "MIC: RECORDING" : "MIC: PAUSE";
        hint = "KEEP TALKING";
        shortcut = "S4 CANCEL";
    } else if (voice == LED_VOICE_TRANSCRIBING) {
        status = "AI: RECOGNIZING";
        hint = "PLEASE WAIT";
        shortcut = "S4 CANCEL";
    } else if (voice == LED_VOICE_THINKING) {
        status = "AI: THINKING";
        hint = "PLEASE WAIT";
        shortcut = "S4 CANCEL";
    } else if (voice == LED_VOICE_BUFFERING) {
        status = "AUDIO: LOADING";
        hint = "PLEASE WAIT";
        shortcut = "S4 CANCEL";
    } else if (voice == LED_VOICE_SPEAKING) {
        avatar = s_robot_speak;
        status = "AUDIO: PLAYING";
        hint = "S4 TO PAUSE";
        shortcut = "S4 PAUSE  DIAL VOL";
    } else if (state != NET_READY) {
        status = "AI: NOT READY";
        hint = state == NET_SETUP || state == NET_PAIR ? "OPEN MUSE APP" : "HOLD S5: STATUS";
    } else if (headpat) {
        avatar = s_robot_love;
        status = "AI: HEART <3";
    } else if (charging) {
        avatar = s_robot_charging;
        status = "BAT: CHARGING";
    } else if (hungry) {
        avatar = s_robot_hungry;
        status = "BAT: LOW";
        hint = "PLUG IN USB";
    } else if (sleeping) {
        avatar = s_robot_sleep;
        status = "AI: SLEEPING";
        hint = "TURN DIAL: WAKE";
    }

    oled_draw_icon32(frame, 0, 2, avatar);

    // 2. 右侧核心状态 (Page 0..3)
    oled_text_at(frame, 0, 36, "TinyPal");
    oled_text_at(frame, 1, 36, network_short_label(state));
    oled_text_at(frame, 2, 36, status);
    oled_text_at(frame, 3, 36, hint);

    // 3. Page 4 顶部绘制微细虚线分割线 (y=32)
    for (unsigned col = 0; col < 128; col += 2) {
        frame[4 * 128 + col] |= 0x01;
    }

    // 4. 下半部 (Page 5..7, 全屏宽 128 像素) 显示硬件参数与快捷键
    char line[24];
    voice_dial_mode_t dial = voice_dial_mode();
    snprintf(line, sizeof(line), "%cVOL %3d%% %cBRI %3d%%",
             dial == VOICE_DIAL_VOLUME ? '>' : ' ', voice_volume(),
             dial == VOICE_DIAL_BRIGHTNESS ? '>' : ' ', easyinput_board_brightness());
    oled_text_line(frame, 5, line);

    int percent = battery_percent_estimate(mv);
    if (mv > 0 && mv <= 9999 && percent >= 0 && percent <= 100) {
        snprintf(line, sizeof(line), "BAT ~%d%% %d.%02dV %s", percent,
                 mv / 1000, (mv % 1000) / 10,
                 hungry ? "LOW" : charging ? "CHG" : external ? "USB" : "");
    } else {
        snprintf(line, sizeof(line), "BAT --%% --V %s", charging ? "CHG" : external ? "USB" : "");
    }
    oled_text_line(frame, 6, line);

    if (dial == VOICE_DIAL_BRIGHTNESS) {
        snprintf(line, sizeof(line), "BRIGHTNESS: %3d%%", easyinput_board_brightness());
        oled_text_line(frame, 7, line);
    } else if (dial == VOICE_DIAL_RADIO) {
        oled_text_line(frame, 7, "RADIO: TURN TO PICK");
    } else {
        oled_text_line(frame, 7, shortcut);
    }
}

static esp_err_t start_panel(void) {
    // 原UART0控制台必须已切到原生USB，再解除J4的GPIO复用。
    esp_err_t err = gpio_reset_pin(GPIO_NUM_43);
    if (err == ESP_OK) err = gpio_reset_pin(GPIO_NUM_44);
    if (err != ESP_OK) return err;
    const i2c_master_bus_config_t bus = {
        .i2c_port = -1, .sda_io_num = GPIO_NUM_43, .scl_io_num = GPIO_NUM_44,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    err = i2c_new_master_bus(&bus, &s_bus);
    if (err != ESP_OK) return err;
    // SSD1315的SA0决定地址，只探测该芯片的两个合法地址，不扫描其它设备。
    int address = 0;
    for (int candidate = 0x3c; candidate <= 0x3d; ++candidate) {
        err = i2c_master_probe(s_bus, candidate, 100);
        if (err == ESP_OK) { address = candidate; break; }
        if (err != ESP_ERR_NOT_FOUND) return err;
    }
    if (!address) return ESP_ERR_NOT_FOUND;
    const i2c_device_config_t device = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = address, .scl_speed_hz = 100000,
    };
    err = i2c_master_bus_add_device(s_bus, &device, &s_device);
    if (err != ESP_OK) return err;
    // SSD1315页寻址、64行、内部电荷泵；先清RAM，再点亮，避免上电杂点。
    const uint8_t init[] = {0xae, 0xd5, 0x80, 0xa8, 0x3f, 0xd3, 0x00, 0x40,
        0x8d, 0x14, 0x20, 0x02, 0xa1, 0xc8, 0xda, 0x12, 0x81, 0x20,
        0xd9, 0xf1, 0xdb, 0x30, 0xa4, 0xa6, 0x2e};
    err = commands(init, sizeof(init));
    if (err == ESP_OK) ESP_LOGI(TAG, "SSD1315 found at 0x%02x; SDA=43 SCL=44 100kHz", address);
    return err;
}

static void oled_task(void *arg) {
    uint8_t *frames = heap_caps_calloc(2, 1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_err_t err = frames ? start_panel() : ESP_ERR_NO_MEM;
    bool on = false, first = true;
    unsigned contrast = 0;
    unsigned precharge = 0;
    int logged_percent = -1;
    bool logged_sleeping = false, logged_dark = false;
    while (err == ESP_OK) {
        led_voice_t voice; bool dark;
        led_status_oled_snapshot(&voice, &dark);
        int percent = easyinput_board_brightness();
        // 沿用电台的实际熄灯状态，AI打断期间仍可显示；不改电台所有权。
        if (dark) {
            if (on) {
                const uint8_t off[] = {0xae}; err = commands(off, sizeof(off)); on = false;
                ESP_LOGI(TAG, "power diag: command=0xae result=%s", esp_err_to_name(err));
            }
            if (percent != logged_percent || !logged_dark)
                ESP_LOGI(TAG, "brightness diag: requested=%d dark=1 action=deferred", percent);
        } else {
            render(frames, voice);
            unsigned level = s_is_sleeping ? 1 : (1 + easyinput_brightness_scale(63, percent));
            unsigned period = easyinput_oled_precharge(s_is_sleeping ? 1 : percent);
            if (precharge != period) {
                const uint8_t timing[] = {0xd9, period};
                err = commands(timing, sizeof(timing));
                if (err == ESP_OK) precharge = period;
                ESP_LOGI(TAG, "precharge diag: requested=%d sleeping=%d command=0xd9 value=0x%02x result=%s",
                         percent, (int)s_is_sleeping, period, esp_err_to_name(err));
            }
            if (err == ESP_OK && contrast != level) {
                const uint8_t brightness[] = {0x81, level};
                err = commands(brightness, sizeof(brightness)); contrast = level;
                // 仅记录固定参数和传输结果；ESP_OK不代表已测得光学亮度变化。
                ESP_LOGI(TAG, "brightness diag: requested=%d sleeping=%d dark=0 command=0x81 value=0x%02x action=write result=%s",
                         percent, (int)s_is_sleeping, level, esp_err_to_name(err));
            } else if (err == ESP_OK && (percent != logged_percent || s_is_sleeping != logged_sleeping || logged_dark)) {
                ESP_LOGI(TAG, "brightness diag: requested=%d sleeping=%d dark=0 command=0x81 value=0x%02x action=cached result=not_sent",
                         percent, (int)s_is_sleeping, level);
            }
            if (err == ESP_OK && (first || memcmp(frames, frames + 1024, 1024))) {
                err = display_frame(frames);
                if (err == ESP_OK) { memcpy(frames + 1024, frames, 1024); first = false; }
            }
            if (err == ESP_OK && !on) {
                const uint8_t enable[] = {0xaf}; err = commands(enable, sizeof(enable)); on = err == ESP_OK;
                ESP_LOGI(TAG, "power diag: command=0xaf result=%s", esp_err_to_name(err));
            }
        }
        logged_percent = percent;
        logged_sleeping = s_is_sleeping;
        logged_dark = dark;
        if (err == ESP_OK) vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGE(TAG, "display stopped: %s; check J4 wiring and 3.3V pull-ups", esp_err_to_name(err));
    if (s_device) i2c_master_bus_rm_device(s_device);
    if (s_bus) i2c_del_master_bus(s_bus);
    free(frames);
    vTaskDeleteWithCaps(NULL);
}

void easyinput_oled_init(void) {
    // 此任务不执行Flash写入/模型初始化，栈放PSRAM以保留TLS内部RAM。
    if (xTaskCreateWithCaps(oled_task, "oled", 3072, NULL, 1, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS)
        ESP_LOGE(TAG, "cannot start display task");
}
