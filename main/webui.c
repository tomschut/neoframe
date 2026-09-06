#include "webui.h"
#include "formutil.h"
#include "logbuf.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG="neoframe-webui";
static const nf_config *s_config;
static QueueHandle_t s_serial_messages;
static char s_error[160];
static httpd_handle_t s_server;

static esp_err_t enqueue(const char *line) {
    char *copy=strdup(line);
    if (!copy) return ESP_ERR_NO_MEM;
    if (xQueueSend(s_serial_messages,&copy,0)!=pdTRUE) { free(copy); return ESP_ERR_INVALID_STATE; }
    return ESP_OK;
}

static esp_err_t send_page(httpd_req_t *req) {
    char image_url[1024], config_url[1024], firmware_url[1024];
    nf_html_escape(s_config->image_url,image_url,sizeof(image_url));
    nf_html_escape(s_config->config_url,config_url,sizeof(config_url));
    nf_html_escape(s_config->firmware_url,firmware_url,sizeof(firmware_url));

    const esp_app_desc_t *app=esp_ota_get_app_description();
    char version[64];
    nf_html_escape(app?app->version:"unknown",version,sizeof(version));

    char *html=malloc(6144);
    if (!html) return httpd_resp_send_500(req);
    int n=snprintf(html,6144,
        "<!doctype html><title>NeoFrame settings</title>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<style>body{font-family:sans-serif;max-width:420px;margin:24px auto;padding:0 16px}"
        "label{display:block;margin-top:12px;font-weight:600}"
        "input,select{width:100%%;padding:8px;margin-top:4px;box-sizing:border-box;font-size:16px}"
        "button{margin-top:20px;width:100%%;padding:12px;font-size:16px}"
        ".err{background:#fee;border:1px solid #c00;padding:8px;border-radius:4px;color:#900}"
        "form{margin-top:24px}h3{margin-top:32px;border-top:1px solid #ddd;padding-top:16px}"
        ".checkline{display:flex;align-items:center;gap:8px;margin-top:12px}"
        ".checkline input{width:auto}</style>"
        "<h2>NeoFrame settings</h2>"
        "%s%s%s"
        "<form method=post action=/save>"
        "<label>Image URL</label><input name=image_url type=url maxlength=500 required value=\"%s\">"
        "<label>Settings URL (optional)</label><input name=config_url type=url maxlength=500 value=\"%s\">"
        "<label>Refresh every (minutes)</label><input name=interval type=number min=3 max=1440 required value=\"%d\">"
        "<label>Active from</label><input name=start type=time required value=\"%s\">"
        "<label>Active until</label><input name=stop type=time required value=\"%s\">"
        "<label>Power profile</label><select name=power_profile>"
        "<option value=low_power%s>low_power</option><option value=always_on%s>always_on</option></select>"
        "<div class=checkline><input id=led name=led_enabled type=checkbox value=1%s>"
        "<label for=led style=margin:0>LED enabled</label></div>"
        "<h3>Firmware</h3><p>Running version: <code>%s</code></p>"
        "<label>Firmware URL (optional)</label><input name=firmware_url type=url maxlength=500 value=\"%s\">"
        "<button type=submit>Save</button>"
        "</form>"
        "<form method=post action=/reload>"
        "<button type=submit>Force reload now</button>"
        "</form>"
        "<form method=post action=/ota>"
        "<button type=submit>Check &amp; install firmware update now</button>"
        "</form>"
        "<h3>Logs</h3><p><a href=/logs>View recent log output</a></p>",
        *s_error?"<div class=err>":"", s_error, *s_error?"</div>":"",
        image_url, config_url, (int)(s_config->update_interval_s/60),
        s_config->active_start, s_config->active_end,
        strcmp(s_config->power_profile,"low_power")?"":" selected",
        strcmp(s_config->power_profile,"always_on")?"":" selected",
        s_config->led_enabled?" checked":"",
        version, firmware_url);
    httpd_resp_set_type(req,"text/html");
    httpd_resp_send(req,html,n);
    free(html);
    return ESP_OK;
}

static esp_err_t handle_root(httpd_req_t *req) { return send_page(req); }

static esp_err_t handle_save(httpd_req_t *req) {
    int total=req->content_len;
    if (total<=0 || total>=4096) return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Form too large");
    char *body=malloc(total+1);
    if (!body) return httpd_resp_send_500(req);
    int received=0;
    while (received<total) {
        int r=httpd_req_recv(req,body+received,total-received);
        if (r<=0) { free(body); return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Read failed"); }
        received+=r;
    }
    body[received]=0;

    char image_url[512]={0}, config_url[512]={0}, firmware_url[512]={0}, interval[16]={0}, start[6]={0}, stop[6]={0},
        power_profile[16]={0}, led[4]={0};
    nf_form_field(body,"image_url",image_url,sizeof(image_url));
    nf_form_field(body,"config_url",config_url,sizeof(config_url));
    nf_form_field(body,"firmware_url",firmware_url,sizeof(firmware_url));
    nf_form_field(body,"interval",interval,sizeof(interval));
    nf_form_field(body,"start",start,sizeof(start));
    nf_form_field(body,"stop",stop,sizeof(stop));
    nf_form_field(body,"power_profile",power_profile,sizeof(power_profile));
    nf_form_field(body,"led_enabled",led,sizeof(led));
    free(body);

    /* Every value is attacker-influenced (a raw POST can send anything
     * regardless of what the HTML form allows) and gets embedded into a
     * JSON string below, so every field is escaped - not just the URLs. */
    char image_url_e[1040], config_url_e[1040], firmware_url_e[1040], start_e[16], stop_e[16], power_profile_e[32];
    nf_json_escape(image_url,image_url_e,sizeof(image_url_e));
    nf_json_escape(config_url,config_url_e,sizeof(config_url_e));
    nf_json_escape(firmware_url,firmware_url_e,sizeof(firmware_url_e));
    nf_json_escape(start,start_e,sizeof(start_e));
    nf_json_escape(stop,stop_e,sizeof(stop_e));
    nf_json_escape(power_profile,power_profile_e,sizeof(power_profile_e));

    long minutes=strtol(interval,NULL,10);
    char json[3400];
    int n=snprintf(json,sizeof(json),
        "{\"image_url\":\"%s\",\"config_url\":\"%s\",\"firmware_url\":\"%s\",\"update_interval_s\":%ld,"
        "\"active_start\":\"%s\",\"active_end\":\"%s\",\"power_profile\":\"%s\",\"led_enabled\":%s}",
        image_url_e,config_url_e,firmware_url_e,minutes>0?minutes*60:0,start_e,stop_e,power_profile_e,*led?"true":"false");
    if (n<0 || n>=(int)sizeof(json)) {
        snprintf(s_error,sizeof(s_error),"Submitted values too long.");
        return send_page(req);
    }

    nf_config candidate;
    if (!nf_config_parse(json,s_config,&candidate,false)) {
        snprintf(s_error,sizeof(s_error),"Please check the values below - one or more fields is invalid.");
        return send_page(req);
    }
    *s_error=0;
    if (enqueue(json)!=ESP_OK) ESP_LOGW(TAG,"Settings queue full; try again");
    httpd_resp_set_type(req,"text/html");
    httpd_resp_sendstr(req,
        "<!doctype html><title>NeoFrame</title>"
        "<body style=\"font-family:sans-serif;max-width:420px;margin:40px auto;text-align:center\">"
        "<h2>Saved</h2><p>Applying now.</p><p><a href=/>Back</a></p>");
    return ESP_OK;
}

static esp_err_t handle_reload(httpd_req_t *req) {
    if (enqueue("force")!=ESP_OK) ESP_LOGW(TAG,"Settings queue full; try again");
    httpd_resp_set_type(req,"text/html");
    httpd_resp_sendstr(req,
        "<!doctype html><title>NeoFrame</title>"
        "<body style=\"font-family:sans-serif;max-width:420px;margin:40px auto;text-align:center\">"
        "<h2>Reload requested</h2><p><a href=/>Back</a></p>");
    return ESP_OK;
}

static esp_err_t handle_ota(httpd_req_t *req) {
    if (enqueue("ota")!=ESP_OK) ESP_LOGW(TAG,"Settings queue full; try again");
    httpd_resp_set_type(req,"text/html");
    httpd_resp_sendstr(req,
        "<!doctype html><title>NeoFrame</title>"
        "<body style=\"font-family:sans-serif;max-width:420px;margin:40px auto;text-align:center\">"
        "<h2>OTA check requested</h2><p>The device will reboot automatically if an update is "
        "found; this page and the settings page will be unreachable for a moment.</p>"
        "<p><a href=/>Back</a></p>");
    return ESP_OK;
}

static esp_err_t handle_logs(httpd_req_t *req) {
    char *buf=malloc(16*1024+1);
    if (!buf) return httpd_resp_send_500(req);
    size_t n=nf_logbuf_read(buf,16*1024+1);
    httpd_resp_set_type(req,"text/plain");
    httpd_resp_send(req,buf,n);
    free(buf);
    return ESP_OK;
}

esp_err_t nf_webui_start(const nf_config *config, QueueHandle_t serial_messages) {
    s_config=config; s_serial_messages=serial_messages; *s_error=0;
    httpd_config_t cfg=HTTPD_DEFAULT_CONFIG();
    cfg.stack_size=8192;
    esp_err_t e=httpd_start(&s_server,&cfg);
    if (e!=ESP_OK) { ESP_LOGE(TAG,"Settings server failed to start: %s",esp_err_to_name(e)); s_server=NULL; return e; }
    httpd_uri_t root={.uri="/",.method=HTTP_GET,.handler=handle_root};
    httpd_uri_t save={.uri="/save",.method=HTTP_POST,.handler=handle_save};
    httpd_uri_t reload={.uri="/reload",.method=HTTP_POST,.handler=handle_reload};
    httpd_uri_t ota={.uri="/ota",.method=HTTP_POST,.handler=handle_ota};
    httpd_uri_t logs={.uri="/logs",.method=HTTP_GET,.handler=handle_logs};
    httpd_register_uri_handler(s_server,&root);
    httpd_register_uri_handler(s_server,&save);
    httpd_register_uri_handler(s_server,&reload);
    httpd_register_uri_handler(s_server,&ota);
    httpd_register_uri_handler(s_server,&logs);
    ESP_LOGI(TAG,"Always-on settings page at http://<device-ip>/ (see \"sta ip:\" above for the address)");
    return ESP_OK;
}

void nf_webui_stop(void) {
    if (s_server) { httpd_stop(s_server); s_server=NULL; }
}
