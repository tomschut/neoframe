#include "portal.h"
#include "formutil.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG="neoframe-portal";
#define CONNECTED_BIT 1
#define DONE_BIT 1
#define AP_PASSWORD "1234567890"

static EventGroupHandle_t s_wifi_events, s_done_events;
static nf_config s_base, s_candidate, s_result;
static bool s_have_candidate;
static char s_error[160];
static uint32_t s_ap_ip; /* network byte order */
static volatile bool s_dns_stop;
static SemaphoreHandle_t s_dns_done;
static esp_netif_t *s_ap_netif; /* created once, never destroyed - see nf_portal_provision */

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)data;
    if (base==IP_EVENT && id==IP_EVENT_STA_GOT_IP) xEventGroupSetBits(s_wifi_events,CONNECTED_BIT);
}

static bool wifi_verify(const char *ssid, const char *pass, int timeout_ms) {
    wifi_config_t w={0};
    memcpy(w.sta.ssid,ssid,strlen(ssid));
    memcpy(w.sta.password,pass,strlen(pass));
    esp_wifi_disconnect();
    xEventGroupClearBits(s_wifi_events,CONNECTED_BIT);
    if (esp_wifi_set_config(WIFI_IF_STA,&w)!=ESP_OK) return false;
    if (esp_wifi_connect()!=ESP_OK) return false;
    EventBits_t bits=xEventGroupWaitBits(s_wifi_events,CONNECTED_BIT,pdFALSE,pdFALSE,pdMS_TO_TICKS(timeout_ms));
    return bits & CONNECTED_BIT;
}

/* GD's example resolves to /, /submit and every captive-portal probe path;
 * only these two builders (not the vendor panel/HTTP code) are new here. */
static esp_err_t send_form(httpd_req_t *req) {
    const nf_config *pre=s_have_candidate?&s_candidate:&s_base;
    char ssid[96], url[1024];
    nf_html_escape(pre->wifi_ssid,ssid,sizeof(ssid));
    nf_html_escape(pre->image_url,url,sizeof(url));
    char *html=malloc(4096);
    if (!html) return httpd_resp_send_500(req);
    int n=snprintf(html,4096,
        "<!doctype html><title>NeoFrame setup</title>"
        "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<style>body{font-family:sans-serif;max-width:420px;margin:24px auto;padding:0 16px}"
        "label{display:block;margin-top:12px;font-weight:600}"
        "input{width:100%%;padding:8px;margin-top:4px;box-sizing:border-box;font-size:16px}"
        "button{margin-top:20px;width:100%%;padding:12px;font-size:16px}"
        ".err{background:#fee;border:1px solid #c00;padding:8px;border-radius:4px;color:#900}</style>"
        "<h2>NeoFrame setup</h2>"
        "%s%s%s"
        "<form method=post action=/submit>"
        "<label>WiFi network name</label><input name=ssid maxlength=32 required value=\"%s\">"
        "<label>WiFi password (blank = open network)</label><input name=pass type=password maxlength=64>"
        "<label>Image URL</label><input name=image_url type=url maxlength=500 required value=\"%s\">"
        "<label>Refresh every (minutes)</label><input name=interval type=number min=3 max=1440 required value=\"%d\">"
        "<label>Active from</label><input name=start type=time required value=\"%s\">"
        "<label>Active until</label><input name=stop type=time required value=\"%s\">"
        "<button type=submit>Verify &amp; Save</button></form>",
        *s_error?"<div class=err>":"", s_error, *s_error?"</div>":"",
        ssid, url, (int)(pre->update_interval_s/60), pre->active_start, pre->active_end);
    httpd_resp_set_type(req,"text/html");
    httpd_resp_send(req,html,n);
    free(html);
    return ESP_OK;
}

static esp_err_t handle_root(httpd_req_t *req) { return send_form(req); }

static esp_err_t handle_submit(httpd_req_t *req) {
    int total=req->content_len;
    if (total<=0 || total>=2048) return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Form too large");
    char *body=malloc(total+1);
    if (!body) return httpd_resp_send_500(req);
    int received=0;
    while (received<total) {
        int r=httpd_req_recv(req,body+received,total-received);
        if (r<=0) { free(body); return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Read failed"); }
        received+=r;
    }
    body[received]=0;

    char ssid[33]={0}, pass[65]={0}, image_url[512]={0}, start[6]={0}, stop[6]={0}, interval[16]={0};
    nf_form_field(body,"ssid",ssid,sizeof(ssid));
    nf_form_field(body,"pass",pass,sizeof(pass));
    nf_form_field(body,"image_url",image_url,sizeof(image_url));
    nf_form_field(body,"interval",interval,sizeof(interval));
    nf_form_field(body,"start",start,sizeof(start));
    nf_form_field(body,"stop",stop,sizeof(stop));
    free(body);

    nf_config candidate=s_base;
    candidate.version=1;
    strncpy(candidate.wifi_ssid,ssid,sizeof(candidate.wifi_ssid)-1);
    strncpy(candidate.wifi_pass,pass,sizeof(candidate.wifi_pass)-1);
    strncpy(candidate.image_url,image_url,sizeof(candidate.image_url)-1);
    strncpy(candidate.active_start,start,sizeof(candidate.active_start)-1);
    strncpy(candidate.active_end,stop,sizeof(candidate.active_end)-1);
    long minutes=strtol(interval,NULL,10);
    candidate.update_interval_s=(minutes>0)?(uint32_t)(minutes*60):0;

    s_candidate=candidate; s_have_candidate=true;

    if (!nf_config_valid(&candidate)) {
        snprintf(s_error,sizeof(s_error),"Please check the values below - one or more fields is invalid.");
        return send_form(req);
    }

    /* Respond now, before wifi_verify() attempts a STA connection: joining
     * another network while running as an AP often forces this SoftAP to
     * follow the target's channel, which can drop the client's connection
     * to this very request. Answering first means the browser always gets
     * something even if the link dies a moment later; the outcome (success
     * or the error banner) is picked up on the next page load instead. */
    httpd_resp_set_type(req,"text/html");
    httpd_resp_sendstr(req,
        "<!doctype html><title>NeoFrame</title>"
        "<body style=\"font-family:sans-serif;max-width:420px;margin:40px auto;text-align:center\">"
        "<h2>Connecting&hellip;</h2><p>Verifying WiFi now - this may disconnect you "
        "from this setup network. Reconnect and reload this page in about 20 seconds "
        "if the frame isn't online yet.</p>");

    ESP_LOGI(TAG,"Verifying \"%s\"...",candidate.wifi_ssid);
    if (!wifi_verify(candidate.wifi_ssid,candidate.wifi_pass,15000)) {
        snprintf(s_error,sizeof(s_error),"Could not connect to \"%s\". Check the network name and password.",candidate.wifi_ssid);
        return ESP_OK;
    }

    *s_error=0;
    s_result=candidate;
    xEventGroupSetBits(s_done_events,DONE_BIT);
    return ESP_OK;
}

static esp_err_t handle_redirect(httpd_req_t *req) {
    char location[24];
    snprintf(location,sizeof(location),"http://%u.%u.%u.%u/",
        s_ap_ip&0xFF,(s_ap_ip>>8)&0xFF,(s_ap_ip>>16)&0xFF,(s_ap_ip>>24)&0xFF);
    httpd_resp_set_status(req,"302 Found");
    httpd_resp_set_hdr(req,"Location",location);
    httpd_resp_send(req,NULL,0);
    return ESP_OK;
}

static void dns_task(void *arg) {
    (void)arg;
    int sock=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
    if (sock<0) { xSemaphoreGive(s_dns_done); vTaskDelete(NULL); return; }
    struct timeval tv={.tv_sec=1,.tv_usec=0};
    setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(53),.sin_addr.s_addr=INADDR_ANY};
    if (bind(sock,(struct sockaddr *)&addr,sizeof(addr))<0) {
        close(sock); xSemaphoreGive(s_dns_done); vTaskDelete(NULL); return;
    }
    uint8_t buf[512];
    while (!s_dns_stop) {
        struct sockaddr_in from; socklen_t fromlen=sizeof(from);
        int n=recvfrom(sock,buf,sizeof(buf),0,(struct sockaddr *)&from,&fromlen);
        if (n<12) continue; /* timeout, error, or too short to be a DNS query */
        buf[2]=0x81; buf[3]=0x80;      /* standard response, recursion available */
        buf[6]=0; buf[7]=1;            /* ANCOUNT = 1 */
        buf[8]=0; buf[9]=0; buf[10]=0; buf[11]=0; /* NSCOUNT, ARCOUNT = 0 */
        if (n+16>(int)sizeof(buf)) continue;
        uint8_t *ans=buf+n;
        ans[0]=0xC0; ans[1]=0x0C;      /* name: pointer to the question at offset 12 */
        ans[2]=0x00; ans[3]=0x01;      /* TYPE A */
        ans[4]=0x00; ans[5]=0x01;      /* CLASS IN */
        ans[6]=ans[7]=ans[8]=0; ans[9]=60; /* TTL 60s */
        ans[10]=0x00; ans[11]=0x04;    /* RDLENGTH 4 */
        memcpy(ans+12,&s_ap_ip,4);
        sendto(sock,buf,n+16,0,(struct sockaddr *)&from,fromlen);
    }
    close(sock);
    xSemaphoreGive(s_dns_done);
    vTaskDelete(NULL);
}

esp_err_t nf_portal_provision(const nf_config *base, nf_config *out, QueueHandle_t serial_messages) {
    s_base=*base; s_have_candidate=false; *s_error=0; s_dns_stop=false;
    s_wifi_events=xEventGroupCreate();
    s_done_events=xEventGroupCreate();
    if (!s_wifi_events || !s_done_events) {
        if (s_wifi_events) vEventGroupDelete(s_wifi_events);
        if (s_done_events) vEventGroupDelete(s_done_events);
        return ESP_ERR_NO_MEM;
    }
    esp_event_handler_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event,NULL);

    /* Created once and never destroyed: destroying and recreating this netif
     * on every portal entry raced against LWIP's own async cleanup of
     * things that still referenced it (its DHCP server, the just-stopped
     * httpd's TCP connections) - a double-free in lwip's memp pool
     * (tlsf_free assert) that persisted even after fixing the dns_task
     * race. A netif that's simply never freed can't be freed twice. */
    if (!s_ap_netif) s_ap_netif=esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) {
        esp_event_handler_unregister(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event);
        vEventGroupDelete(s_wifi_events); vEventGroupDelete(s_done_events);
        return ESP_FAIL;
    }
    esp_netif_t *ap_netif=s_ap_netif;

    uint8_t mac[6]={0}; esp_wifi_get_mac(WIFI_IF_AP,mac);
    char ap_ssid[24];
    snprintf(ap_ssid,sizeof(ap_ssid),"NeoFrame-%02X%02X%02X",mac[3],mac[4],mac[5]);
    wifi_config_t ap_config={0};
    strncpy((char *)ap_config.ap.ssid,ap_ssid,sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len=strlen(ap_ssid);
    strncpy((char *)ap_config.ap.password,AP_PASSWORD,sizeof(ap_config.ap.password)-1);
    ap_config.ap.authmode=WIFI_AUTH_WPA2_PSK;
    ap_config.ap.max_connection=4;
    ap_config.ap.channel=1;

    if (esp_wifi_set_mode(WIFI_MODE_APSTA)!=ESP_OK || esp_wifi_set_config(WIFI_IF_AP,&ap_config)!=ESP_OK) {
        esp_event_handler_unregister(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event);
        vEventGroupDelete(s_wifi_events); vEventGroupDelete(s_done_events);
        return ESP_FAIL;
    }

    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(ap_netif,&ip_info);
    s_ap_ip=ip_info.ip.addr;

    s_dns_done=xSemaphoreCreateBinary();
    bool dns_running=s_dns_done && xTaskCreate(dns_task,"portal_dns",3072,NULL,3,NULL)==pdPASS;

    httpd_handle_t server=NULL;
    httpd_config_t http_cfg=HTTPD_DEFAULT_CONFIG();
    http_cfg.uri_match_fn=httpd_uri_match_wildcard;
    http_cfg.stack_size=8192;
    esp_err_t e=httpd_start(&server,&http_cfg);
    if (e==ESP_OK) {
        httpd_uri_t root={.uri="/",.method=HTTP_GET,.handler=handle_root};
        httpd_uri_t submit={.uri="/submit",.method=HTTP_POST,.handler=handle_submit};
        httpd_uri_t any={.uri="/*",.method=HTTP_GET,.handler=handle_redirect};
        httpd_register_uri_handler(server,&root);
        httpd_register_uri_handler(server,&submit);
        httpd_register_uri_handler(server,&any);
        ESP_LOGI(TAG,"Captive portal \"%s\" (password " AP_PASSWORD ") active at http://%u.%u.%u.%u/",
            ap_ssid,s_ap_ip&0xFF,(s_ap_ip>>8)&0xFF,(s_ap_ip>>16)&0xFF,(s_ap_ip>>24)&0xFF);
        for (;;) {
            if (xEventGroupWaitBits(s_done_events,DONE_BIT,pdFALSE,pdFALSE,pdMS_TO_TICKS(200)) & DONE_BIT) break;
            char *line;
            if (serial_messages && xQueueReceive(serial_messages,&line,0)==pdTRUE) {
                nf_config candidate;
                bool ok=nf_config_parse(line,&s_base,&candidate,false);
                free(line);
                if (ok) { s_result=candidate; ESP_LOGI(TAG,"Provisioned over serial instead"); break; }
                ESP_LOGW(TAG,"Invalid serial configuration; captive portal still active");
            }
        }
        httpd_stop(server);
    } else {
        ESP_LOGE(TAG,"Captive portal HTTP server failed to start: %s",esp_err_to_name(e));
    }

    s_dns_stop=true;
    /* Must actually wait for dns_task to close its socket before tearing
     * down the AP netif/mode below - a UDP packet still in flight through
     * LWIP when the interface it belongs to gets destroyed underneath it
     * caused a double-free in lwip's memp pool (tlsf_free assert). The
     * timeout is just a safety net in case the task never started cleanly;
     * if it fires, deliberately leak the semaphore rather than risk the
     * (still-running) task later signalling one we already deleted. */
    bool dns_finished=!dns_running || xSemaphoreTake(s_dns_done,pdMS_TO_TICKS(2000))==pdTRUE;
    if (dns_finished && s_dns_done) { vSemaphoreDelete(s_dns_done); s_dns_done=NULL; }
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_event_handler_unregister(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_event);
    vEventGroupDelete(s_wifi_events); vEventGroupDelete(s_done_events);

    if (e!=ESP_OK) return e;
    *out=s_result;
    return ESP_OK;
}
