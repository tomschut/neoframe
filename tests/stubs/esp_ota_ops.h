#pragma once
#include "esp_err.h"
typedef struct { char version[32]; } esp_app_desc_t;
typedef struct { int unused; } esp_partition_t;
typedef int esp_ota_img_states_t;
#define ESP_OTA_IMG_PENDING_VERIFY 1
const esp_partition_t *esp_ota_get_running_partition(void);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *,esp_ota_img_states_t *);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);
esp_err_t esp_ota_get_partition_description(const esp_partition_t *,esp_app_desc_t *);
const esp_app_desc_t *esp_ota_get_app_description(void);
_Noreturn void esp_restart(void);
