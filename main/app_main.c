/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
/* Blynk MQTT Example
 *
 * Connects to Blynk Cloud over MQTT (TLS) and demonstrates the basic
 * Blynk MQTT API: device info on connect, subscribing to datastream
 * downlinks, and publishing a datastream update.
 *
 * See https://docs.blynk.io for the full MQTT API reference.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "protocol_examples_common.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "mqtt_client.h"

static const char *TAG = "blynk_mqtt";

/* Blynk always authenticates with the literal username "device";
 * the per-device auth token is sent as the password. */
#define BLYNK_MQTT_USERNAME "device"

/* MQTT send/receive buffer size; also reported to Blynk.Cloud as "rxbuff" in
 * info/mcu so the server knows the largest message it can send us. */
#define BLYNK_MQTT_BUFFER_SIZE 1024

/* Blynk binary info tag: scanned by Blynk.Cloud out of the flashed image to
 * identify running firmware for OTA version checks.
 * https://docs.blynk.io/en/blynk.cloud-mqtt-api/device-mqtt-api/ota#blynk-binary-info-tag */
#define BLYNK_PARAM_KV(k, v) k "\0" v "\0"

volatile const char firmwareTag[] __attribute__((used)) = "blnkinf\0"
    BLYNK_PARAM_KV("mcu", CONFIG_FIRMWARE_VERSION)
    BLYNK_PARAM_KV("fw-type", CONFIG_BLYNK_TEMPLATE_ID)
    BLYNK_PARAM_KV("build", __DATE__ " " __TIME__)
    BLYNK_PARAM_KV("blynk", "0.1.0")
    BLYNK_PARAM_KV("hw", CONFIG_BOARD)
    "\0";

#define BLYNK_TOPIC_INFO         "info/mcu"
#define BLYNK_TOPIC_DS_UPLINK    "ds/V0"
#define BLYNK_TOPIC_DS_DOWNLINK  "downlink/ds/#"
#define BLYNK_TOPIC_DS_PREFIX    "downlink/ds/"
#define BLYNK_TOPIC_PING         "downlink/ping"
#define BLYNK_TOPIC_REBOOT       "downlink/reboot"
#define BLYNK_TOPIC_REDIRECT     "downlink/redirect"
#define BLYNK_TOPIC_OTA          "downlink/ota/json"

/* Downlink control topics subscribed on every connect, alongside BLYNK_TOPIC_DS_DOWNLINK. */
static const struct {
    const char *topic;
    int qos;
} s_control_subs[] = {
    { BLYNK_TOPIC_DS_DOWNLINK, 1 },
    { BLYNK_TOPIC_PING, 1 }, /* Blynk always publishes this at QoS 1 */
    { BLYNK_TOPIC_REBOOT, 1 },
    { BLYNK_TOPIC_REDIRECT, 1 },
    { BLYNK_TOPIC_OTA, 1 },
};

/* Minimal flat-JSON field lookup; the control payloads above are small,
 * single-level objects (e.g. {"url":"...","size":123}), so a full JSON
 * parser isn't needed. */
static bool json_get_string(const char *json, const char *key, char *out, size_t out_size)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p || !(p = strchr(p + strlen(pattern), ':'))) {
        return false;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '"') {
        return false;
    }
    p++;
    const char *end = strchr(p, '"');
    if (!end) {
        return false;
    }
    size_t len = (size_t)(end - p);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, p, len);
    out[len] = '\0';
    return true;
}

static bool json_get_int(const char *json, const char *key, int *out)
{
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p || !(p = strchr(p + strlen(pattern), ':'))) {
        return false;
    }
    *out = atoi(p + 1);
    return true;
}

static void ota_task(void *pvParameter)
{
    char *url = pvParameter;
    ESP_LOGI(TAG, "starting OTA update from %s", url);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t err = esp_https_ota(&ota_cfg);
    free(url);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA succeeded, rebooting");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

static void handle_ota_request(const char *payload)
{
    char url[256];
    if (!json_get_string(payload, "url", url, sizeof(url))) {
        ESP_LOGW(TAG, "OTA payload missing 'url', ignoring: %s", payload);
        return;
    }
    char *url_copy = strdup(url);
    if (!url_copy) {
        ESP_LOGE(TAG, "OTA: out of memory");
        return;
    }
    if (xTaskCreate(ota_task, "blynk_ota", 8192, url_copy, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to create OTA task");
        free(url_copy);
    }
}

static void handle_redirect(esp_mqtt_client_handle_t client, const char *payload)
{
    char new_uri[160];

    if (strstr(payload, "://")) {
        /* Payload is already a full broker URI */
        strlcpy(new_uri, payload, sizeof(new_uri));
    } else {
        char host[96];
        int port = 8883;
        if (!json_get_string(payload, "host", host, sizeof(host))) {
            ESP_LOGW(TAG, "redirect payload missing 'host', ignoring: %s", payload);
            return;
        }
        json_get_int(payload, "port", &port);
        snprintf(new_uri, sizeof(new_uri), "mqtts://%s:%d", host, port);
    }

    ESP_LOGW(TAG, "server redirect to %s", new_uri);
    if (esp_mqtt_client_set_uri(client, new_uri) == ESP_OK) {
        esp_mqtt_client_reconnect(client);
    } else {
        ESP_LOGE(TAG, "invalid redirect URI: %s", new_uri);
    }
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32, base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");

        for (size_t i = 0; i < sizeof(s_control_subs) / sizeof(s_control_subs[0]); i++) {
            msg_id = esp_mqtt_client_subscribe(client, s_control_subs[i].topic, s_control_subs[i].qos);
            ESP_LOGI(TAG, "subscribed to %s, msg_id=%d", s_control_subs[i].topic, msg_id);
        }

        char info[256];
        int len = snprintf(info, sizeof(info),
                            "{\"tmpl\":\"%s\",\"ver\":\"%s\",\"build\":\"%s\",\"type\":\"%s\",\"rxbuff\":%d}",
                            CONFIG_BLYNK_TEMPLATE_ID, CONFIG_FIRMWARE_VERSION, __DATE__ " " __TIME__,
                            CONFIG_BLYNK_TEMPLATE_ID, BLYNK_MQTT_BUFFER_SIZE);
        msg_id = esp_mqtt_client_publish(client, BLYNK_TOPIC_INFO, info, len, 1, 0);
        ESP_LOGI(TAG, "published device info, msg_id=%d", msg_id);
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        /* Demo: push an example value to virtual pin V0 */
        msg_id = esp_mqtt_client_publish(client, BLYNK_TOPIC_DS_UPLINK, "0", 0, 1, 0);
        ESP_LOGI(TAG, "sent example datastream update to %s, msg_id=%d", BLYNK_TOPIC_DS_UPLINK, msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_DATA: {
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        printf("DATA=%.*s\r\n", event->data_len, event->data);

        /* Control topics carry small single-fragment JSON/text payloads;
         * copy into a null-terminated buffer for string parsing below. */
        char payload[512];
        bool have_payload = false;
        if (event->current_data_offset == 0 && event->data_len == event->total_data_len &&
            (size_t)event->data_len < sizeof(payload)) {
            memcpy(payload, event->data, event->data_len);
            payload[event->data_len] = '\0';
            have_payload = true;
        }

        if (event->topic_len == strlen(BLYNK_TOPIC_PING) &&
            strncmp(event->topic, BLYNK_TOPIC_PING, event->topic_len) == 0) {
            ESP_LOGI(TAG, "received server ping");
        } else if (event->topic_len == strlen(BLYNK_TOPIC_REBOOT) &&
                   strncmp(event->topic, BLYNK_TOPIC_REBOOT, event->topic_len) == 0) {
            ESP_LOGW(TAG, "reboot requested by server, restarting...");
            vTaskDelay(pdMS_TO_TICKS(200));
            esp_restart();
        } else if (event->topic_len == strlen(BLYNK_TOPIC_REDIRECT) &&
                   strncmp(event->topic, BLYNK_TOPIC_REDIRECT, event->topic_len) == 0) {
            if (have_payload) {
                handle_redirect(client, payload);
            } else {
                ESP_LOGW(TAG, "redirect payload missing/too large, ignoring");
            }
        } else if (event->topic_len == strlen(BLYNK_TOPIC_OTA) &&
                   strncmp(event->topic, BLYNK_TOPIC_OTA, event->topic_len) == 0) {
            if (have_payload) {
                handle_ota_request(payload);
            } else {
                ESP_LOGW(TAG, "OTA payload missing/too large, ignoring");
            }
        } else if (event->topic_len > strlen(BLYNK_TOPIC_DS_PREFIX) &&
                   strncmp(event->topic, BLYNK_TOPIC_DS_PREFIX, strlen(BLYNK_TOPIC_DS_PREFIX)) == 0) {
            const char *pin_name = event->topic + strlen(BLYNK_TOPIC_DS_PREFIX);
            int pin_name_len = event->topic_len - strlen(BLYNK_TOPIC_DS_PREFIX);
            ESP_LOGI(TAG, "datastream update: %.*s = %.*s", pin_name_len, pin_name, event->data_len, event->data);
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            ESP_LOGI(TAG, "Last error code reported from esp-tls: 0x%x", event->error_handle->esp_tls_last_esp_err);
            ESP_LOGI(TAG, "Last tls stack error number: 0x%x", event->error_handle->esp_tls_stack_err);
            ESP_LOGI(TAG, "Last captured errno : %d (%s)", event->error_handle->esp_transport_sock_errno,
                     strerror(event->error_handle->esp_transport_sock_errno));
        } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGI(TAG, "Connection refused error: 0x%x", event->error_handle->connect_return_code);
        } else {
            ESP_LOGW(TAG, "Unknown error type: 0x%x", event->error_handle->error_type);
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static void mqtt_app_start(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = CONFIG_BLYNK_MQTT_BROKER_URI,
            .verification.crt_bundle_attach = esp_crt_bundle_attach, /* Use built-in certificate bundle */
        },
        .credentials = {
            .username = BLYNK_MQTT_USERNAME,
            .authentication.password = CONFIG_BLYNK_AUTH_TOKEN,
        },
        .buffer.size = BLYNK_MQTT_BUFFER_SIZE,
    };

    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());
    ESP_LOGI(TAG, "[APP] Testing OTA V2");
    /* Force the linker to keep firmwareTag: -ffunction-sections/-fdata-sections
     * put it in its own section, and this toolchain's default --gc-sections
     * link (via picolibc.specs) can drop it despite __attribute__((used)) if
     * nothing genuinely references it. */
    ESP_LOGI(TAG, "[APP] Blynk firmware tag @ %p (%u bytes)", (const void *)firmwareTag,
             (unsigned)sizeof(firmwareTag));

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("esp-tls", ESP_LOG_VERBOSE);
    esp_log_level_set("mqtt_client", ESP_LOG_VERBOSE);
    esp_log_level_set("blynk_mqtt", ESP_LOG_VERBOSE);
    esp_log_level_set("transport_base", ESP_LOG_VERBOSE);
    esp_log_level_set("transport", ESP_LOG_VERBOSE);
    esp_log_level_set("outbox", ESP_LOG_VERBOSE);

    if (strlen(CONFIG_BLYNK_AUTH_TOKEN) == 0) {
        ESP_LOGW(TAG, "CONFIG_BLYNK_AUTH_TOKEN is empty - set it via `idf.py menuconfig` "
                 "under \"Blynk Configuration\" before connecting.");
    }

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi (over the ESP32-C6 co-processor
     * via esp_hosted) or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

    mqtt_app_start();
}
