#include "http.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include <string.h>
#include <strings.h>
typedef struct { nf_validator *v; bool encoded; } headers;
static esp_err_t event(esp_http_client_event_t *e) {
    headers *h=e->user_data;
    if (e->event_id != HTTP_EVENT_ON_HEADER) return ESP_OK;
    if (!strcasecmp(e->header_key,"Content-Encoding") && strcasecmp(e->header_value,"identity")) h->encoded=true;
    char *dst=NULL; size_t cap=0;
    if (!strcasecmp(e->header_key,"ETag")) { dst=h->v->etag; cap=sizeof(h->v->etag); }
    if (!strcasecmp(e->header_key,"Last-Modified")) { dst=h->v->modified; cap=sizeof(h->v->modified); }
    if (dst) {
        size_t n=strlen(e->header_value);
        if (n<cap && !strchr(e->header_value,'\r') && !strchr(e->header_value,'\n')) memcpy(dst,e->header_value,n+1);
    }
    return ESP_OK;
}
esp_err_t nf_fetch(const char *url, const nf_validator *conditional,
    uint8_t *body, size_t capacity, size_t *length, int *status,
    nf_validator *received, int budget_ms) {
    memset(received,0,sizeof(*received)); *length=0; *status=0;
    headers h={.v=received};
    esp_http_client_config_t c={.url=url, .timeout_ms=2000, .event_handler=event,
        .user_data=&h, .crt_bundle_attach=esp_crt_bundle_attach, .disable_auto_redirect=true};
    esp_http_client_handle_t client=esp_http_client_init(&c);
    if (!client) return ESP_ERR_NO_MEM;
    int64_t deadline=esp_timer_get_time()+(int64_t)budget_ms*1000;
    esp_http_client_set_header(client,"Accept-Encoding","identity");
    if (conditional) {
        if (*conditional->etag) esp_http_client_set_header(client,"If-None-Match",conditional->etag);
        else if (*conditional->modified) esp_http_client_set_header(client,"If-Modified-Since",conditional->modified);
    }
    esp_err_t result=esp_http_client_open(client,0);
    if (result != ESP_OK) goto done;
    int64_t announced=esp_http_client_fetch_headers(client);
    if (announced<0) { result=ESP_FAIL; goto done; }
    *status=esp_http_client_get_status_code(client);
    if (*status==304 && conditional && (*conditional->etag || *conditional->modified)) goto done;
    if (*status!=200 || h.encoded || announced>(int64_t)capacity) { result=ESP_ERR_INVALID_RESPONSE; goto done; }
    while (!esp_http_client_is_complete_data_received(client)) {
        if (esp_timer_get_time()>=deadline) { result=ESP_ERR_TIMEOUT; goto done; }
        uint8_t extra;
        size_t available=capacity-*length;
        int n=esp_http_client_read(client, available ? (char *)body+*length : (char *)&extra,
            available ? (available>4096 ? 4096 : available) : 1);
        if (n<0) { result=ESP_FAIL; goto done; }
        if (n==0) {
            if (!esp_http_client_is_complete_data_received(client)) result=ESP_ERR_INVALID_SIZE;
            break;
        }
        if (!available) { result=ESP_ERR_INVALID_SIZE; goto done; }
        *length+=(size_t)n;
    }
    if (announced>0 && (size_t)announced!=*length) result=ESP_ERR_INVALID_SIZE;
done:
    esp_http_client_close(client); esp_http_client_cleanup(client);
    return result;
}
