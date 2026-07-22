/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the Blynk MQTT client: connects to the broker configured under
 * "Blynk Configuration" (menuconfig), subscribes to datastream and control
 * downlinks, and publishes device info on every clean connection.
 *
 * Call once network connectivity is up (e.g. after example_connect()).
 */
void blynk_mqtt_start(void);

#ifdef __cplusplus
}
#endif
