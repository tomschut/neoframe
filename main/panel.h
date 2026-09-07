#pragma once
#include "esp_err.h"
#include <stdint.h>
esp_err_t nf_panel_init(void);
esp_err_t nf_panel_render(const uint8_t *frame);
/* De-asserts SW_C, killing the e-paper analog boost supply (VGH/VGL/VDDP/
 * VDDN/VCOM) - the same rail a failed render already de-asserts. Safe to
 * call any time; nf_panel_render re-asserts and fully re-initializes the
 * panel from scratch on its next call regardless of this. */
void nf_panel_sleep(void);
