#include "config.h"
#include "cJSON.h"
#include "nvs.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>
static int weekday(const char *s) {
    const char *names[]={"mon","tue","wed","thu","fri","sat","sun"};
    for (int i=0;i<7;i++) if (!strncmp(s,names[i],3)) return i;
    return -1;
}
static unsigned day_mask(const char *s) {
    if (!strcmp(s,"daily")) return 127;
    size_t n=strlen(s);
    if (n!=3 && n!=7) return 0;
    int first=weekday(s), last=n==3?first:weekday(s+4);
    if (first<0 || last<0 || (n==7 && s[3]!='-')) return 0;
    unsigned mask=0;
    for (int d=first;;d=(d+1)%7) { mask|=1U<<d; if (d==last) return mask; }
}
static unsigned duration(const char *s) {
    if (*s<'0' || *s>'9' || strlen(s)>5) return 0;
    char *end; unsigned long n=strtoul(s,&end,10);
    if (!end[0] || end[1] || (*end!='m' && *end!='h')) return 0;
    n*=*end=='m'?60:3600;
    return n>=180 && n<=86400 ? n:0;
}
esp_err_t nf_config_reset_wifi(nf_config *c) {
    nf_config next=*c;
    memset(next.wifi_ssid,0,sizeof(next.wifi_ssid));
    memset(next.wifi_pass,0,sizeof(next.wifi_pass));
    esp_err_t e=nf_config_save(&next);
    if (e==ESP_OK) *c=next;
    return e;
}
void nf_config_default(nf_config *c) {
    memset(c, 0, sizeof(*c));
    c->version = 3; c->update_interval_s = 600; c->led_enabled = 1;
    strcpy(c->active_start, "08:00"); strcpy(c->active_end, "00:00");
    strcpy(c->power_profile, "always_on");
    strcpy(c->timezone, "CET-1CEST,M3.5.0,M10.5.0/3");
}
bool nf_config_valid(const nf_config *c) {
#define TERMINATED(f) if (!memchr(c->f, 0, sizeof(c->f))) return false
    TERMINATED(wifi_ssid); TERMINATED(wifi_pass); TERMINATED(image_url); TERMINATED(config_url);
    TERMINATED(firmware_version); TERMINATED(timezone); TERMINATED(firmware_url); TERMINATED(active_start); TERMINATED(active_end); TERMINATED(power_profile);
#undef TERMINATED
    if (c->window_count>NF_MAX_WINDOWS) return false;
    for (unsigned i=0;i<c->window_count;i++) {
        const nf_window *w=&c->windows[i];
        if (!w->days || w->days>127 || !memchr(w->start,0,6) || !memchr(w->stop,0,6) ||
            !nf_time_valid(w->start) || !nf_time_valid(w->stop) || w->interval_s<180 ||
            w->interval_s>86400 || w->interval_s%60) return false;
    }
    size_t n = strlen(c->wifi_pass);
    if (n && n < 8) return false;
    if (n == 64) for (size_t i = 0; i < n; ++i)
        if (!strchr("0123456789abcdefABCDEF", c->wifi_pass[i])) return false;
    return c->version == 3 && c->paused <= 1 && *c->timezone && c->update_interval_s >= NF_MIN_INTERVAL && c->update_interval_s <= NF_DAY &&
        nf_url_valid(c->image_url, true) && nf_url_valid(c->config_url, true) &&
        nf_url_valid(c->firmware_url, true) && (!*c->firmware_version || *c->firmware_url) &&
        nf_time_valid(c->active_start) && nf_time_valid(c->active_end) && c->led_enabled <= 1 &&
        (!strcmp(c->power_profile, "low_power") || !strcmp(c->power_profile, "always_on") ||
            !strcmp(c->power_profile, "ac_power"));
}
bool nf_config_parse(const char *json, const nf_config *base, nf_config *out, bool remote) {
    if (!json || strstr(json,"\\u0000")) return false;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithOpts(json, &end, true);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return false; }
    nf_config next = *base;
    bool ok = true;
    /* Require the complete documented remote contract; never apply half a response. */
    const char *required[] = {"update_interval_s", "active_start", "active_end", "image_url", "led_enabled"};
    if (remote) {
        cJSON *v=cJSON_GetObjectItemCaseSensitive(root,"firmware_version");
        cJSON *u=cJSON_GetObjectItemCaseSensitive(root,"firmware_url");
        if (!!v != !!u) ok=false;
        memset(next.firmware_version,0,sizeof(next.firmware_version));
        if (cJSON_GetObjectItemCaseSensitive(root,"schedule")) {
            const char *required_schedule[]={"schedule","timezone","paused","image_url","power_profile"};
            for (unsigned i=0;i<5;i++) if (!cJSON_GetObjectItemCaseSensitive(root,required_schedule[i])) ok=false;
        } else for (size_t i=0;i<5;i++) if (!cJSON_GetObjectItemCaseSensitive(root,required[i])) ok=false;
    }
    cJSON *item;
    cJSON_ArrayForEach(item, root) {
        const char *k = item->string;
        for (cJSON *p = root->child; p != item; p = p->next)
            if (!strcmp(p->string, k)) ok = false;
#define STR_FIELD(f) if (!strcmp(k, #f)) { \
    if (!cJSON_IsString(item) || strlen(item->valuestring) >= sizeof(next.f)) ok = false; \
    else strcpy(next.f, item->valuestring); \
}
        if (!strcmp(k, "wifi_ssid") || !strcmp(k, "wifi_pass") || !strcmp(k, "config_url")) {
            if (remote) { ok = false; continue; }
            STR_FIELD(wifi_ssid) else STR_FIELD(wifi_pass) else STR_FIELD(config_url)
        } else if (!strcmp(k,"schedule")) {
            if (!cJSON_IsArray(item) || cJSON_GetArraySize(item)<1 || cJSON_GetArraySize(item)>NF_MAX_WINDOWS) { ok=false; continue; }
            next.window_count=0; memset(next.windows,0,sizeof(next.windows));
            cJSON *entry;
            cJSON_ArrayForEach(entry,item) {
                if (!cJSON_IsObject(entry) || cJSON_GetArraySize(entry)!=4) { ok=false; break; }
                cJSON *days=cJSON_GetObjectItemCaseSensitive(entry,"days");
                cJSON *start=cJSON_GetObjectItemCaseSensitive(entry,"start");
                cJSON *stop=cJSON_GetObjectItemCaseSensitive(entry,"stop");
                cJSON *every=cJSON_GetObjectItemCaseSensitive(entry,"every");
                if (!cJSON_IsString(days) || !cJSON_IsString(start) || !cJSON_IsString(stop) ||
                    !cJSON_IsString(every) || strlen(start->valuestring)!=5 || strlen(stop->valuestring)!=5) { ok=false; break; }
                nf_window *w=&next.windows[next.window_count++];
                strcpy(w->start,start->valuestring); strcpy(w->stop,stop->valuestring);
                w->days=day_mask(days->valuestring); w->interval_s=duration(every->valuestring);
            }
        } else STR_FIELD(firmware_url)
        else STR_FIELD(firmware_version)
        else STR_FIELD(image_url)
        else STR_FIELD(active_start)
        else STR_FIELD(active_end)
        else STR_FIELD(power_profile)
        else if (!strcmp(k,"timezone")) {
            if (!cJSON_IsString(item)) ok=false;
            else if (!strcmp(item->valuestring,"Europe/Amsterdam")) strcpy(next.timezone,"CET-1CEST,M3.5.0,M10.5.0/3");
            else if (!strcmp(item->valuestring,"UTC")) strcpy(next.timezone,"UTC0");
            else if (strchr(item->valuestring,'/') && !strchr(item->valuestring,',')) ok=false;
            else if (strlen(item->valuestring)>=sizeof(next.timezone)) ok=false;
            else strcpy(next.timezone,item->valuestring);
        }
        else if (!strcmp(k, "update_interval_s")) {
            if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
                item->valuedouble < NF_MIN_INTERVAL || item->valuedouble > NF_DAY ||
                floor(item->valuedouble) != item->valuedouble) ok = false;
            else next.update_interval_s = (uint32_t)item->valuedouble;
        } else if (!strcmp(k, "paused")) {
            if (!cJSON_IsBool(item)) ok = false;
            else next.paused = cJSON_IsTrue(item);
        } else if (!strcmp(k, "led_enabled")) {
            if (!cJSON_IsBool(item)) ok = false;
            else next.led_enabled = cJSON_IsTrue(item);
        } else ok = false;
#undef STR_FIELD
    }
    ok = ok && nf_config_valid(&next) && (!remote || nf_url_valid(next.image_url, false));
    cJSON_Delete(root);
    if (ok) *out = next;
    return ok;
}
esp_err_t nf_config_load(nf_config *c) {
    nf_config_default(c);
    nvs_handle_t h;
    esp_err_t e = nvs_open("neoframe", NVS_READONLY, &h);
    if (e != ESP_OK) return e;
    nf_config stored={0}; size_t n = sizeof(stored);
    e = nvs_get_blob(h, "config", &stored, &n); nvs_close(h);
    if (e != ESP_OK) return e;
    /* v1 used this exact prefix, rounded to four-byte struct alignment. */
    size_t legacy_size = (offsetof(nf_config, timezone) + 3) & ~(size_t)3;
    if (stored.version == 1 && n == legacy_size) {
        stored.version=3;
        strcpy(stored.timezone,"CET-1CEST,M3.5.0,M10.5.0/3");
        stored.paused=0;
        /* Old low_power was inert: do not put an existing wall device to sleep on upgrade. */
        strcpy(stored.power_profile,"always_on");
    } else if (stored.version==2 && n==offsetof(nf_config,firmware_version)) {
        stored.version=3;
    } else if (n != sizeof(stored)) return ESP_ERR_INVALID_ARG;
    if (!nf_config_valid(&stored)) return ESP_ERR_INVALID_ARG;
    *c = stored; return ESP_OK;
}
esp_err_t nf_config_save(const nf_config *c) {
    if (!nf_config_valid(c)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h; esp_err_t e = nvs_open("neoframe", NVS_READWRITE, &h);
    if (e != ESP_OK) return e;
    e = nvs_set_blob(h, "config", c, sizeof(*c));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h); return e;
}
