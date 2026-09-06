#include "logbuf.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#define NF_LOGBUF_SIZE (16*1024)

static char *s_buf;
static size_t s_head;       /* next write position; wraps */
static bool s_full;
static SemaphoreHandle_t s_mutex;
static vprintf_like_t s_original;

static void append(const char *data, size_t len) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < len; ++i) {
        s_buf[s_head] = data[i];
        s_head = (s_head + 1) % NF_LOGBUF_SIZE;
        if (s_head == 0) s_full = true;
    }
    xSemaphoreGive(s_mutex);
}

static int logbuf_vprintf(const char *fmt, va_list args) {
    int result = s_original ? s_original(fmt, args) : vprintf(fmt, args);
    char line[256];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n > 0) {
        if (n >= (int)sizeof(line)) n = sizeof(line) - 1;
        /* Strip ANSI color escapes (\033[...<letter>) for a clean web view. */
        char clean[256];
        size_t o = 0;
        for (int i = 0; i < n; ++i) {
            if (line[i] == '\033') { while (i < n && line[i] != 'm') ++i; continue; }
            clean[o++] = line[i];
        }
        append(clean, o);
    }
    return result;
}

void nf_logbuf_init(void) {
    s_buf = heap_caps_malloc(NF_LOGBUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_buf) return;
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) { heap_caps_free(s_buf); s_buf = NULL; return; }
    s_original = esp_log_set_vprintf(logbuf_vprintf);
}

size_t nf_logbuf_read(char *out, size_t cap) {
    if (!s_buf || cap == 0) { if (cap) *out = 0; return 0; }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t total = s_full ? NF_LOGBUF_SIZE : s_head;
    size_t oldest = s_full ? s_head : 0;
    size_t n = (total < cap - 1) ? total : cap - 1;
    size_t start = (oldest + (total - n)) % NF_LOGBUF_SIZE;
    for (size_t i = 0; i < n; ++i) out[i] = s_buf[(start + i) % NF_LOGBUF_SIZE];
    out[n] = 0;
    xSemaphoreGive(s_mutex);
    return n;
}
