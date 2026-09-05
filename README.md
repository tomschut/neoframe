# NeoFrame firmware (C, ESP-IDF 4.4.4)

P0 firmware for Good Display ESP32-133C02 / GDEP133C02. The source implements
persistent WiFi STA, native packed-image polling and independent optional settings
polling. It has been compiled for ESP32-S3; physical panel operation, WiFi persistence
across real power loss and factory restore are **not yet hardware verified**.

The user explicitly waived the backup gate for software development on 2026-09-05,
reporting that a replacement binary is ready. No device was flashed or erased.
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
`build/partition_table/partition-table.bin`, plus `build/flasher_args.json`.
These are separate application, bootloader and partition artifacts, not a full
factory-recovery image. The build does not flash hardware.

16 MB flash and octal PSRAM settings come from the GD example's `sdkconfig` and
module declaration. Verify that the actual board matches before using these images.
The custom partition layout is IDF's `singleapp_large`: NVS at 0x9000, PHY at 0xf000,
application at 0x10000. This is **not the factory image-slot layout**, and flashing
it would replace the factory partition table. No OTA is implemented.

## Initial configuration and recovery

P0 uses USB serial configuration at **115200 baud**, one JSON object per line.
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

## Image contract

HTTP(S) 200 must contain exactly **960000 bytes**, with no image-file header:
GD's `pic_display_test()` layout, 1600 rows × 600 bytes. Each row's first 300 bytes
go to controller M, then the second 300 bytes to S in the second transfer pass.
Each nibble must be one of `0,1,2,3,5,6` (black, white, yellow, red, blue, green).
This follows the supplied native GD example; landscape orientation and compatibility
with the stock `/upload` format still require a physical reference-image comparison.

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
be changed remotely. Optional `power_profile` accepts `low_power` or `always_on`.

Settings fetches run in a separate lower-priority task after an image cycle, with
a 4096-byte cap, a 2-second socket timeout and a 4-second body deadline. DNS/connect
latency is bounded by the underlying IDF network stack and is not a strict total
4-second deadline. The image task never waits for that task or holds a shared lock.
A one-slot result queue delivers settings for the next image cycle; results based
on an older configuration generation are discarded. Invalid settings never alter
the image schedule. Slow settings may share network bandwidth, but are not on the
image loop's control path.

## Deferred P1 features

The current firmware uses awake FreeRTOS delays. Deep sleep, active-window
enforcement, captive portal, regular power-rail gating, LED control, battery ADC,
compatibility endpoints and OTA are deferred until P0 is proven on hardware, as
required by the project. `active_start`, `active_end`, `power_profile` and
`led_enabled` are validated and stored but have no P1 behavior yet.

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
2. Provision over serial; wait through the initial 180-second refresh guard.
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
