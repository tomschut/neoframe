#pragma once
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
/* Starts an always-on HTTP settings server (port 80) on whatever interface
 * is up - reachable on the LAN once STA has an IP. Lets a browser view/set
 * image_url, config_url, refresh interval, active window, led_enabled and
 * power_profile, and request an immediate refresh - the same fields the
 * remote config_url path can already touch, nothing more: WiFi credentials
 * are never exposed here. Changes are queued onto serial_messages (the same
 * queue serial_task feeds) so app_main's own loop remains the sole writer
 * of the live config, exactly like every other configuration path already
 * works. `config` must stay valid and live-updated for the device's uptime
 * (main.c's own copy) - only read here, never written. */
esp_err_t nf_webui_start(const nf_config *config, QueueHandle_t serial_messages, const bool *paused);
/* Stops the server (e.g. to free port 80 before the AP-mode captive portal
 * runs its own). Safe to call even if never started. */
void nf_webui_stop(void);
