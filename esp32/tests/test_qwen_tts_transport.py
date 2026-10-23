"""通过可控HTTP、时钟和取消状态执行生产fetch，不发送真实请求。"""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class QwenTransportTest(unittest.TestCase):
    def test_actual_fetch_cancel_timeout_errors_and_truncated_audio(self):
        cxx = shlex.split(os.environ.get('CXX', 'c++'))
        if not cxx or not shutil.which(cxx[0]):
            self.skipTest('主机C++编译器不可用，模拟HTTP执行未验证')
        production = (ROOT / 'components/muse/muse_qwen_tts.cpp').read_text(encoding='utf-8')
        fetch = production[production.index('static esp_err_t transport_event('):production.index('static bool synthesize(')]
        harness = r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include "muse_qwen_error.h"
using esp_err_t = int;
enum { ESP_OK=0, ESP_FAIL=-1, ESP_ERR_HTTP_EAGAIN=7, ESP_ERR_HTTP_READ_TIMEOUT=8,
       ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME=9, ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT=10,
       ESP_ERR_INVALID_ARG=11, HTTP_EVENT_ON_CONNECTED=1, HTTP_METHOD_POST=2, MALLOC_CAP_SPIRAM=1 };
#define CONFIG_HOMEHUB_QWEN_API_KEY "test-only-not-a-credential"
#define CONFIG_HOMEHUB_LED_BACKEND_EASYINPUT_V2 0
#define pdMS_TO_TICKS(ms) (ms)
static int mode, calls, cleanups, offset, status;
static unsigned generation=1;
static int64_t now;
static char log_line[256];
static const char *payload, *error_code;
static bool complete;
static bool current(uint32_t gen) { return generation==gen; }
static int64_t esp_timer_get_time() { return now; }
static void vTaskDelay(int ms) { now+=int64_t(ms)*1000; }
struct esp_http_client_event_t { int event_id; void *client; };
struct esp_http_client_config_t {
    const char *url; bool is_async; int timeout_ms; void *crt_bundle_attach;
    bool disable_auto_redirect; int max_authorization_retries;
    esp_err_t (*event_handler)(esp_http_client_event_t *);
};
static void *esp_crt_bundle_attach;
static void *esp_http_client_init(const esp_http_client_config_t *cfg) {
    assert(cfg->is_async && cfg->disable_auto_redirect && cfg->max_authorization_retries==-1);
    return (void *)1;
}
static int esp_http_client_set_timeout_ms(void *,int n) { assert(n<=1000); return 0; }
static int esp_http_client_set_method(void *,int) { return 0; }
static int esp_http_client_set_header(void *,const char *,const char *) { return 0; }
static int esp_http_client_open(void *,int) { return mode==3 ? ESP_FAIL : ESP_OK; }
static int esp_http_client_write(void *,const char *,int n) { return n; }
static int64_t esp_http_client_fetch_headers(void *) {
    ++calls; now+=250000;
    if (mode==1) { generation=0; return -ESP_ERR_HTTP_EAGAIN; }
    if (mode==2) return -ESP_ERR_HTTP_EAGAIN;
    return int64_t(strlen(payload));
}
static int esp_http_client_get_status_code(void *) { return status; }
static int esp_http_client_read(void *,char *out,int n) {
    if (mode==4 && offset) return -1;
    if (offset>=int(strlen(payload))) { complete=true; return 0; }
    if (n>3) n=3;
    if (n>int(strlen(payload))-offset) n=int(strlen(payload))-offset;
    memcpy(out,payload+offset,n); offset+=n; return n;
}
static bool esp_http_client_is_complete_data_received(void *) { return complete; }
static int esp_http_client_get_and_clear_last_tls_error(void *,int *,int *) {
    return mode==3 ? ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME : 0;
}
static int esp_http_client_get_errno(void *) { return 0; }
static int esp_http_client_cleanup(void *) { ++cleanups; return 0; }
static void *heap_caps_realloc(void *p,size_t n,int) { return realloc(p,n); }
struct cJSON {};
static cJSON object;
static cJSON *cJSON_Parse(const char *) { return &object; }
static cJSON *cJSON_GetObjectItem(cJSON *p,const char *) { return p; }
static const char *cJSON_GetStringValue(cJSON *) { return error_code; }
static void cJSON_Delete(cJSON *) {}
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(tag,format,...) snprintf(log_line,sizeof(log_line),format,__VA_ARGS__)
'''
        harness += fetch + r'''
static uint8_t *run(int scenario,int http,const char *code,size_t *size) {
    mode=scenario; status=http; error_code=code; calls=cleanups=offset=0;
    now=0; generation=1; complete=false; payload="test audio"; log_line[0]=0;
    auto *data=fetch("https://test.invalid/","test",16384,1,size,"synthesis");
    assert(cleanups==1);
    return data;
}
int main() {
    size_t size=0;
    assert(!run(1,200,nullptr,&size)); assert(now<1000000 && strstr(log_line,"category=cancelled"));
    assert(!run(2,200,nullptr,&size)); assert(now>=90000000 && now<91000000 && strstr(log_line,"category=timeout"));
    assert(!run(3,0,nullptr,&size)); assert(strstr(log_line,"category=dns"));
    assert(!run(0,401,nullptr,&size)); assert(strstr(log_line,"category=auth"));
    assert(!run(0,403,"AllocationQuota.FreeTierOnly",&size)); assert(strstr(log_line,"category=quota"));
    assert(!run(0,403,"Arrearage",&size)); assert(strstr(log_line,"category=quota"));
    assert(!run(0,403,"unknown",&size)); assert(strstr(log_line,"category=forbidden"));
    assert(!run(0,429,nullptr,&size)); assert(strstr(log_line,"category=rate_limit"));
    assert(!run(0,504,nullptr,&size)); assert(strstr(log_line,"category=timeout"));
    assert(!run(4,200,nullptr,&size)); assert(strstr(log_line,"category=transport"));
    auto *data=run(0,200,nullptr,&size);
    assert(data && size==strlen(payload) && !memcmp(data,payload,size)); free(data);
    assert(!strcmp(muse_qwen_service_error(0,"invalid_api_key"),"auth"));
    assert(!strcmp(muse_qwen_service_error(0,"Throttling.RateQuota"),"rate_limit"));
    return 0;
}
'''
        directory = Path(tempfile.mkdtemp(prefix='qwen-transport-test-', dir=ROOT / 'tmp/easyinput'))
        src, exe = directory / 'test.cpp', directory / 'test.exe'
        src.write_text(harness, encoding='utf-8')
        subprocess.run(cxx + ['-std=c++11', '-Wall', '-Werror', '-Wno-unused-function',
                             '-I', str(ROOT / 'components/muse'), str(src), '-o', str(exe)],
                       check=True, capture_output=True, text=True)
        subprocess.run([str(exe)], check=True, timeout=10, capture_output=True)

    def test_only_worker_owns_http_cleanup(self):
        source = (ROOT / 'components/muse/muse_qwen_tts.cpp').read_text(encoding='utf-8')
        cancel = source[source.index('void muse_qwen_tts_cancel(void)'):]
        self.assertNotIn('esp_http_client_', cancel)
        self.assertNotIn('vTaskDelete', cancel)
        self.assertIn('s_generation.store(0)', cancel)
        self.assertIn('if (s_status.load() != 1)', cancel)
        self.assertNotIn('esp_http_client_cancel_request', source)
        self.assertIn('cfg.is_async = true', source)
        self.assertIn('90000000', source)


if __name__ == '__main__':
    unittest.main()
