#pragma once
#include "esp_err.h"
/* Fetches firmware_url over HTTPS and, if its embedded app version differs
 * from the currently running image, flashes it to the inactive OTA slot and
 * reboots on success. Returns ESP_OK (without rebooting) if already
 * up to date, or an error if the URL is missing/not HTTPS or the fetch/
 * flash fails. firmware_url is never remote-settable and never surfaced on
 * the always-on settings page - see config.h/README. */
esp_err_t nf_ota_check_and_apply(const char *firmware_url);
/* Call once, after confirming this boot is healthy (e.g. after the first
 * successful WiFi connection), to cancel a pending rollback so the
 * bootloader doesn't revert to the previous slot on the next reset. */
void nf_ota_confirm_healthy(void);
