#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define NF_FRAME_SIZE 960000
#define NF_ROW_BYTES 600
#define NF_ROWS 1600
#define NF_MIN_INTERVAL 180
#define NF_DAY 86400
bool nf_pixels_valid(const uint8_t *data, size_t size);
size_t nf_row_offset(unsigned controller, unsigned row);
bool nf_url_valid(const char *url, bool optional);
bool nf_time_valid(const char *time);
