#pragma once
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
/* Blocks running an open WiFi AP + captive-portal HTTP/DNS server until the
 * operator either submits a WiFi network that this device can actually join
 * (plus image URL / refresh interval / active window - live-verified over
 * WiFi), or sends a valid serial configuration line on serial_messages
 * (nullable; trusted as-is, same as the always-available serial path) -
 * whichever happens first. Writes the accepted configuration to *out
 * (not yet persisted - the caller saves it, same as the serial/remote path). */
esp_err_t nf_portal_provision(const nf_config *base, nf_config *out, QueueHandle_t serial_messages);
