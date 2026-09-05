#pragma once
#include "core.h"
#include "esp_err.h"
typedef struct {
    uint32_t version;
    uint32_t update_interval_s;
    char wifi_ssid[33], wifi_pass[65];
    char image_url[512], config_url[512];
    char active_start[6], active_end[6];
    char power_profile[16];
    uint8_t led_enabled;
} nf_config;
void nf_config_default(nf_config *c);
bool nf_config_valid(const nf_config *c);
bool nf_config_parse(const char *json, const nf_config *base, nf_config *out, bool remote);
esp_err_t nf_config_load(nf_config *c);
esp_err_t nf_config_save(const nf_config *c);
