#pragma once
#include <string.h>

typedef struct { const char *status; const char *hint; } voice_failure_text_t;

// 仅输出固定短语，原始错误、凭据和URL不会作为显示字符串保存。
static inline voice_failure_text_t voice_failure_text(const char *why) {
    if (!why) why = "";
    if (strstr(why, "WAKE INIT FAILED"))
        return (voice_failure_text_t){"WAKE: FAILED", "CHECK FIRMWARE"};
    if (strstr(why, "MICROPHONE STOP") || strstr(why, "MICROPHONE WAKE"))
        return (voice_failure_text_t){"VOICE: STOPPED", "CHECK FIRMWARE"};
    if (strstr(why, "(dns)"))
        return (voice_failure_text_t){"NET: DNS ERROR", "CHECK NETWORK"};
    if (strstr(why, "SPEAKER"))
        return (voice_failure_text_t){"SPK: FAILED", "CHECK AUDIO"};
    if (strstr(why, "RADIO STOP"))
        return (voice_failure_text_t){"RADIO: BUSY", "TRY LATER"};
    if (strstr(why, "(timeout)") || strstr(why, "TIMEOUT"))
        return (voice_failure_text_t){"NET: TIMEOUT", "TRY LATER"};
    if (strstr(why, "(auth)"))
        return (voice_failure_text_t){"API: CHECK KEY", "CHECK API KEY"};
    if (strstr(why, "(quota)"))
        return (voice_failure_text_t){"API: NO QUOTA", "CHECK QUOTA"};
    if (strstr(why, "(rate_limit)"))
        return (voice_failure_text_t){"API: RATE LIMIT", "TRY LATER"};
    if (strstr(why, "(forbidden)"))
        return (voice_failure_text_t){"API: FORBIDDEN", "CHECK ACCESS"};
    if (strstr(why, "(transport)"))
        return (voice_failure_text_t){"NET: LINK ERROR", "CHECK NETWORK"};
    if (strstr(why, "(memory)") || strstr(why, "(buffer_limit)"))
        return (voice_failure_text_t){"AUDIO: MEMORY", "SHORTEN REPLY"};
    if (strstr(why, "(format)"))
        return (voice_failure_text_t){"AUDIO: FORMAT", "CHECK AUDIO"};
    if (strstr(why, "DATE/SOURCES"))
        return (voice_failure_text_t){"BRIEF: INVALID", "CHECK APP TEXT"};
    if (strstr(why, "MICROPHONE") || strstr(why, "RECORDING") || strstr(why, "CATCH"))
        return (voice_failure_text_t){"MIC: FAILED", "SPEAK AGAIN"};
    if (strstr(why, "ASR"))
        return (voice_failure_text_t){"ASR: FAILED", "HOLD S5: STATUS"};
    if (strstr(why, "TTS"))
        return (voice_failure_text_t){"TTS: FAILED", "HOLD S5: STATUS"};
    if (strstr(why, "MUSE"))
        return (voice_failure_text_t){"MUSE: OFFLINE", "HOLD S5: STATUS"};
    return (voice_failure_text_t){"AI: ERROR", "HOLD S5: STATUS"};
}
