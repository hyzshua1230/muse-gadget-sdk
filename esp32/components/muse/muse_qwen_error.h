#pragma once
#include <string.h>

// 只返回固定类别；服务端原文、请求地址和凭据均不进入日志。
static inline const char *muse_qwen_service_error(int status, const char *code) {
    if (!code) code = "";
    if (!strcmp(code, "AllocationQuota.FreeTierOnly") ||
        !strcmp(code, "QuotaExhausted") || !strcmp(code, "Arrearage") ||
        !strcmp(code, "InsufficientBalance")) return "quota";
    if (status == 401 || !strcmp(code, "InvalidApiKey") ||
        !strcmp(code, "invalid_api_key")) return "auth";
    if (status == 429 || !strncmp(code, "Throttling", 10) ||
        !strcmp(code, "rate_limit_exceeded")) return "rate_limit";
    if (status == 408 || status == 504 || !strcmp(code, "RequestTimeOut")) return "timeout";
    if (status == 403) return "forbidden"; // 不能仅凭403认定余额耗尽。
    return "service";
}
