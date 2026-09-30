#pragma once
#include "esp_err.h"
/* Manual HTTP(S) OTA; downloads into the inactive slot. */
esp_err_t nf_ota_check_and_apply(const char *firmware_url);
/* Call once, after confirming this boot is healthy (e.g. after the first
 * successful WiFi connection), to cancel a pending rollback so the
 * bootloader doesn't revert to the previous slot on the next reset. */
void nf_ota_confirm_healthy(void);

/* Skip matching installed versions; verify embedded version before installation. */
esp_err_t nf_ota_apply_requested(const char *url,const char *version);
