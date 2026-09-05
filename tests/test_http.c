#include "http.h"
#include "esp_http_client.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static esp_http_client_config_t config;
static int response, body_size, announced, pos, read_error, opened, closed, cleaned, encoded;
static int64_t now, read_delay;
static char sent_key[64], sent_value[256];
esp_err_t esp_crt_bundle_attach(void *p) { (void)p; return ESP_OK; }
int64_t esp_timer_get_time(void) { return now; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c) {
    config=*c; assert(c->disable_auto_redirect && c->crt_bundle_attach); return &config;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t h,const char *k,const char *v) {
    (void)h; strcpy(sent_key,k); strcpy(sent_value,v); return ESP_OK;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t h,int n) { (void)h;(void)n;return opened; }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t h) {
    (void)h;
    esp_http_client_event_t e={.event_id=HTTP_EVENT_ON_HEADER,.user_data=config.user_data,
        .header_key="ETag",.header_value="\"version1\""};
    config.event_handler(&e);
    if (encoded) { e.header_key="Content-Encoding"; e.header_value="gzip"; config.event_handler(&e); }
    return announced;
}
int esp_http_client_get_status_code(esp_http_client_handle_t h) { (void)h;return response; }
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t h) {
    (void)h; return pos==body_size && (announced==0 || pos==announced);
}
int esp_http_client_read(esp_http_client_handle_t h,char *out,int n) {
    (void)h; now+=read_delay;
    if (read_error) return -1;
    if (n>body_size-pos) n=body_size-pos;
    if (n>7) n=7; /* arbitrary TCP fragmentation */
    memset(out,0x11,n); pos+=n; return n;
}
esp_err_t esp_http_client_close(esp_http_client_handle_t h) { (void)h;closed++;return ESP_OK; }
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t h) { (void)h;cleaned++;return ESP_OK; }
static void reset(int status,int actual,int content_length) {
    response=status; body_size=actual; announced=content_length; pos=0;
    read_error=0; opened=0; closed=0; cleaned=0; encoded=0; now=0; read_delay=0;
    *sent_key=0; *sent_value=0;
}
static esp_err_t fetch(const nf_validator *v,size_t *n) {
    uint8_t buffer[32]; nf_validator got; int status;
    esp_err_t e=nf_fetch("http://example/frame",v,buffer,sizeof(buffer),n,&status,&got,100);
    assert(closed==1 && cleaned==1); return e;
}
int main(void) {
    size_t n; nf_validator v={.etag="\"old\"",.modified="yesterday"};
    reset(200,32,32); assert(fetch(NULL,&n)==ESP_OK && n==32);
    reset(200,32,0); assert(fetch(NULL,&n)==ESP_OK && n==32); /* chunked */
    reset(200,33,0); assert(fetch(NULL,&n)!=ESP_OK); /* oversized chunked */
    reset(200,33,33); assert(fetch(NULL,&n)!=ESP_OK && pos==0);
    reset(200,16,32); assert(fetch(NULL,&n)!=ESP_OK); /* truncated */
    reset(200,32,32); read_error=1; assert(fetch(NULL,&n)!=ESP_OK);
    reset(200,32,32); read_delay=110000; assert(fetch(NULL,&n)==ESP_ERR_TIMEOUT);
    reset(200,32,32); encoded=1; assert(fetch(NULL,&n)!=ESP_OK);
    reset(200,32,32); opened=ESP_ERR_TIMEOUT; assert(fetch(NULL,&n)==ESP_ERR_TIMEOUT);
    const int errors[]={301,302,404,500,503};
    for (unsigned i=0;i<sizeof(errors)/sizeof(*errors);++i) {
        reset(errors[i],32,32); assert(fetch(NULL,&n)!=ESP_OK && pos==0);
    }
    reset(304,0,0); assert(fetch(NULL,&n)!=ESP_OK);
    reset(304,0,0); assert(fetch(&v,&n)==ESP_OK && n==0);
    assert(!strcmp(sent_key,"If-None-Match") && !strcmp(sent_value,"\"old\""));
    *v.etag=0; reset(304,0,0); assert(fetch(&v,&n)==ESP_OK);
    assert(!strcmp(sent_key,"If-Modified-Since"));
    *v.modified=0; reset(304,0,0); assert(fetch(&v,&n)!=ESP_OK);
    puts("PASS: HTTP fragmentation/chunking, bounds, truncation, timeout, errors, validators, cleanup");
}
