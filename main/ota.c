#include "ota.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include <string.h>
#include <stdbool.h>

static const char *TAG = "neoframe-ota";

void nf_ota_confirm_healthy(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "This firmware confirmed healthy; rollback cancelled");
    }
}

esp_err_t nf_ota_check_and_apply(const char *firmware_url) {
    if (!firmware_url || (strncmp(firmware_url, "http://", 7) && strncmp(firmware_url, "https://", 8)))
        return ESP_ERR_INVALID_ARG;
    bool secure = !strncmp(firmware_url, "https://", 8);

    esp_http_client_config_t http_cfg = {
        .url = firmware_url,
        .crt_bundle_attach = secure ? esp_crt_bundle_attach : NULL,
        .timeout_ms = 10000,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &http_cfg };
    esp_https_ota_handle_t handle = NULL;
    esp_err_t e = esp_https_ota_begin(&ota_cfg, &handle);
    if (e != ESP_OK) { ESP_LOGE(TAG, "OTA start failed: %s", esp_err_to_name(e)); return e; }

    esp_app_desc_t new_desc;
    if (esp_https_ota_get_img_desc(handle, &new_desc) == ESP_OK) {
        esp_app_desc_t running_desc;
        if (esp_ota_get_partition_description(esp_ota_get_running_partition(), &running_desc) == ESP_OK &&
            !strcmp(new_desc.version, running_desc.version)) {
            ESP_LOGI(TAG, "Already up to date (%s)", running_desc.version);
            esp_https_ota_abort(handle);
            return ESP_OK;
        }
    }

    do { e = esp_https_ota_perform(handle); } while (e == ESP_ERR_HTTPS_OTA_IN_PROGRESS);
    if (e != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "OTA download incomplete: %s", esp_err_to_name(e));
        esp_https_ota_abort(handle);
        return e != ESP_OK ? e : ESP_FAIL;
    }

    e = esp_https_ota_finish(handle);
    if (e != ESP_OK) { ESP_LOGE(TAG, "OTA finish failed: %s", esp_err_to_name(e)); return e; }

    ESP_LOGI(TAG, "OTA applied (%s); rebooting", new_desc.version);
    esp_restart();
}
