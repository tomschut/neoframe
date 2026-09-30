#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define NF_FRAME_SIZE 960000
#define NF_ROW_BYTES 600
#define NF_ROWS 1600
#define NF_MIN_INTERVAL 180
#define NF_DAY 86400
#define NF_BOOT_SETTINGS_SECONDS 300
/* ready_since < 0 means WiFi/settings access has not become available yet. */
bool nf_boot_settings_pending(bool timer_wake, int64_t ready_since, int64_t now);
/* STA connection attempts without obtaining an IP before portal fallback.
 * Attempts use exponential backoff, capped at 60 seconds. */
#define NF_MAX_CONNECT_FAILURES 10
bool nf_pixels_valid(const uint8_t *data, size_t size);
size_t nf_row_offset(unsigned controller, unsigned row);
bool nf_url_valid(const char *url, bool optional);
bool nf_time_valid(const char *time);

/* Seconds until the next interval slot within the daily window. */
uint32_t nf_schedule_delay(unsigned day_second, unsigned start, unsigned stop, uint32_t interval);
bool nf_schedule_active(unsigned day_second, unsigned start, unsigned stop);
