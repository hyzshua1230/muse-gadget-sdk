#include "network_feedback.h"
#include "network_feedback_policy.h"
#include <stdatomic.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "wifi_mgr.h"
#include "noise_control.h"
#include "noise_tunnel.h"
#include "app.h"

static atomic_uint s_config;
static atomic_bool s_query;
static atomic_int s_state=NET_SETUP, s_notice;
static atomic_llong s_until_ms, s_since_ms;

void network_feedback_configure(bool paired, bool credentials) {
    atomic_store(&s_config,(paired ? 1u : 0u) | (credentials ? 2u : 0u));
}
void network_feedback_request(void) {
    atomic_store(&s_query,true);
    app_network_retry();
}
void network_feedback_poll(bool quiet) {
    static network_policy_t policy;
    static int64_t next_ms;
    int64_t now=esp_timer_get_time()/1000;
    bool query=atomic_exchange(&s_query,false);
    if (now<next_ms && !query) return;
    next_ms=now+100;
    unsigned config=atomic_load(&s_config);
    bool service_ready=noise_ctrl_is_connected();
#if CONFIG_HOMEHUB_TUNNEL
    service_ready=service_ready && noise_tunnel_is_connected();
#endif
    network_state_t state=!(config&1) ? NET_SETUP : !(config&2) ? NET_PAIR :
        !wifi_mgr_is_connected() ? NET_WIFI : service_ready ? NET_READY : NET_SERVICE;
    int old=atomic_exchange(&s_state,state);
    if (old!=state) ESP_LOGI("link.network","state=%d (0=setup 1=wifi 2=service 3=pair 4=ready)",state);
    network_notice_t notice=network_policy_step(&policy,state,now,quiet,query);
    atomic_store(&s_since_ms,policy.tracking ? policy.since_ms : now);
    if (old!=state || quiet) atomic_store(&s_notice,NET_NOTICE_NONE);
    if (notice) {
        // 先发布期限再发布通知；过期通知不会在通话后补播。
        atomic_store(&s_until_ms,now+1000);
        atomic_store(&s_notice,notice);
    }
}
network_notice_t network_feedback_take(void) {
    network_notice_t notice=atomic_exchange(&s_notice,NET_NOTICE_NONE);
    if (esp_timer_get_time()/1000>=atomic_load(&s_until_ms)) return NET_NOTICE_NONE;
    return notice;
}
bool network_feedback_ready(void) { return atomic_load(&s_state)==NET_READY; }
bool network_feedback_notice_valid(network_notice_t notice) {
    int state=atomic_load(&s_state);
    switch (notice) {
        case NET_NOTICE_WIFI: return state==NET_WIFI;
        case NET_NOTICE_SERVICE: return state==NET_SERVICE;
        case NET_NOTICE_PAIR: return state==NET_PAIR || state==NET_SETUP;
        case NET_NOTICE_READY: case NET_NOTICE_RECOVERED: return state==NET_READY;
        case NET_NOTICE_DELAY: return state==NET_WIFI || state==NET_SERVICE || state==NET_PAIR;
        default: return false;
    }
}
int network_feedback_light(void) {
    int state=atomic_load(&s_state);
    if (state==NET_SETUP || state==NET_READY) return 0;
    int64_t elapsed=esp_timer_get_time()/1000-atomic_load(&s_since_ms);
    if (elapsed<5000) return 0; // 短暂抖动不显示故障。
    return elapsed<15000 ? 1 : elapsed<60000 ? 2 : 3;
}
