#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

static inline bool muse_briefing_reject(const char **reason, const char *code) {
    if (reason) *reason = code;
    return false;
}

// 只规范日期表示，不把正文中的事件日期当成简报日期。
static inline const char *muse_briefing_date(const char *p, const char *end, int date[3]) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    int year = 0, month = 0, day = 0;
    for (int i = 0; i < 4; ++i) {
        if (p >= end || *p < '0' || *p > '9') return NULL;
        year = year * 10 + *p++ - '0';
    }
    bool chinese = end-p >= 3 && !memcmp(p, "年", 3);
    if (chinese) p += 3;
    else if (p < end && *p == '-') ++p;
    else return NULL;
    int digits = 0;
    while (p < end && *p >= '0' && *p <= '9' && digits < 2) { month = month*10 + *p++-'0'; ++digits; }
    if (!digits || (!chinese && digits != 2)) return NULL;
    if (chinese && end-p >= 3 && !memcmp(p, "月", 3)) p += 3;
    else if (!chinese && p < end && *p == '-') ++p;
    else return NULL;
    digits = 0;
    while (p < end && *p >= '0' && *p <= '9' && digits < 2) { day = day*10 + *p++-'0'; ++digits; }
    if (!digits || (!chinese && digits != 2) || (p < end && *p >= '0' && *p <= '9')) return NULL;
    if (chinese) {
        if (end-p < 3 || memcmp(p, "日", 3)) return NULL;
        p += 3;
    }
    const int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (year < 2026 || month < 1 || month > 12 || day < 1) return NULL;
    int maximum = days[month-1] + (month == 2 && year%4 == 0 && (year%100 != 0 || year%400 == 0));
    if (day > maximum) return NULL;
    date[0] = year; date[1] = month; date[2] = day;
    return p;
}

static inline const char *muse_briefing_url(const char *p) {
    while ((p = strstr(p, "http"))) {
        if (!strncmp(p, "https://", 8) || !strncmp(p, "http://", 7)) return p;
        p += 4;
    }
    return NULL;
}

// 参考URL只做文本校验，不在设备上访问；中文破折号不能吞进URL。
static inline size_t muse_briefing_url_size(const char *p) {
    const char *end = p;
    while (*end && !strchr(" \t\r\n)]<>", *end)) {
        if (!strncmp(end, "—", 3) || !strncmp(end, "，", 3) ||
            !strncmp(end, "；", 3) || !strncmp(end, "。", 3)) break;
        ++end;
    }
    return end-p;
}

// 从完整消息中提取朗读正文，App原文保持不变。这里是格式检查，不是事实核查。
// 接受固定标记，或“首行日期 + 正文 + 末尾编号来源”，不另发生成/重试请求。
static inline bool muse_briefing_speech(char *text, const char **reason) {
    if (reason) *reason = "ok";
    if (!text || !*text) return muse_briefing_reject(reason, "empty_message");
    const char *date_marker = "【北京时间】";
    const char *body_marker = "【简报正文】";
    const char *source_marker = "【参考来源】";
    const char *end = text + strlen(text);
    const char *date = strstr(text, date_marker);
    date = date ? date + strlen(date_marker) : text;
    int header_date[3], spoken_date[3];
    const char *after_date = muse_briefing_date(date, end, header_date);
    if (!after_date) return muse_briefing_reject(reason, "header_date_invalid");
    char *body = strstr(text, body_marker);
    if (body) body += strlen(body_marker);
    else {
        body = strchr((char *)after_date, '\n');
        if (!body) return muse_briefing_reject(reason, "body_missing");
        ++body;
    }
    char *sources = strstr(text, source_marker);
    const char *links = sources ? sources + strlen(source_marker) : NULL;
    if (!sources) {
        // 无标记时，只从末尾编号来源列表分离正文，不能在正文任意URL处截断。
        for (char *line = body; line && *line;) {
            while (*line == ' ' || *line == '\t') ++line;
            char *next = strchr(line, '\n');
            if (!strncmp(line, "1.", 2)) {
                const char *url = muse_briefing_url(line);
                if (url && (!next || url < next)) {
                    sources = line; links = line; break;
                }
            }
            line = next ? next+1 : NULL;
        }
    }
    if (!sources) return muse_briefing_reject(reason, "sources_missing");
    if (body < after_date || sources < body || date >= sources)
        return muse_briefing_reject(reason, "section_order_invalid");
    while (body < sources && strchr(" \t\r\n", *body)) ++body;
    size_t length = sources-body;
    while (length && strchr(" \t\r\n", body[length-1])) --length;
    if (!length) return muse_briefing_reject(reason, "body_empty");

    const char *spoken = strstr(body, "北京时间");
    if (!spoken || spoken >= body+length ||
        !muse_briefing_date(spoken+strlen("北京时间"), body+length, spoken_date))
        return muse_briefing_reject(reason, "spoken_date_invalid");
    for (int i = 0; i < 3; ++i)
        if (header_date[i] != spoken_date[i]) return muse_briefing_reject(reason, "date_mismatch");

    const char *urls[3]; size_t sizes[3]; int count = 0;
    for (const char *p = muse_briefing_url(links); p && count < 3;) {
        size_t scheme = !strncmp(p, "https://", 8) ? 8 : 7;
        size_t size = muse_briefing_url_size(p);
        if (size <= scheme) return muse_briefing_reject(reason, "source_url_invalid");
        const char *host = p+scheme;
        size_t host_size = strcspn(host, "/?# \t\r\n)]<>");
        if (host_size > size-scheme) host_size = size-scheme;
        if (!memchr(host, '.', host_size) || memchr(host, '@', host_size))
            return muse_briefing_reject(reason, "source_url_invalid");
        bool duplicate = false;
        for (int i = 0; i < count; ++i)
            duplicate |= sizes[i] == size-scheme && !memcmp(urls[i], host, size-scheme);
        if (!duplicate) { urls[count] = host; sizes[count] = size-scheme; ++count; }
        p = muse_briefing_url(p+size);
    }
    if (count < 3) return muse_briefing_reject(reason, "distinct_sources_less_than_three");
    for (size_t i = 0; i < length; ++i) {
        if (strchr("[]*#`<>|", body[i])) return muse_briefing_reject(reason, "body_markup");
        if ((i+7 <= length && !memcmp(body+i, "http://", 7)) ||
            (i+8 <= length && !memcmp(body+i, "https://", 8)))
            return muse_briefing_reject(reason, "body_url");
    }
    memmove(text, body, length);
    text[length] = 0;
    return true;
}
