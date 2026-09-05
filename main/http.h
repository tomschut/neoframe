#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
typedef struct { char etag[256], modified[128]; } nf_validator;
/* Returns ESP_OK only for a complete 200 or an allowed 304. */
esp_err_t nf_fetch(const char *url, const nf_validator *conditional,
    uint8_t *body, size_t capacity, size_t *length, int *status,
    nf_validator *received, int budget_ms);
