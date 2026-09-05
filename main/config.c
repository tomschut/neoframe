#include "config.h"
#include "cJSON.h"
#include "nvs.h"
#include <string.h>
#include <math.h>
void nf_config_default(nf_config *c) {
    memset(c, 0, sizeof(*c));
    c->version = 1; c->update_interval_s = 600; c->led_enabled = 1;
    strcpy(c->active_start, "08:00"); strcpy(c->active_end, "00:00");
    strcpy(c->power_profile, "low_power");
}
bool nf_config_valid(const nf_config *c) {
#define TERMINATED(f) if (!memchr(c->f, 0, sizeof(c->f))) return false
    TERMINATED(wifi_ssid); TERMINATED(wifi_pass); TERMINATED(image_url); TERMINATED(config_url);
    TERMINATED(active_start); TERMINATED(active_end); TERMINATED(power_profile);
#undef TERMINATED
    size_t n = strlen(c->wifi_pass);
    if (n && n < 8) return false;
    if (n == 64) for (size_t i = 0; i < n; ++i)
        if (!strchr("0123456789abcdefABCDEF", c->wifi_pass[i])) return false;
    return c->version == 1 && c->update_interval_s >= NF_MIN_INTERVAL && c->update_interval_s <= NF_DAY &&
        nf_url_valid(c->image_url, true) && nf_url_valid(c->config_url, true) &&
        nf_time_valid(c->active_start) && nf_time_valid(c->active_end) && c->led_enabled <= 1 &&
        (!strcmp(c->power_profile, "low_power") || !strcmp(c->power_profile, "always_on"));
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
    if (remote) for (size_t i = 0; i < 5; ++i)
        if (!cJSON_GetObjectItemCaseSensitive(root, required[i])) ok = false;
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
        } else STR_FIELD(image_url)
        else STR_FIELD(active_start)
        else STR_FIELD(active_end)
        else STR_FIELD(power_profile)
        else if (!strcmp(k, "update_interval_s")) {
            if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
                item->valuedouble < NF_MIN_INTERVAL || item->valuedouble > NF_DAY ||
                floor(item->valuedouble) != item->valuedouble) ok = false;
            else next.update_interval_s = (uint32_t)item->valuedouble;
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
    nf_config stored; size_t n = sizeof(stored);
    e = nvs_get_blob(h, "config", &stored, &n); nvs_close(h);
    if (e != ESP_OK) return e;
    if (n != sizeof(stored) || !nf_config_valid(&stored)) return ESP_ERR_INVALID_ARG;
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
