#pragma once
#include <stddef.h>
/* Captures a copy of everything ESP_LOG prints into a fixed-size RAM ring
 * buffer (in addition to the normal console output, never instead of it),
 * so the always-on settings page can show recent log history. Call once,
 * early in app_main. */
void nf_logbuf_init(void);
/* Copies the most recent buffered log text (chronological order, ANSI color
 * codes stripped) into out, NUL-terminated, truncated to fit cap. Returns
 * the number of bytes written (excluding the NUL). */
size_t nf_logbuf_read(char *out, size_t cap);
