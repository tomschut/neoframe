# NeoFrame firmware (C, ESP-IDF 4.4.4)

P0 firmware for Good Display ESP32-133C02 / GDEP133C02. The source implements
persistent WiFi STA, native packed-image polling and independent optional settings
polling. It has been compiled and flashed to real ESP32-S3 hardware; a full factory
flash dump was taken and its restore verified (write-back + hash match) before any
custom flash, per the project's backup gate. Physical panel color/orientation
fidelity against the GD reference image is still pending a side-by-side comparison.
The original project instructions are retained unchanged.

## Build and test

With ESP-IDF **4.4.4** activated:

```sh
idf.py build
sh tools/test.sh
```

Or from this project directory with Podman:

```sh
podman run --rm --userns=keep-id -v "$PWD:/project:Z" -w /project docker.io/espressif/idf:v4.4.4 idf.py build
podman run --rm --userns=keep-id -v "$PWD:/project:Z" -w /project docker.io/espressif/idf:v4.4.4 sh tools/test.sh
```

Output: `build/neoframe.bin`, `build/bootloader/bootloader.bin`,
`build/partition_table/partition-table.bin`, `build/ota_data_initial.bin`, plus
`build/flasher_args.json` (has the exact offsets/flags to use). These are separate
application, bootloader and partition artifacts, not a full factory-recovery image.
The build does not flash hardware.

16 MB flash and octal PSRAM settings come from the GD example's `sdkconfig` and
module declaration. Verify that the actual board matches before using these images.
The custom partition layout (`partitions.csv`) is OTA-capable: NVS at 0x9000, PHY at
0xf000, `otadata` at 0x10000, and two 1.875 MB app slots (`ota_0`/`ota_1`) at 0x20000
and 0x200000. This is **not the factory image-slot layout**, and flashing it replaces
the factory partition table entirely - the same backup gate applies. Moving from an
already-flashed single-app build to this OTA-capable table needs one more full
wipe+reflash (bootloader + partition table + app); after that, updates can go over
the air (see "Firmware updates (OTA)" below).

## Initial configuration and recovery

On first boot (no WiFi credentials stored), the device opens a **WiFi access
point** named `NeoFrame-XXXXXX` (last 3 MAC bytes), password `1234567890`,
and a captive portal. Connect a phone or laptop to it; most OSes pop the setup page
automatically, or open `http://192.168.4.1/` manually. The page collects
WiFi network name/password, image URL, refresh interval (minutes), and the
active-window start/end times. Submitting the form live-tests the WiFi
credentials (up to 15s) before saving anything — a wrong password redisplays
the form with an error rather than persisting bad credentials. On success the
portal closes, the AP shuts down, and the device proceeds as a normal WiFi
client. The portal starts when `wifi_ssid` is empty in NVS, or when a
saved WiFi network fails to connect **10 consecutive attempts** in a row
(backoff-capped at 60s/attempt, so roughly 5 minutes of retrying first) —
either way the credentials on file are left untouched unless the portal
succeeds, so a transient outage doesn't erase working config, and the
always-on settings page (below) is briefly stopped and restarted around
this so both HTTP servers don't fight over port 80.

## Always-on settings page

Once connected to WiFi, the device also serves a persistent settings page at
`http://<device-ip>/` (port 80; the IP is logged as `sta ip:` at connect
time, or check your router's DHCP client list). It exposes `image_url`,
`config_url`, `firmware_url`, refresh interval, active window, `led_enabled`,
and `power_profile` — everything the remote `config_url` path can already
change, plus `firmware_url` on top — plus a "Force reload now" button that
bypasses the poll interval immediately, and a "Check & install firmware
update now" button (see "Firmware updates (OTA)" below). **It never exposes
WiFi credentials** — changing which network the device joins is still
restricted to the AP-mode portal (needs the WPA2 password) or serial (needs
a USB cable). `firmware_url` is deliberately not held to that same
restriction: once mounted, this page is realistically the only thing you can
reach, so `firmware_url` is settable here too — it has no authentication
beyond being on your LAN at all, so anyone on your network can point the
device at a firmware image of their choosing and trigger an install. It also
links to `/logs` (recent log output, plain text).

The same effect as "Force reload now" is available over serial at any time:
send the literal line `force` (not JSON) to bypass both the poll interval
and the render-spacing guard on the next loop tick. Sending `ota` likewise
triggers an immediate OTA check against the configured `firmware_url`.

Serial configuration (below) remains available at all times, including
while the portal is up: a valid serial JSON line immediately satisfies
provisioning and closes the portal, whichever arrives first.

P0 also accepts USB serial configuration at **115200 baud**, one JSON object per line.
Connect a serial terminal without local echo and send (replace the example values):

```json
{"wifi_ssid":"Your 2.4GHz network","wifi_pass":"your-password","image_url":"http://192.168.1.10:8000/frame","config_url":""}
```

Wait for `Configuration saved` or `Configuration already saved`. Settings are
committed before use. They survive reset/power loss in namespace `neoframe`.
Serial configuration remains available with missing credentials, a wrong WiFi
password, failed settings service or failed panel refresh. A later JSON line can
change just the local fields you need. Empty `config_url` disables remote settings;
empty `wifi_pass` selects an open network. Credentials are not logged by this app.
The serial terminal itself must also have local echo/logging disabled if desired.

The complete versioned configuration is one NVS blob under key `config` rather
than separate keys. This makes configuration replacement atomic at the NVS blob
level and accommodates `update_interval_s`, whose 17-character name exceeds
NVS's 15-character key limit. Existing stock firmware credentials are not imported.
Malformed/unknown-version blobs use safe defaults; NVS initialization errors are
reported without automatically erasing the partition.

## Firmware updates (OTA)

Set `firmware_url` over serial or the always-on settings page (never through
the remote `config_url` path - the AP-mode portal's form doesn't currently
have a field for it either, only used for first-time setup) to an
`http://` or `https://` URL serving an ESP-IDF app image. Trigger a check either by
sending the literal serial line `ota`, or via "Check & install firmware
update now" on the always-on settings page (which only ever fetches the URL
already on file). The check compares the fetched image's embedded version string against
the running one (`esp_https_ota_get_img_desc`) and skips the flash/reboot
entirely if they match, so re-checking an unchanged URL is cheap. On an
actual update it writes to the inactive OTA slot and reboots into it
immediately on success.

Rollback safety: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is on, and a freshly
flashed OTA image stays in `ESP_OTA_IMG_PENDING_VERIFY` until the device
successfully connects to WiFi at least once (`nf_ota_confirm_healthy`) — if
it never gets that far (crash-loops, bad build), the bootloader automatically
reverts to the previous slot on the next boot. There is currently no
automatic/periodic OTA check; it is manual-trigger-only in this build.

## Logging

`ESP_LOG` output is also captured into a 16 KB RAM ring buffer (ANSI color
codes stripped) from boot, in addition to the normal serial console — nothing
is redirected away from serial. View it at `/logs` on the always-on settings
page (plain text). The buffer is not persisted across reboots.

## Pause / resume

`/pause` and `/resume` (POST, no body) on the always-on settings page, or the
serial lines `pause`/`resume`, toggle a runtime-only state (not persisted;
every boot starts active) intended for an external presence trigger (e.g.
Home Assistant calling these over the LAN) rather than manual day-to-day use.
Pausing: de-asserts `SW_C` (the e-paper analog boost supply — the same rail a
failed render already de-asserts; harmless since e-ink holds its image with
zero power either way), skips the image-poll loop entirely, and enables
automatic light sleep (`esp_pm_configure`, CPU parked at a fixed 160MHz
during idle — no dynamic frequency scaling, kept off deliberately since DFS
combined with octal PSRAM at 80MHz has timing edge cases on some ESP32-S3
configs that haven't been validated here) plus WiFi modem power-save
(`WIFI_PS_MIN_MODEM`). **WiFi stays associated and the settings page stays
reachable** — confirmed on hardware (98ms response while paused) — this is
why light sleep was used instead of deep sleep, at the cost of much smaller
power savings than deep sleep would give. Resuming reverses all of this and
immediately triggers a fresh poll, so the panel updates right away rather
than waiting out whatever was left of the interval.

## Image contract

HTTP(S) 200 must contain exactly **960000 bytes**, with no image-file header:
GD's `pic_display_test()` layout, 1600 rows × 600 bytes. Each row's first 300 bytes
go to controller M, then the second 300 bytes to S in the second transfer pass.
Each nibble must be one of `0,1,2,3,5,6` (black, white, yellow, red, blue, green).
This follows the supplied native GD example; landscape orientation and compatibility
with the stock `/upload` format still require a physical reference-image comparison.
The device always expects this native layout - the server is responsible for
serving an already-correctly-oriented frame; there is no on-device rotation.

The entire response is validated before touching the panel. ETag is preferred over
Last-Modified. Validators are adopted only after successful rendering and are tied
to the image URL. They intentionally remain in RAM: after reboot the first fetch
is unconditional, avoiding an incorrect 304 when no cached frame exists.
Redirects, compressed responses, invalid pixels, truncation and oversize are rejected.
HTTPS validates the server certificate using the IDF CA bundle and needs correct
time; SNTP starts with WiFi and the image loop waits for valid time for HTTPS.

Two PSRAM buffers preserve the last successful frame during a failed download.
Refresh attempts are at least 180 seconds apart, with a 180-second boot cooldown
to protect against repeated power cycling. The always-on loop attempts a daily
refresh even on unchanged content and can re-render the RAM cache offline.
This is best effort: a powered-off board, missing initial image or broken panel
cannot guarantee a daily refresh.

## Optional remote settings

Set `config_url` through serial. The remote endpoint must return the complete
documented contract; missing required fields reject the entire update:

```json
{"update_interval_s":600,"active_start":"08:00","active_end":"00:00","image_url":"http://192.168.1.10:8000/frame","led_enabled":false}
```

Interval must be an integer from 180 to 86400 seconds. Times must be valid HH:MM.
Duplicate keys, unknown fields, wrong types, overlong strings, malformed/partial JSON,
HTTP errors and oversized JSON are ignored. WiFi credentials and `config_url` cannot
be changed remotely. Optional `power_profile` accepts `low_power`, `always_on`, or
`ac_power` (see "Scheduled deep sleep" below).

Settings fetches run in a separate lower-priority task after an image cycle, with
a 4096-byte cap, a 2-second socket timeout and a 4-second body deadline. DNS/connect
latency is bounded by the underlying IDF network stack and is not a strict total
4-second deadline. The image task never waits for that task or holds a shared lock.
A one-slot result queue delivers settings for the next image cycle; results based
on an older configuration generation are discarded. Invalid settings never alter
the image schedule. Slow settings may share network bandwidth, but are not on the
image loop's control path. Like `image_url`, `config_url` fetches send a
conditional GET (ETag preferred over Last-Modified) once a prior fetch has
succeeded; the validator is adopted only after the response is applied and is
reset whenever `config_url` itself changes.

## Deferred P1 features

LED control, battery ADC, and compatibility endpoints remain deferred until
P0 is proven on hardware, as required by the project. `led_enabled` is
validated, stored, and settable through the captive portal or serial/remote
config, but has no runtime behavior yet.

Everything else originally deferred here has since shipped ahead of that gate
at the user's explicit request: OTA (manual-trigger only, see above),
pause/resume (automatic *light* sleep, WiFi stays associated - see
"Pause / resume" above), and now true timer-based deep sleep with
active-window/cron-style schedule enforcement and panel power-rail gating
across sleep, selected via `power_profile: low_power` (see
"Scheduled deep sleep" below). `active_start`/`active_end` remain supported
as the legacy single-window form when no `schedule` array is configured.

The panel performs the GD POF command after refresh. BUSY waits have a 120-second
limit; on a panel error, SW_C is deasserted to prevent leaving the boost supply on.
SPI uses GPIO41/40 and CLK9; manual chip selects 18/17; RESET6; BUSY7; SW_C45.
No unknown GPIO is driven. See `documentation/LED-FINDINGS.md` for the LED question.

## Hardware acceptance

Start the included test server on a machine reachable by the ESP32:

```sh
python3 tools/serve.py --bind 0.0.0.0 --image-url http://192.168.1.10:8000/frame
```

By default it serves six native color bars. To compare the exact GD sample image,
add `--reference` followed by the quoted path to the supplied `main/image.h`.
To serve your own already-packed image, use `--frame path/to/frame.bin`.

1. Verify the available restore binary and board identity before a later manual flash.
2. Erase NVS or use a blank device; confirm the `NeoFrame-XXXXXX` AP appears and the
   captive portal page loads. Submit a wrong password and confirm it's rejected with
   an error and nothing is saved; then submit correct values and confirm it saves,
   the AP closes, and the device comes up as a WiFi client. Separately, confirm a
   serial JSON line sent while the portal is still open provisions immediately too.
   Wait through the initial 180-second refresh guard.
3. Compare color bars and the GD reference image, checking both halves and orientation.
4. Reboot and power-cycle; confirm STA reconnects with the saved credentials.
5. Check `/frame` returns 200 then 304; change source content and restart server for a new ETag.
6. Set `config_url` successively to `/config`, `/config/garbage`, `/config/partial`,
   `/config/500`, `/config/timeout`, an unreachable address and empty string.
   Confirm image cycles continue and rejected settings do not change saved configuration.
7. Set the image URL to `/frame/truncated`, `/frame/oversized`, `/frame/invalid`.
   Confirm no invalid frame is rendered, then restore `/frame`.
8. Disconnect WiFi and test reconnection. Verify refresh spacing and daily cached refresh.

Host sanitizer tests exercise the production C configuration parser, storage adapter,
pixel/geometry helpers and HTTP adapter with simulated NVS/network failures. They do
not emulate the radio, real NVS power loss, FreeRTOS scheduling or panel electronics.

## Scheduled deep sleep

Choose “Edit sleep schedule JSON” on the settings page to paste a schema.
Set Settings URL on the main page to your server's JSON endpoint for automatic
updates. The remote response can be:

```json
{
  "timezone": "Europe/Amsterdam",
  "power_profile": "low_power",
  "paused": false,
  "image_url": "http://192.168.2.26:8084/api/current_frame",
  "schedule": [
    {"days": "mon-fri", "start": "08:00", "stop": "22:00", "every": "5m"},
    {"days": "sat-sun", "start": "10:00", "stop": "23:00", "every": "10m"}
  ]
}
```

Up to eight windows. Days accept `daily`, a lowercase weekday (`mon` … `sun`),
or an inclusive range (`mon-fri`, `fri-mon`). Intervals accept whole minutes or
hours (`5m`, `1h`), from 3 minutes to 24 hours. End times are exclusive.
Overnight windows belong to their starting day; equal start/end means 24 hours.
Overlapping windows combine their wake times. Europe/Amsterdam and UTC are
supported named timezones; other zones require a POSIX TZ rule. DST is handled
when finding weekly wake times. This is a window schedule, not cron syntax.

In low_power the device wakes, connects, fetches settings first, commits valid
changes to NVS, optionally renders, and deep sleeps. Outside every window it
sleeps until the next opening. ETag is preferred over Last-Modified; validators
survive timer deep sleep. Invalid responses retain saved settings. Unchanged
settings do not write NVS. Image requests are unconditional after sleep because
the PSRAM image cache is lost.

Home Assistant changes `paused` on your **settings server**, preserving the rest
of the schema and updating its ETag/Last-Modified. Paused devices skip rendering
but still wake at scheduled check times to discover resume. Overnight changes
are read at the next window opening. The device cannot receive requests during
deep sleep; the last picture remains visible. Set `power_profile` to `always_on`
in the remote response to regain persistent local web access on the next wake.
Local POST /schedule accepts the same JSON and queues it for validation/storage.

A third `power_profile`, `ac_power`, is for a frame that's permanently mains-powered
and has no reason to sleep or save power, but should still follow the same
`schedule`/`paused` behavior as a battery-powered (`low_power`) frame - for
example only showing the photo during the day, or letting Home Assistant pause
it, without ever losing settings-page/WiFi access to do so. It evaluates the
same schedule/`paused` gate as `low_power` to decide whether to render, but
never sleeps and never stops the settings page or WiFi: the device stays fully
reachable at all times, and `config_url`/serial changes keep applying
immediately, same as `always_on`. It's also useful for validating a `schedule`
and `timezone` against the real clock and windows before committing to
`low_power`, where a mistake is only visible/fixable at the next wake. In
`ac_power`, polling cadence outside the render gate still follows the schedule
(the per-window `every` interval, or time until the next window opens) rather
than `update_interval_s`.

The old daily active_start/active_end/update_interval_s contract is also accepted
when no schedule has been configured. Existing v1 configs migrate with credentials
preserved and always_on selected, since low_power previously did not deep sleep.
Cold boots retain the 180-second guard; timer wakes use retained last-render time.
Network/time acquisition is bounded; without a valid clock, rendering is skipped
and retried after update_interval_s (600 seconds by default). The panel rail is
held off during sleep. Actual power consumption and wake reliability still need
hardware verification. Host tests cover parser and schedule behavior.
