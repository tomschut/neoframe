#include "ota.h"
#include "esp_https_ota.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
static esp_app_desc_t running={.version="old"}, offered={.version="new"};
static int began,aborted,finished,performed;
static esp_err_t begin_error,desc_error,download_error,finish_error;
static bool complete=true;
static jmp_buf reboot;
const char *esp_err_to_name(esp_err_t e) { (void)e; return "test-error"; }
esp_err_t esp_crt_bundle_attach(void *p) { (void)p;return ESP_OK; }
const esp_partition_t *esp_ota_get_running_partition(void) { return NULL; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p,esp_ota_img_states_t *s) { (void)p;*s=0;return ESP_OK; }
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) { return ESP_OK; }
esp_err_t esp_ota_get_partition_description(const esp_partition_t *p,esp_app_desc_t *d) { (void)p;*d=running;return ESP_OK; }
const esp_app_desc_t *esp_ota_get_app_description(void) { return &running; }
_Noreturn void esp_restart(void) { longjmp(reboot,1); }
esp_err_t esp_https_ota_begin(const esp_https_ota_config_t *c,esp_https_ota_handle_t *h) {
 assert(!strcmp(c->http_config->url,"http://host/fw.bin"));
 assert(!c->http_config->crt_bundle_attach); began++; *h=&began;return begin_error;
}
esp_err_t esp_https_ota_get_img_desc(esp_https_ota_handle_t h,esp_app_desc_t *d) { (void)h;*d=offered;return desc_error; }
esp_err_t esp_https_ota_abort(esp_https_ota_handle_t h) { (void)h;aborted++;return ESP_OK; }
esp_err_t esp_https_ota_perform(esp_https_ota_handle_t h) { (void)h;performed++;return download_error; }
bool esp_https_ota_is_complete_data_received(esp_https_ota_handle_t h) { (void)h;return complete; }
esp_err_t esp_https_ota_finish(esp_https_ota_handle_t h) { (void)h;finished++;return finish_error; }
int main(void) {
 const char *url="http://host/fw.bin";
 assert(nf_ota_apply_requested(url,"")==ESP_OK && !began);
 assert(nf_ota_apply_requested(url,"old")==ESP_OK && !began);
 begin_error=ESP_ERR_TIMEOUT; assert(nf_ota_apply_requested(url,"new")==ESP_ERR_TIMEOUT && !performed && !finished);
 begin_error=ESP_FAIL; assert(nf_ota_apply_requested(url,"new")==ESP_FAIL && !performed && !finished);
 begin_error=ESP_OK;
 assert(nf_ota_apply_requested(url,"wrong")==ESP_ERR_INVALID_RESPONSE && aborted==1 && !performed);
 desc_error=ESP_FAIL; assert(nf_ota_apply_requested(url,"new")==ESP_FAIL && !performed); desc_error=ESP_OK;
 download_error=ESP_FAIL; assert(nf_ota_apply_requested(url,"new")==ESP_FAIL && !finished); download_error=ESP_OK;
 complete=false; assert(nf_ota_apply_requested(url,"new")==ESP_FAIL && !finished); complete=true;
 finish_error=ESP_FAIL; assert(nf_ota_apply_requested(url,"new")==ESP_FAIL); finish_error=ESP_OK;
 if (!setjmp(reboot)) { nf_ota_apply_requested(url,"new"); assert(!"expected reboot"); }
 assert(finished==2);
 strcpy(running.version,"new"); int count=began;
 assert(nf_ota_apply_requested(url,"new")==ESP_OK && began==count);
 puts("PASS: OTA no-op, version mismatch, description/download/finish failures, retry, reboot, installed-version skip");
}
