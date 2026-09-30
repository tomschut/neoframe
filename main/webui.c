#include "webui.h"
#include "formutil.h"
#include "cJSON.h"
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
static const bool *s_paused;

static esp_err_t enqueue(const char *line) {
    char *copy=strdup(line);
    if (!copy) return ESP_ERR_NO_MEM;
    if (xQueueSend(s_serial_messages,&copy,0)!=pdTRUE) { free(copy); return ESP_ERR_INVALID_STATE; }
    return ESP_OK;
}

static esp_err_t send_page(httpd_req_t *req) {
    char image_url[1024], config_url[1024], firmware_url[1024], timezone[384];
    nf_html_escape(s_config->timezone,timezone,sizeof(timezone));
    nf_html_escape(s_config->image_url,image_url,sizeof(image_url));
    nf_html_escape(s_config->config_url,config_url,sizeof(config_url));
    nf_html_escape(s_config->firmware_url,firmware_url,sizeof(firmware_url));

    const esp_app_desc_t *app=esp_ota_get_app_description();
    char version[64];
    nf_html_escape(app?app->version:"unknown",version,sizeof(version));

    char *html=malloc(8192);
    if (!html) return httpd_resp_send_500(req);
    int n=snprintf(html,8192,
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
        "<h2>NeoFrame settings</h2><p><a href=/schedule>Edit sleep schedule JSON</a></p>"
        "%s%s%s"
        "<form method=post action=/save>"
        "<label>Image URL</label><input name=image_url type=url maxlength=500 required value=\"%s\">"
        "<label>Settings URL (optional)</label><input name=config_url type=url maxlength=500 value=\"%s\">"
        "<label>Refresh every (minutes)</label><input name=interval type=number min=3 max=1440 required value=\"%d\">"
        "<label>Active from</label><input name=start type=time required value=\"%s\">"
        "<label>Active until</label><input name=stop type=time required value=\"%s\">"
        "<label>Timezone (POSIX rule)</label><input name=timezone maxlength=63 value=\"%s\">"
        "<label>Power profile</label><select name=power_profile>"
        "<option value=low_power%s>low_power</option><option value=always_on%s>always_on</option>"
        "<option value=ac_power%s>ac_power</option></select>"
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
        "<h3>Refresh state</h3><p>Currently: <strong>%s</strong></p>"
        "<form method=post action=/%s>"
        "<button type=submit>%s</button>"
        "</form>"
        "<h3>WiFi setup</h3><p>Forget the saved WiFi network and restart into "
        "the setup hotspot. Your image settings and schedule will be kept.</p>"
        "<form method=post action=/wifi-reset>"
        "<button type=submit>Reset WiFi and open setup</button></form>"
        "<h3>Logs</h3><p><a href=/logs>View recent log output</a></p>",
        *s_error?"<div class=err>":"", s_error, *s_error?"</div>":"",
        image_url, config_url, (int)(s_config->update_interval_s/60),
        s_config->active_start, s_config->active_end, timezone,
        strcmp(s_config->power_profile,"low_power")?"":" selected",
        strcmp(s_config->power_profile,"always_on")?"":" selected",
        strcmp(s_config->power_profile,"ac_power")?"":" selected",
        s_config->led_enabled?" checked":"",
        version, firmware_url,
        s_paused&&*s_paused?"Paused":"Active",
        s_paused&&*s_paused?"resume":"pause",
        s_paused&&*s_paused?"Resume":"Pause");
    if (n<0 || n>=8192) { free(html); return httpd_resp_send_500(req); }
    httpd_resp_set_type(req,"text/html");
    httpd_resp_send(req,html,n);
    free(html);
    return ESP_OK;
}

static esp_err_t handle_schedule(httpd_req_t *req) {
    if (req->method==HTTP_POST) {
        if (!req->content_len || req->content_len>4096) return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"JSON must be 1-4096 bytes");
        char *body=calloc(1,req->content_len+1);
        nf_config *candidate=malloc(sizeof(*candidate));
        if (!body || !candidate) { free(body); free(candidate); return httpd_resp_send_500(req); }
        size_t used=0;
        while (used<req->content_len) {
            int n=httpd_req_recv(req,body+used,req->content_len-used);
            if (n<=0) { free(body); free(candidate); return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Read failed"); }
            used+=n;
        }
        bool valid=!memchr(body,0,used) && nf_config_parse(body,s_config,candidate,false);
        esp_err_t e=valid?enqueue(body):ESP_ERR_INVALID_ARG;
        free(candidate); free(body);
        if (e!=ESP_OK) return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid settings or queue busy");
        httpd_resp_set_type(req,"text/plain");
        return httpd_resp_sendstr(req,"Settings queued. Low-power mode makes this page unavailable while asleep.");
    }
    httpd_resp_set_type(req,"text/html");
    return httpd_resp_sendstr(req,
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'><title>Schedule</title>"
        "<h2>Remote settings and sleep schedule</h2><p>Paste the JSON settings schema below. "
        "Use the Settings URL on the main page for automatic remote updates. "
        "Low-power mode sleeps between checks; the local page will be unavailable.</p>"
        "<textarea id=j rows=22 style='width:95%;max-width:800px' placeholder='Paste settings JSON here'></textarea>"
        "<p><button id=b>Apply settings</button> <a href=/>Back</a></p><pre id=result></pre>"
        "<script>b.onclick=async()=>{try{JSON.parse(j.value);const r=await fetch('/schedule',"
        "{method:'POST',headers:{'Content-Type':'application/json'},body:j.value});"
        "result.textContent=await r.text()}catch(e){result.textContent=String(e)}};</script>");
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
    char submitted_timezone[64]={0};
    nf_form_field(body,"timezone",submitted_timezone,sizeof(submitted_timezone));
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
    char timezone_value[64], timezone_e[384];
    /* Missing field from an older browser form preserves the current timezone. */
    strcpy(timezone_value,s_config->timezone);
    if (*submitted_timezone) strcpy(timezone_value,submitted_timezone);
    nf_json_escape(timezone_value,timezone_e,sizeof(timezone_e));
    char json[3900];
    int n=snprintf(json,sizeof(json),
        "{\"timezone\":\"%s\",\"image_url\":\"%s\",\"config_url\":\"%s\",\"firmware_url\":\"%s\",\"update_interval_s\":%ld,"
        "\"active_start\":\"%s\",\"active_end\":\"%s\",\"power_profile\":\"%s\",\"led_enabled\":%s}",
        timezone_e,image_url_e,config_url_e,firmware_url_e,minutes>0?minutes*60:0,start_e,stop_e,power_profile_e,*led?"true":"false");
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

static esp_err_t handle_wifi_reset(httpd_req_t *req) {
    if (enqueue("wifi-reset")!=ESP_OK) {
        httpd_resp_set_status(req,"503 Service Unavailable");
        httpd_resp_set_type(req,"text/plain");
        return httpd_resp_sendstr(req,"WiFi reset could not be queued. Please try again.");
    }
    httpd_resp_set_type(req,"text/html");
    return httpd_resp_sendstr(req,
        "<!doctype html><title>NeoFrame WiFi setup</title>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<body style='font-family:sans-serif;max-width:420px;margin:40px auto;padding:0 16px'>"
        "<h2>WiFi reset requested</h2><p>After saving the reset, NeoFrame will restart "
        "and disconnect from this network.</p><p>Connect to the <strong>NeoFrame-XXXXXX</strong> "
        "hotspot using password <strong>1234567890</strong>, then open "
        "<strong>http://192.168.4.1/</strong> to choose a WiFi network.</p>"
        "<p>If the device stays on this network, check <a href=/logs>the logs</a> for a storage error.</p>");
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

static esp_err_t handle_pause(httpd_req_t *req) {
    if (enqueue("pause")!=ESP_OK) ESP_LOGW(TAG,"Settings queue full; try again");
    httpd_resp_set_type(req,"text/html");
    httpd_resp_sendstr(req,
        "<!doctype html><title>NeoFrame</title>"
        "<body style=\"font-family:sans-serif;max-width:420px;margin:40px auto;text-align:center\">"
        "<h2>Paused</h2><p>Automatic refresh is off - the panel keeps showing its last image.</p>"
        "<p><a href=/>Back</a></p>");
    return ESP_OK;
}

static esp_err_t handle_resume(httpd_req_t *req) {
    if (enqueue("resume")!=ESP_OK) ESP_LOGW(TAG,"Settings queue full; try again");
    httpd_resp_set_type(req,"text/html");
    httpd_resp_sendstr(req,
        "<!doctype html><title>NeoFrame</title>"
        "<body style=\"font-family:sans-serif;max-width:420px;margin:40px auto;text-align:center\">"
        "<h2>Resumed</h2><p>Automatic refresh is back on.</p>"
        "<p><a href=/>Back</a></p>");
    return ESP_OK;
}

esp_err_t nf_webui_start(const nf_config *config, QueueHandle_t serial_messages, const bool *paused) {
    s_config=config; s_serial_messages=serial_messages; s_paused=paused; *s_error=0;
    httpd_config_t cfg=HTTPD_DEFAULT_CONFIG();
    /* handle_save's locals alone (three 1040-byte escape buffers, a 3400-byte
     * json buffer, a ~2.2KB nf_config) exceed 10KB - 8192 silently overflowed
     * this task's stack on every POST /save regardless of body content. */
    cfg.stack_size=24576;
    cfg.max_uri_handlers=12;
    esp_err_t e=httpd_start(&s_server,&cfg);
    if (e!=ESP_OK) { ESP_LOGE(TAG,"Settings server failed to start: %s",esp_err_to_name(e)); s_server=NULL; return e; }
    httpd_uri_t root={.uri="/",.method=HTTP_GET,.handler=handle_root};
    httpd_uri_t save={.uri="/save",.method=HTTP_POST,.handler=handle_save};
    httpd_uri_t reload={.uri="/reload",.method=HTTP_POST,.handler=handle_reload};
    httpd_uri_t ota={.uri="/ota",.method=HTTP_POST,.handler=handle_ota};
    httpd_uri_t logs={.uri="/logs",.method=HTTP_GET,.handler=handle_logs};
    httpd_uri_t pause={.uri="/pause",.method=HTTP_POST,.handler=handle_pause};
    httpd_uri_t resume={.uri="/resume",.method=HTTP_POST,.handler=handle_resume};
    httpd_uri_t schedule_get={.uri="/schedule",.method=HTTP_GET,.handler=handle_schedule};
    httpd_uri_t schedule_post={.uri="/schedule",.method=HTTP_POST,.handler=handle_schedule};
    httpd_uri_t wifi_reset={.uri="/wifi-reset",.method=HTTP_POST,.handler=handle_wifi_reset};
    httpd_register_uri_handler(s_server,&wifi_reset);
    httpd_register_uri_handler(s_server,&schedule_get);
    httpd_register_uri_handler(s_server,&schedule_post);
    httpd_register_uri_handler(s_server,&root);
    httpd_register_uri_handler(s_server,&save);
    httpd_register_uri_handler(s_server,&reload);
    httpd_register_uri_handler(s_server,&ota);
    httpd_register_uri_handler(s_server,&logs);
    httpd_register_uri_handler(s_server,&pause);
    httpd_register_uri_handler(s_server,&resume);
    ESP_LOGI(TAG,"Always-on settings page at http://<device-ip>/ (see \"sta ip:\" above for the address)");
    return ESP_OK;
}

void nf_webui_stop(void) {
    if (s_server) { httpd_stop(s_server); s_server=NULL; }
}
