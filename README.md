| Supported Targets | ESP32-P4 |
| ----------------- | -------- |

# Blynk MQTT Example (Waveshare ESP32-P4, Wi-Fi via ESP32-C6)

This example connects an ESP32-P4 to [Blynk Cloud](https://blynk.io) over MQTT (TLS). It demonstrates the basic Blynk MQTT API: publishing device info on connect, subscribing to datastream downlinks, and publishing a datastream update.

## Hardware

The ESP32-P4 has no native Wi-Fi/BT radio, so Wi-Fi is provided by the on-board ESP32-C6 co-processor over SDIO, using the `esp_hosted` + `esp_wifi_remote` managed components (already configured in `sdkconfig.defaults`/`sdkconfig` for the Waveshare ESP32-P4 board — no extra wiring/config needed beyond flashing the C6's `esp_hosted` slave firmware if it isn't already on the board).

Early P4 boards ship silicon revision <v3.0 (e.g. v1.3), while ESP-IDF defaults to requiring rev ≥v3.1. If flashing fails with `requires chip revision in range [v3.1 - v3.99]`, this project's `sdkconfig.defaults` already sets `ESP32P4_SELECTS_REV_LESS_V3` + `ESP32P4_REV_MIN_100` (floor of v1.0) to support it — run `idf.py fullclean && idf.py build` after any chip-revision change, since it affects the bootloader image.

Rev <3.0 silicon also only has a 360MHz CPLL (no 400MHz path), so `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ` must be 360, not the usual 400MHz default — already set in `sdkconfig.defaults`. Leaving it at 400 on this silicon boots the bootloader fine but then hard-crashes immediately in the app with `assert failed: esp_clk_init clk.c:104`.

This board's flash chip is 16MB, and it uses a custom two-OTA-slot [partitions.csv](partitions.csv) (`ota_0`/`ota_1`, 2MB each) instead of the upstream example's single-app table, so `downlink/ota/json` has an `otadata`/`ota_x` layout to write into — `esp_https_ota()` has nowhere to write an update without it.

## Configure the project

Open the project configuration menu:

```
idf.py menuconfig
```

1. **Example Connection Configuration** — select "connect using WiFi interface" (`EXAMPLE_CONNECT_WIFI`, already the project default in `sdkconfig.defaults`) and set your Wi-Fi SSID/password. The connection goes out over the C6 co-processor automatically; no separate transport config is needed here — that's under "Component config → Wi-Fi Remote" (already set to `esp32c6` / SDIO).
2. **Blynk Configuration**:
   - `BLYNK_MQTT_BROKER_URI` — defaults to `mqtts://fra1.blynk.cloud:8883`. Change the region prefix (`fra1`, `lon1`, `sgp1`, ...) to match the datacenter your Blynk account/device lives in — check the device's "Device Info" page in Blynk.Console if unsure.
   - `BLYNK_TEMPLATE_ID` — the Template ID from your Blynk.Console template.
   - `BLYNK_AUTH_TOKEN` — the per-device Auth Token from Blynk.Console (Device Info page).
   - `FIRMWARE_VERSION` — bump before each release; reported in `info/mcu` and the OTA binary info tag's `mcu` field.
   - `BOARD` — hardware identifier embedded in the OTA binary info tag's `hw` field.

TLS server verification uses ESP-IDF's built-in certificate bundle (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`), so no certificate needs to be embedded for Blynk's broker.

## Build and Flash

```
idf.py -p PORT flash monitor
```

(To exit the serial monitor, type ``Ctrl-]``.)

## Blynk MQTT protocol notes

- Auth: username is always the literal string `device`; the password is the device's Blynk Auth Token.
- Publish device metadata on every clean connection to `info/mcu`: `{"tmpl":"...","ver":"...","build":"...","type":"...","rxbuff":1024}` — `rxbuff` reports the MQTT client's actual buffer size (`BLYNK_MQTT_BUFFER_SIZE` in `blynk_mqtt.c`), which caps the largest message the broker can send back.
- Subscribe to `downlink/ds/#` to receive datastream (virtual pin) updates pushed from the Blynk app/dashboard; the topic suffix after `downlink/ds/` is the datastream name.
- Publish datastream updates uplink to `ds/<name>`, e.g. `ds/V0` with the raw value as payload.

The example also handles these downlink control topics (all subscribed at QoS 1 on connect):

- `downlink/ping` — server liveness check; logged only, the QoS 1 PUBACK is the implicit reply.
- `downlink/reboot` — restarts the device via `esp_restart()`.
- `downlink/redirect` — reconnects the MQTT client to a new broker. Payload can be a full URI (`mqtts://host:port`) or JSON `{"host":"...","port":8883}`.
- `downlink/ota/json` — downloads and applies a firmware update via `esp_https_ota()`, then reboots. Payload: `{"url":"https://.../firmware.bin"}`.

`downlink/redirect` and `downlink/ota/json` use a minimal hand-rolled flat-JSON field lookup (no cJSON dependency) — it only understands single-level `"key":"value"`/`"key":123` pairs, which is all these control payloads contain.

The binary also embeds a [Blynk binary info tag](https://docs.blynk.io/en/blynk.cloud-mqtt-api/device-mqtt-api/ota#blynk-binary-info-tag) (`firmwareTag[]` in `blynk_mqtt.c`) — a null-separated `key\0value\0` blob starting with `"blnkinf"` that Blynk.Cloud scans out of the flashed image to identify the running firmware (`mcu`, `fw-type`, `build`, `blynk`, `hw`) for OTA version checks.

See https://docs.blynk.io for the full MQTT API reference.

## Example Output

```
I (3714) event: sta ip: 192.168.0.139, mask: 255.255.255.0, gw: 192.168.0.2
I (3964) MQTT_CLIENT: Sending MQTT CONNECT message, type: 1, id: 0000
I (4164) blynk_mqtt: MQTT_EVENT_CONNECTED
I (4174) blynk_mqtt: subscribed to downlink/ds/#, msg_id=17886
I (4174) blynk_mqtt: published device info, msg_id=41464
I (4314) blynk_mqtt: MQTT_EVENT_PUBLISHED, msg_id=41464
I (4484) blynk_mqtt: MQTT_EVENT_SUBSCRIBED, msg_id=17886
I (4484) blynk_mqtt: sent example datastream update to ds/V0, msg_id=0
```
