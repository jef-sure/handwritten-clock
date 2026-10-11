#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "dgx_screen.h"

/* Starts Wi-Fi without connecting anywhere. NVS and the settings must be started. */
esp_err_t net_init(void);

/* Whether the name and the password of a network are kept. */
bool net_is_provisioned(void);

/*
 * Connects to the network kept and keeps the connection; the system clock is
 * set from the network as soon as there is one. Bluetooth is given up for the
 * rest of the boot: its memory goes to the heap.
 */
esp_err_t net_start(void);
bool      net_is_connected(void);

/* Whether the network has given the time; before that the clock goes from the time of the build. */
bool net_has_time(void);

/* How strong the signal of the access point is, in dBm; false without a connection. */
bool net_get_rssi(int *rssi);

/* The address the device has in the network, as text; false while it has none. */
bool net_get_address(char *text, size_t size);

/* Forgets the network kept: the next boot is the setup of Wi-Fi. */
void net_forget(void);

/*
 * Takes the name and the password of a network over BLE from the phone
 * application "ESP BLE Provisioning"; the screen shows the QR code for it.
 * Returns when a connection with what was given is made, true, or the time
 * is out, false; timeout_ms 0 waits for ever. A network kept before stays
 * kept until another one is accepted. Restart after it.
 */
bool net_provision(dgx_screen_t *screen, uint32_t timeout_ms);
