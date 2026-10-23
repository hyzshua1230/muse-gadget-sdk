#include "easyinput_oled.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "easyinput_board.h"
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
        case 0: return "NET: SETUP APP";
        case 1: return "NET: WIFI CONN";
        case 2: return "NET: SRV CONN";
        case 3: return "NET: PAIR APP";
        case 4: return "NET: ONLINE";
        default: return "NET: CONNECT";
    }
}

static const char *voice_short_label(led_voice_t voice, bool paused) {
    if (easyinput_board_muted()) return "MIC: MUTED";
    if (paused) return "S3:  PAUSED";
    switch (voice) {
        case LED_VOICE_LISTENING:
        case LED_VOICE_END_CANDIDATE:
            return "MIC: LISTENING";
        case LED_VOICE_TRANSCRIBING:
        case LED_VOICE_THINKING:
            return "AI:  THINKING";
        case LED_VOICE_BUFFERING:
        case LED_VOICE_SPEAKING:
            return "AI:  SPEAKING";
        case LED_VOICE_ERROR:
            return "AI:  ERROR";
        case LED_VOICE_REPLAY_WAIT:
            return "AI:  REPLAY";
        case LED_VOICE_REPLAY_NOTICE:
            return "AI:  NO REPLAY";
        default:
            return "AI:  IDLE";
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

static void render(uint8_t *frame, led_voice_t voice) {
    int state = network_feedback_state();
    int mv; bool external, charging;
    easyinput_board_power(&mv, &external, &charging);
    memset(frame, 0, 1024);

    // 1. 左侧 (0..31 列, Page 0..3) 绘制 32x32 灵动机器人表情
    const uint8_t *avatar = s_robot_idle;
    if (voice == LED_VOICE_LISTENING || voice == LED_VOICE_END_CANDIDATE) {
        avatar = s_robot_listen;
    } else if (voice == LED_VOICE_SPEAKING || voice == LED_VOICE_BUFFERING) {
        avatar = s_robot_speak;
    }
    oled_draw_icon32(frame, 0, 2, avatar);

    // 2. 右侧 (36..127 列, Page 0..3) 紧凑显示系统核心状态
    oled_text_at(frame, 0, 36, "EasyInput");
    oled_text_at(frame, 1, 36, "Muse Companion");
    oled_text_at(frame, 2, 36, network_short_label(state));
    oled_text_at(frame, 3, 36, voice_short_label(voice, voice_playback_paused()));

    // 3. Page 4 顶部绘制微细虚线分割线 (y=32)
    for (unsigned col = 0; col < 128; col += 2) {
        frame[4 * 128 + col] |= 0x01;
    }

    // 4. 下半部 (Page 5..7, 全屏宽 128 像素) 显示硬件参数与快捷键
    char line[24];
    snprintf(line, sizeof(line), "VOL %3d%%  LIGHT %3d%%", voice_volume(), easyinput_board_brightness());
    oled_text_line(frame, 5, line);

    if (mv > 0) {
        snprintf(line, sizeof(line), "BAT %dmV %s", mv, charging ? "CHG" : external ? "USB" : "BAT");
    } else {
        snprintf(line, sizeof(line), "BAT --mV %s", external ? "USB" : "BAT");
    }
    oled_text_line(frame, 6, line);
    oled_text_line(frame, 7, "S5 NEWS  S8 STORY");
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
    while (err == ESP_OK) {
        led_voice_t voice; bool dark;
        led_status_oled_snapshot(&voice, &dark);
        // 沿用电台的实际熄灯状态，AI打断期间仍可显示；不改电台所有权。
        if (dark) {
            if (on) { const uint8_t off[] = {0xae}; err = commands(off, sizeof(off)); on = false; }
        } else {
            unsigned level = 1 + easyinput_board_brightness() * 63 / 100;
            if (contrast != level) {
                const uint8_t brightness[] = {0x81, level};
                err = commands(brightness, sizeof(brightness)); contrast = level;
            }
            render(frames, voice);
            if (err == ESP_OK && (first || memcmp(frames, frames + 1024, 1024))) {
                err = display_frame(frames);
                if (err == ESP_OK) { memcpy(frames + 1024, frames, 1024); first = false; }
            }
            if (err == ESP_OK && !on) {
                const uint8_t enable[] = {0xaf}; err = commands(enable, sizeof(enable)); on = err == ESP_OK;
            }
        }
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
