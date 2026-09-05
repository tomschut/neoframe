#include "config.h"
#include "http.h"
#include "panel.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_sleep.h"
#include "lwip/apps/sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG="neoframe";
static EventGroupHandle_t wifi_events;
static QueueHandle_t serial_messages, settings_jobs, settings_results;
typedef struct { nf_config config; uint32_t generation; } settings_job;
typedef struct { uint32_t generation; char json[4097]; } settings_result;
static int64_t seconds(void) { return esp_timer_get_time()/1000000; }
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)data;
    if (base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) xEventGroupSetBits(wifi_events,1);
    if (base==WIFI_EVENT && id==WIFI_EVENT_STA_DISCONNECTED) xEventGroupClearBits(wifi_events,1);
}
static bool online(void) { return xEventGroupGetBits(wifi_events)&1; }
static void credentials(const nf_config *c) {
    esp_wifi_disconnect(); xEventGroupClearBits(wifi_events,1);
    wifi_config_t w={0};
    memcpy(w.sta.ssid,c->wifi_ssid,strlen(c->wifi_ssid));
    memcpy(w.sta.password,c->wifi_pass,strlen(c->wifi_pass));
    esp_err_t e=esp_wifi_set_config(WIFI_IF_STA,&w);
    if (e!=ESP_OK) ESP_LOGE(TAG,"WiFi configuration rejected: %s",esp_err_to_name(e));
}
static void serial_task(void *arg) {
    (void)arg;
    /* UART stdin is nonblocking. Never echo credentials; discard oversized lines. */
    char *line=malloc(4097); if (!line) { vTaskDelete(NULL); return; }
    size_t used=0; bool overflow=false;
    for (;;) {
        int ch=getchar();
        if (ch==EOF) { clearerr(stdin); vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        if (ch=='\r') continue;
        if (ch=='\n') {
            if (used && !overflow) {
                line[used]=0; char *copy=strdup(line);
                if (!copy || xQueueSend(serial_messages,&copy,0)!=pdTRUE) {
                    free(copy); ESP_LOGW(TAG,"Serial queue full; retry configuration");
                }
            } else if (overflow) ESP_LOGW(TAG,"Serial configuration exceeds 4096 bytes");
            used=0; overflow=false;
        } else if (used<4096) line[used++]=(char)ch;
        else overflow=true;
    }
}
static void settings_task(void *arg) {
    (void)arg;
    settings_job job;
    for (;;) {
        xQueueReceive(settings_jobs,&job,portMAX_DELAY);
        if (!*job.config.config_url || !online()) continue;
        settings_result *r=calloc(1,sizeof(*r));
        if (!r) { ESP_LOGW(TAG,"Settings allocation failed; ignored"); continue; }
        r->generation=job.generation;
        nf_validator v; size_t n; int status;
        esp_err_t e=nf_fetch(job.config.config_url,NULL,(uint8_t *)r->json,4096,&n,&status,&v,4000);
        if (e!=ESP_OK || status!=200 || memchr(r->json,0,n)) {
            ESP_LOGW(TAG,"Settings fetch ignored: %s, HTTP %d",esp_err_to_name(e),status); free(r); continue;
        }
        r->json[n]=0;
        if (xQueueSend(settings_results,&r,0)!=pdTRUE) free(r);
    }
}
static bool apply(nf_config *c, const char *json, bool remote, uint32_t *generation) {
    nf_config next;
    if (!nf_config_parse(json,c,&next,remote)) {
        ESP_LOGW(TAG,"Invalid %s configuration; previous configuration retained",remote?"remote":"serial"); return false;
    }
    if (!memcmp(c,&next,sizeof(next))) {
        if (!remote) ESP_LOGI(TAG,"Configuration already saved");
        return true;
    }
    esp_err_t e=nf_config_save(&next);
    if (e!=ESP_OK) { ESP_LOGE(TAG,"Configuration not saved: %s",esp_err_to_name(e)); return false; }
    bool changed=strcmp(c->wifi_ssid,next.wifi_ssid)||strcmp(c->wifi_pass,next.wifi_pass);
    *c=next; ++*generation;
    if (changed) credentials(c);
    ESP_LOGI(TAG,"Configuration saved (version 1); P0 uses always-on polling");
    return true;
}
void app_main(void) {
    ESP_LOGI(TAG,"Wake reason %d",esp_sleep_get_wakeup_cause());
    /* Never erase NVS as an automatic recovery strategy: credentials matter. */
    esp_err_t e=nvs_flash_init();
    if (e!=ESP_OK) { ESP_LOGE(TAG,"NVS unavailable: %s; retained without erasing",esp_err_to_name(e)); return; }
    nf_config c; e=nf_config_load(&c);
    if (e!=ESP_OK) ESP_LOGW(TAG,"No valid configuration (%s); send serial JSON",esp_err_to_name(e));
    wifi_events=xEventGroupCreate();
    serial_messages=xQueueCreate(2,sizeof(char *));
    settings_jobs=xQueueCreate(1,sizeof(settings_job));
    settings_results=xQueueCreate(1,sizeof(settings_result *));
    if (!wifi_events || !serial_messages || !settings_jobs || !settings_results) {
        ESP_LOGE(TAG,"Cannot allocate task queues"); return;
    }
    ESP_ERROR_CHECK(esp_netif_init()); ESP_ERROR_CHECK(esp_event_loop_create_default());
    if (!esp_netif_create_default_wifi_sta()) { ESP_LOGE(TAG,"Cannot create STA interface"); return; }
    wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT(); ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,wifi_event,NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA)); ESP_ERROR_CHECK(esp_wifi_start());
    credentials(&c);
    sntp_setoperatingmode(SNTP_OPMODE_POLL); sntp_setservername(0,"pool.ntp.org"); sntp_init();
    if (xTaskCreate(serial_task,"serial_config",4096,NULL,2,NULL)!=pdPASS)
        ESP_LOGE(TAG,"Serial configuration task unavailable");
    if (xTaskCreate(settings_task,"settings",8192,NULL,1,NULL)!=pdPASS)
        ESP_LOGW(TAG,"Settings task unavailable; image loop continues");
    ESP_LOGI(TAG,"Send a JSON line with wifi_ssid, wifi_pass, image_url and optional config_url at 115200 baud");
    uint8_t *candidate=heap_caps_malloc(NF_FRAME_SIZE,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    uint8_t *cached=heap_caps_malloc(NF_FRAME_SIZE,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if (!candidate || !cached) ESP_LOGE(TAG,"PSRAM frame allocation failed; serial configuration remains available");
    bool panel_ready=false, have_frame=false;
    nf_validator validator={0}; char validator_url[512]={0};
    uint32_t generation=0, backoff=1;
    int64_t connect_at=0, next_poll=0, last_refresh=0;
    /* A full boot cooldown also protects against rapid power cycling. */
    int64_t render_after=seconds()+NF_MIN_INTERVAL;
    for (;;) {
        char *line;
        while (xQueueReceive(serial_messages,&line,0)==pdTRUE) {
            if (apply(&c,line,false,&generation)) { next_poll=0; connect_at=0; backoff=1; }
            free(line);
        }
        int64_t now=seconds();
        if (online()) backoff=1;
        else if (*c.wifi_ssid && now>=connect_at) {
            esp_wifi_connect(); connect_at=now+backoff;
            if (backoff<60) backoff=backoff*2>60?60:backoff*2;
        }
        if (now>=next_poll && now>=render_after && candidate && cached) {
            settings_result *r;
            while (xQueueReceive(settings_results,&r,0)==pdTRUE) {
                if (r->generation==generation) apply(&c,r->json,true,&generation);
                free(r);
            }
            if (strcmp(validator_url,c.image_url)) {
                memset(&validator,0,sizeof(validator)); strcpy(validator_url,c.image_url);
            }
            bool due=have_frame && now-last_refresh>=NF_DAY-60;
            bool render=false;
            if (online() && *c.image_url) {
                size_t n=0; int status=0; nf_validator received;
                /* TLS certificate dates require SNTP; HTTP can run before sync. */
                if (!strncmp(c.image_url,"https://",8) && time(NULL)<1700000000) e=ESP_ERR_INVALID_STATE;
                else e=nf_fetch(c.image_url,due?NULL:&validator,candidate,NF_FRAME_SIZE,&n,&status,&received,60000);
                if (e==ESP_OK && status==200 && nf_pixels_valid(candidate,n)) render=true;
                else if (e==ESP_OK && status==304) ESP_LOGI(TAG,"Image unchanged");
                else ESP_LOGW(TAG,"Image fetch rejected: %s, HTTP %d, bytes %u",esp_err_to_name(e),status,(unsigned)n);
                if (render) {
                    if (!panel_ready) panel_ready=nf_panel_init()==ESP_OK;
                    e=panel_ready?nf_panel_render(candidate):ESP_ERR_INVALID_STATE;
                    render_after=seconds()+NF_MIN_INTERVAL;
                    if (e==ESP_OK) {
                        uint8_t *swap=cached; cached=candidate; candidate=swap;
                        validator=received; have_frame=true; last_refresh=seconds(); due=false;
                        ESP_LOGI(TAG,"Frame rendered successfully");
                    } else ESP_LOGE(TAG,"Panel failed: %s; validator retained",esp_err_to_name(e));
                }
            }
            if (due && !render && panel_ready) {
                e=nf_panel_render(cached); render_after=seconds()+NF_MIN_INTERVAL;
                if (e==ESP_OK) last_refresh=seconds();
                else ESP_LOGE(TAG,"Daily cached refresh failed: %s",esp_err_to_name(e));
            }
            settings_job job={.config=c,.generation=generation};
            if (*c.config_url) xQueueOverwrite(settings_jobs,&job);
            next_poll=seconds()+c.update_interval_s;
            if (have_frame && next_poll>last_refresh+NF_DAY-60) next_poll=last_refresh+NF_DAY-60;
            if (next_poll<render_after) next_poll=render_after;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
