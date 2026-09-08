#include "lowpower.h"
#include "schedule.h"
#include "http.h"
#include "panel.h"
#include "ota.h"
#include "esp_attr.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG="neoframe-sleep";
/* RTC RAM survives timer sleep, not power loss. NVS owns the actual settings. */
static RTC_DATA_ATTR nf_validator settings_validator;
static RTC_DATA_ATTR char settings_url[512];
static RTC_DATA_ATTR time_t last_render;
void nf_lowpower_cycle(nf_config *c) {
    bool timer=esp_sleep_get_wakeup_cause()==ESP_SLEEP_WAKEUP_TIMER;
    if (!timer) { memset(&settings_validator,0,sizeof(settings_validator)); *settings_url=0; last_render=0; }
    /* Bound unavailable-network wake time; retry on the next scheduled wake. */
    esp_netif_ip_info_t ip;
    esp_netif_t *sta=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    bool connected=false;
    for (unsigned i=0;i<30;i++) {
        if (sta && esp_netif_get_ip_info(sta,&ip)==ESP_OK && ip.ip.addr) { connected=true; break; }
        if (i%10==0) esp_wifi_connect();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (connected) {
        nf_ota_confirm_healthy();
        for (unsigned i=0; time(NULL)<1700000000 && i<20;i++) vTaskDelay(pdMS_TO_TICKS(1000));
        if (strcmp(settings_url,c->config_url)) { memset(&settings_validator,0,sizeof(settings_validator)); strcpy(settings_url,c->config_url); }
        if (*c->config_url) {
            char *body=calloc(1,4097);
            nf_config *next=malloc(sizeof(*next));
            if (body && next) {
                nf_validator got; size_t n; int status;
                esp_err_t e=nf_fetch(c->config_url,&settings_validator,(uint8_t *)body,4096,&n,&status,&got,4000);
                if (e==ESP_OK && status==304) ESP_LOGI(TAG,"Settings unchanged");
                else if (e==ESP_OK && status==200 && !memchr(body,0,n) && nf_config_parse(body,c,next,true)) {
                    if (!memcmp(next,c,sizeof(*c)) || nf_config_save(next)==ESP_OK) {
                        *c=*next; settings_validator=got;
                        ESP_LOGI(TAG,"Remote settings accepted");
                    } else ESP_LOGW(TAG,"Settings save failed; retaining previous settings");
                } else ESP_LOGW(TAG,"Settings unavailable or invalid; retaining saved settings");
            }
            free(body); free(next);
        }
    }
    setenv("TZ",c->timezone,1); tzset();
    if (strcmp(c->power_profile,"low_power")) return;
    time_t now=time(NULL);
    bool clock_ok=now>=1700000000;
    bool active=clock_ok && nf_schedule_now(c,now);
    if (connected && active && !c->paused && *c->image_url) {
        /* Cold starts retain the existing guard; timer wakes use retained wall time. */
        int wait=timer ? (last_render && now-last_render<NF_MIN_INTERVAL ? NF_MIN_INTERVAL-(now-last_render):0) : NF_MIN_INTERVAL-esp_timer_get_time()/1000000;
        if (wait>0) vTaskDelay(pdMS_TO_TICKS(wait*1000));
        now=time(NULL);
        if (nf_schedule_now(c,now)) {
            uint8_t *frame=heap_caps_malloc(NF_FRAME_SIZE,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
            if (frame) {
                nf_validator got; size_t n; int status;
                /* No RAM frame survives sleep: always request a full image. */
                esp_err_t e=nf_fetch(c->image_url,NULL,frame,NF_FRAME_SIZE,&n,&status,&got,60000);
                if (e==ESP_OK && status==200 && nf_pixels_valid(frame,n)) {
                    e=nf_panel_init();
                    if (e==ESP_OK) e=nf_panel_render(frame);
                    last_render=time(NULL);
                    ESP_LOGI(TAG,"Render result: %s",esp_err_to_name(e));
                } else ESP_LOGW(TAG,"Image rejected: %s, HTTP %d",esp_err_to_name(e),status);
                free(frame);
            }
        }
    }
    now=time(NULL);
    uint32_t delay=clock_ok ? nf_schedule_next(c,now) : c->update_interval_s;
    if (last_render && now-last_render<NF_MIN_INTERVAL && delay<NF_MIN_INTERVAL-(now-last_render)) delay=NF_MIN_INTERVAL-(now-last_render);
    /* Configure the verified supply GPIO even if no panel was initialized. */
    gpio_set_direction(GPIO_NUM_45,GPIO_MODE_OUTPUT); nf_panel_sleep();
    gpio_hold_en(GPIO_NUM_45); gpio_deep_sleep_hold_en();
    ESP_LOGI(TAG,"%s; deep sleeping %u seconds",c->paused?"Paused":active?"Cycle complete":"Outside active window or clock unavailable",(unsigned)delay);
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup((uint64_t)delay*1000000));
    esp_deep_sleep_start();
}
