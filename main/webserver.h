#pragma once

#include <stdbool.h>

#include "esp_err.h"

/*
 * The page of settings, served in the network: what it gets is kept in NVS.
 * The page is misc/index.html; the build cuts it into pieces at its %FIELDS%.
 */
esp_err_t web_start(void);
void      web_stop(void);

/* Whether the one who saved the settings asked to forget the Wi-Fi network as well. */
bool web_clears_wifi(void);

/* Whether the one at the page asked to forget everything that is kept: the settings and the network. */
bool web_clears_all(void);

/* Whether somebody has saved the settings or left them as they are, and has been told so. */
bool web_is_done(void);
