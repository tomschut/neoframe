#pragma once
#include "config.h"
/* Returns only if remote settings select always_on. Otherwise sleeps. */
void nf_lowpower_cycle(nf_config *config);
