#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "network_feedback.h"

typedef struct {
    bool tracking, warned, explained, quiet_outage;
    int64_t since_ms;
} network_policy_t;

// 每次故障只提醒一次；状态改变不重置整个故障期的计时。
static inline network_notice_t network_policy_step(network_policy_t *p, network_state_t state,
                                                   int64_t now, bool quiet, bool query) {
    if (state == NET_SETUP) {
        *p = (network_policy_t){0};
        return query && !quiet ? NET_NOTICE_PAIR : NET_NOTICE_NONE;
    }
    if (state == NET_READY) {
        bool recovered = p->tracking && now-p->since_ms >= 15000 && !p->quiet_outage;
        p->tracking=false; p->warned=p->explained=false; p->quiet_outage=false;
        return quiet ? NET_NOTICE_NONE : query ? NET_NOTICE_READY : recovered ? NET_NOTICE_RECOVERED : NET_NOTICE_NONE;
    }
    if (!p->tracking) { p->tracking=true; p->since_ms=now; }
    p->quiet_outage |= quiet;
    int64_t elapsed=now-p->since_ms;
    network_notice_t notice=NET_NOTICE_NONE;
    if (!p->warned && elapsed>=15000) { p->warned=true; notice=NET_NOTICE_DELAY; }
    if (!p->explained && elapsed>=60000) {
        p->explained=true;
        notice=state==NET_WIFI ? NET_NOTICE_WIFI : state==NET_PAIR ? NET_NOTICE_PAIR : NET_NOTICE_SERVICE;
    }
    if (query) notice=state==NET_WIFI ? NET_NOTICE_WIFI : state==NET_PAIR ? NET_NOTICE_PAIR : NET_NOTICE_SERVICE;
    return quiet || (!query && p->quiet_outage) ? NET_NOTICE_NONE : notice;
}
