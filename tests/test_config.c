#include "config.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static nf_config stored, pending;
static int exists, fail_commit;
esp_err_t nvs_open(const char *ns,int mode,nvs_handle_t *h) {
    assert(!strcmp(ns,"neoframe")); (void)mode; *h=1; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *n) {
    (void)h; assert(!strcmp(key,"config"));
    if (!exists) return ESP_FAIL;
    assert(*n>=sizeof(stored)); memcpy(out,&stored,sizeof(stored)); *n=sizeof(stored); return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *in,size_t n) {
    (void)h; assert(!strcmp(key,"config") && n==sizeof(pending)); memcpy(&pending,in,n); return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    (void)h; if (fail_commit) return ESP_FAIL; stored=pending; exists=1; return ESP_OK;
}
void nvs_close(nvs_handle_t h) { (void)h; }
int main(void) {
    nf_config base,out,reloaded;
    nf_config_default(&base); assert(nf_config_valid(&base));
    assert(nf_config_load(&out)!=ESP_OK && !strcmp(out.active_start,"08:00"));
    const char *good="{\"update_interval_s\":180,\"image_url\":\"http://host/frame\",\"active_start\":\"22:00\",\"active_end\":\"08:00\",\"led_enabled\":false}";
    assert(nf_config_parse(good,&base,&out,true));
    assert(out.update_interval_s==180 && !out.led_enabled);
    const char *bad[]={"{", "[]", "null", "{}", "{\"image_url\":\"http://host/frame\"}",
        "{\"update_interval_s\":179}", "{\"update_interval_s\":180.5}", "{\"update_interval_s\":86401}",
        "{\"update_interval_s\":1e999}", "{\"update_interval_s\":\"600\"}",
        "{\"image_url\":\"file:///etc/passwd\"}", "{\"active_start\":\"24:00\"}",
        "{\"active_end\":\"12:60\"}", "{\"led_enabled\":1}", "{\"wifi_pass\":\"short\"}",
        "{\"unknown\":true}", "{\"image_url\":\"http://host\",\"image_url\":\"https://other\"}",
        "{\"image_url\":\"http://host\\u0000/hidden\"}", "{\"led_enabled\":true}garbage"};
    for (size_t i=0;i<sizeof(bad)/sizeof(*bad);++i) {
        nf_config snapshot=out;
        assert(!nf_config_parse(bad[i],&base,&out,true));
        assert(!memcmp(&out,&snapshot,sizeof(out)));
        if (i>4) assert(!nf_config_parse(bad[i],&base,&out,false));
    }
    assert(nf_config_parse("{\"wifi_ssid\":\"My WiFi\",\"wifi_pass\":\"abcdefgh\",\"config_url\":\"http://host/settings\"}",&out,&base,false));
    assert(nf_config_save(&base)==ESP_OK);
    assert(nf_config_load(&reloaded)==ESP_OK && !memcmp(&base,&reloaded,sizeof(base)));
    fail_commit=1; out=base; strcpy(out.wifi_ssid,"replacement");
    assert(nf_config_save(&out)!=ESP_OK);
    assert(nf_config_load(&reloaded)==ESP_OK && !strcmp(reloaded.wifi_ssid,"My WiFi"));
    stored.version=9; assert(nf_config_load(&reloaded)!=ESP_OK && reloaded.version==1);
    stored=base; memset(stored.wifi_ssid,'x',sizeof(stored.wifi_ssid));
    assert(nf_config_load(&reloaded)!=ESP_OK);
    assert(!nf_url_valid("http:///",false)); assert(!nf_url_valid("http://user@host/",false));
    assert(!nf_url_valid("http://host/\r\nInjected: yes",false));
    uint8_t *frame=malloc(NF_FRAME_SIZE); assert(frame);
    memset(frame,0x16,NF_FRAME_SIZE); assert(nf_pixels_valid(frame,NF_FRAME_SIZE));
    assert(!nf_pixels_valid(frame,NF_FRAME_SIZE-1));
    frame[0]=0x40; assert(!nf_pixels_valid(frame,NF_FRAME_SIZE));
    frame[0]=0x17; assert(!nf_pixels_valid(frame,NF_FRAME_SIZE));
    for (unsigned y=0;y<NF_ROWS;++y) {
        assert(nf_row_offset(0,y)==y*600); assert(nf_row_offset(1,y)==y*600+300);
    }
    assert(nf_row_offset(2,0)==SIZE_MAX); free(frame);
    puts("PASS: configuration validation, atomic rejection, NVS roundtrip/failure, pixels, dual-CS geometry");
}
