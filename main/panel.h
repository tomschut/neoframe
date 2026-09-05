#pragma once
#include "esp_err.h"
#include <stdint.h>
esp_err_t nf_panel_init(void);
esp_err_t nf_panel_render(const uint8_t *frame);
