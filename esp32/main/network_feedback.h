#pragma once
#include <stdbool.h>

typedef enum { NET_SETUP, NET_WIFI, NET_SERVICE, NET_PAIR, NET_READY } network_state_t;
typedef enum { NET_NOTICE_NONE, NET_NOTICE_DELAY, NET_NOTICE_WIFI, NET_NOTICE_SERVICE,
               NET_NOTICE_PAIR, NET_NOTICE_READY, NET_NOTICE_RECOVERED } network_notice_t;

void network_feedback_configure(bool paired, bool credentials);
// 只由旋钮任务驱动；忙碌、静音或电台期间不排队补播。
void network_feedback_poll(bool quiet);
void network_feedback_request(void);
network_notice_t network_feedback_take(void);
int network_feedback_light(void); // 0不覆盖、1蓝呼吸、2黄呼吸、3黄间歇。
bool network_feedback_ready(void);
bool network_feedback_notice_valid(network_notice_t notice);
