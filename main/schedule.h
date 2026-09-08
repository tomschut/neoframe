#pragma once
#include "config.h"
#include <time.h>
bool nf_schedule_now(const nf_config *c,time_t now);
uint32_t nf_schedule_next(const nf_config *c,time_t now);
