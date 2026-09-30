#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "schedule.h"
#include "nvs.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static nf_config stored, pending;
static int exists, fail_commit;
static size_t stored_size=sizeof(nf_config);
esp_err_t nvs_open(const char *ns,int mode,nvs_handle_t *h) {
    assert(!strcmp(ns,"neoframe")); (void)mode; *h=1; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *n) {
    (void)h; assert(!strcmp(key,"config"));
    if (!exists) return ESP_FAIL;
    assert(*n>=stored_size); memcpy(out,&stored,stored_size); *n=stored_size; return ESP_OK;
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
    /* Remote OTA requires URL and target version together; local manual OTA needs only a URL. */
    const char *remote_with_firmware="{\"update_interval_s\":600,\"image_url\":\"http://host/frame\","
        "\"active_start\":\"08:00\",\"active_end\":\"00:00\",\"led_enabled\":true,"
        "\"firmware_url\":\"https://host/fw.bin\"}";
    assert(!nf_config_parse(remote_with_firmware,&base,&out,true));
    const char *remote_ota="{\"update_interval_s\":600,\"image_url\":\"http://host/frame\",\"active_start\":\"08:00\",\"active_end\":\"22:00\",\"led_enabled\":false,\"firmware_version\":\"test-v2\",\"firmware_url\":\"http://host/fw.bin\"}";
    assert(nf_config_parse(remote_ota,&base,&out,true));
    assert(!strcmp(out.firmware_version,"test-v2"));
    assert(nf_config_parse(good,&out,&reloaded,true) && !*reloaded.firmware_version);
    assert(!nf_config_parse("{\"firmware_version\":\"test\"}",&base,&out,false));
    stored=out; stored.version=2; stored_size=offsetof(nf_config,firmware_version); exists=1;
    assert(nf_config_load(&reloaded)==ESP_OK && reloaded.version==3 && !*reloaded.firmware_version);
    stored_size=sizeof(stored);

    assert(nf_config_parse("{\"firmware_url\":\"https://host/fw.bin\"}",&base,&out,false));
    assert(!strcmp(out.firmware_url,"https://host/fw.bin"));
    assert(nf_config_parse("{\"firmware_url\":\"http://host/fw.bin\"}",&base,&out,false));
    assert(!strcmp(out.firmware_url,"http://host/fw.bin"));
    assert(nf_config_save(&base)==ESP_OK);
    assert(nf_config_load(&reloaded)==ESP_OK && !memcmp(&base,&reloaded,sizeof(base)));
    fail_commit=1; out=base; strcpy(out.wifi_ssid,"replacement");
    assert(nf_config_save(&out)!=ESP_OK);
    assert(nf_config_load(&reloaded)==ESP_OK && !strcmp(reloaded.wifi_ssid,"My WiFi"));
    out=base;
    assert(nf_config_reset_wifi(&out)!=ESP_OK);
    assert(!memcmp(&out,&base,sizeof(out)));
    assert(nf_config_load(&reloaded)==ESP_OK && !memcmp(&reloaded,&base,sizeof(base)));
    fail_commit=0;
    assert(nf_config_reset_wifi(&out)==ESP_OK);
    nf_config reset_expected=base;
    memset(reset_expected.wifi_ssid,0,sizeof(reset_expected.wifi_ssid));
    memset(reset_expected.wifi_pass,0,sizeof(reset_expected.wifi_pass));
    assert(!memcmp(&out,&reset_expected,sizeof(out)));
    assert(nf_config_load(&reloaded)==ESP_OK && !memcmp(&reloaded,&reset_expected,sizeof(reloaded)));
    stored.version=9; assert(nf_config_load(&reloaded)!=ESP_OK && reloaded.version==3);
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
    assert(nf_config_parse("{\"paused\":true,\"timezone\":\"UTC0\"}",&base,&out,false));
    assert(out.paused && !strcmp(out.timezone,"UTC0"));
    assert(!nf_config_parse("{\"paused\":1}",&base,&out,false));
    assert(nf_schedule_active(8*3600,8*3600,22*3600));
    assert(!nf_schedule_active(22*3600,8*3600,22*3600));
    assert(nf_schedule_active(3600,22*3600,8*3600));
    assert(nf_schedule_delay(8*3600,8*3600,22*3600,300)==300);
    assert(nf_schedule_delay(7*3600,8*3600,22*3600,300)==3600);
    assert(nf_schedule_delay(22*3600-1,8*3600,22*3600,300)==10*3600+1);
    assert(nf_config_parse("{\"schedule\":[{\"days\":\"mon-fri\",\"start\":\"08:00\",\"stop\":\"22:00\",\"every\":\"5m\"}]}",&base,&out,false));
    setenv("TZ","UTC0",1); tzset();
    struct tm monday={.tm_year=126,.tm_mon=8,.tm_mday=7,.tm_hour=8};
    time_t epoch=mktime(&monday);
    assert(nf_schedule_now(&out,epoch));
    assert(nf_schedule_next(&out,epoch)==300);
    assert(!nf_schedule_now(&out,epoch-3600));
    assert(nf_schedule_next(&out,epoch-3600)==3600);
    assert(!nf_config_parse("{\"schedule\":[{\"days\":\"invalid\",\"start\":\"08:00\",\"stop\":\"22:00\",\"every\":\"5m\"}]}",&base,&out,false));
    const char *schema="{\"timezone\":\"Europe/Amsterdam\",\"power_profile\":\"low_power\","
        "\"paused\":true,\"image_url\":\"http://host/frame\",\"schedule\":["
        "{\"days\":\"fri\",\"start\":\"22:00\",\"stop\":\"02:00\",\"every\":\"5m\"}]}";
    assert(nf_config_parse(schema,&base,&out,true) && out.paused);
    assert(!strcmp(out.timezone,"CET-1CEST,M3.5.0,M10.5.0/3"));
    struct tm saturday={.tm_year=126,.tm_mon=8,.tm_mday=12,.tm_hour=1};
    epoch=mktime(&saturday);
    assert(nf_schedule_now(&out,epoch));
    assert(!nf_schedule_now(&out,epoch+3600));
    assert(nf_schedule_next(&out,epoch+3600)==6*86400+20*3600);
    assert(!nf_config_parse("{\"schedule\":[]}",&base,&out,false));
    assert(!nf_config_parse("{\"timezone\":\"Europe/NotReal\"}",&base,&out,false));
    assert(nf_config_parse("{\"power_profile\":\"ac_power\"}",&base,&out,false));
    assert(!strcmp(out.power_profile,"ac_power"));
    assert(!nf_config_parse("{\"power_profile\":\"turbo\"}",&base,&out,false));
    assert(nf_config_parse("{\"schedule\":[{\"days\":\"daily\",\"start\":\"00:00\",\"stop\":\"00:00\",\"every\":\"5m\"}]}",&base,&out,false));
    setenv("TZ","CET-1CEST,M3.5.0,M10.5.0/3",1); tzset();
    struct tm spring={.tm_year=126,.tm_mon=2,.tm_mday=29,.tm_hour=1,.tm_min=59,.tm_isdst=-1};
    epoch=mktime(&spring);
    assert(nf_schedule_next(&out,epoch)==60);
    struct tm autumn={.tm_year=126,.tm_mon=9,.tm_mday=25,.tm_hour=2,.tm_min=59,.tm_isdst=1};
    epoch=mktime(&autumn);
    assert(nf_schedule_next(&out,epoch)==60);
    /* Real v1 prefix length, including its padding, migrates without erasure. */
    stored=base; stored.version=1; strcpy(stored.power_profile,"low_power");
    stored_size=(offsetof(nf_config,timezone)+3)&~(size_t)3;
    assert(nf_config_load(&reloaded)==ESP_OK);
    assert(!strcmp(reloaded.wifi_ssid,base.wifi_ssid));
    assert(!strcmp(reloaded.power_profile,"always_on") && !reloaded.paused);
    stored_size=sizeof(nf_config);
    /* Cold boot and software reset wait for connectivity, then allow five
     * full minutes of settings access. Timer wakes never get this delay. */
    assert(nf_boot_settings_pending(false,-1,1000));
    assert(nf_boot_settings_pending(false,1000,1000));
    assert(nf_boot_settings_pending(false,1000,1299));
    assert(!nf_boot_settings_pending(false,1000,1300));
    assert(!nf_boot_settings_pending(false,1000,1301));
    assert(!nf_boot_settings_pending(true,-1,0));
    assert(!nf_boot_settings_pending(true,1000,1001));
    puts("PASS: configuration, NVS/reset failure, pixels, scheduling and boot settings window");
}
