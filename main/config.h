#pragma once
#include "core.h"
#include "esp_err.h"
typedef struct {
    uint8_t days; /* bit 0 Monday .. bit 6 Sunday; day on which window starts */
    char start[6], stop[6];
    uint32_t interval_s;
} nf_window;
#define NF_MAX_WINDOWS 8
typedef struct {
    uint32_t version;
    uint32_t update_interval_s;
    char wifi_ssid[33], wifi_pass[65];
    char image_url[512], config_url[512];
    char firmware_url[512]; /* HTTP(S); manual or remote OTA */
    char active_start[6], active_end[6];
    char power_profile[16];
    uint8_t led_enabled;
    char timezone[64]; /* POSIX TZ rule */
    uint8_t paused;
    uint8_t window_count;
    nf_window windows[NF_MAX_WINDOWS];
    char firmware_version[32]; /* Empty disables automatic OTA. */
} nf_config;
void nf_config_default(nf_config *c);
bool nf_config_valid(const nf_config *c);
bool nf_config_parse(const char *json, const nf_config *base, nf_config *out, bool remote);
esp_err_t nf_config_load(nf_config *c);
esp_err_t nf_config_save(const nf_config *c);
/* Persist empty WiFi credentials, preserving all other settings. */
esp_err_t nf_config_reset_wifi(nf_config *c);
