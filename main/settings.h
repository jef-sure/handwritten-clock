#pragma once

#include <stdbool.h>

#include "esp_err.h"

enum {
    SETTINGS_HOST_NAME_LEN = 32,
    SETTINGS_TIMEZONE_LEN = 64,
    SETTINGS_WEATHER_KEY_LEN = 48,
    SETTINGS_LOCATION_LEN = 64,
    SETTINGS_IPINFO_KEY_LEN = 32,
    SETTINGS_CITY_LEN = 64,
};

typedef struct {
    char host_name[SETTINGS_HOST_NAME_LEN];
    char weather_key[SETTINGS_WEATHER_KEY_LEN]; /* of the API of OpenWeatherMap; no weather without it */
    bool auto_location;                         /* the place and the time zone are found by the address in the Internet */
    char ipinfo_key[SETTINGS_IPINFO_KEY_LEN];   /* the token of ipinfo.io that finds them */
    char location[SETTINGS_LOCATION_LEN];       /* or the place is this: coordinates, or a name or a postal code with a country */
    char timezone[SETTINGS_TIMEZONE_LEN];       /* and the time zone this, a name of the tz database; empty: not chosen yet */
    /*
     * What is found out and kept, not asked on the page: the place of the
     * settings as coordinates and its name. A time zone not chosen is found
     * out too, from the answer about the weather, and kept above.
     */
    bool  has_place;
    float lat, lon;
    char  city[SETTINGS_CITY_LEN];
    /* Asked on the page again: the name of the place the screen shows; empty: the name found out. */
    char place_name[SETTINGS_CITY_LEN];
} settings_t;

/* Reads the settings kept in NVS; NVS itself must be started. */
esp_err_t settings_init(void);
void      settings_get(settings_t *settings);
esp_err_t settings_update(const settings_t *settings);

/* Forgets everything that is kept: the settings are the defaults again. */
esp_err_t settings_erase(void);

/* All of them are text fields with an end; an empty host name gets its default. */
void settings_normalize(settings_t *settings);

/*
 * A setup has the screen and the memory to itself: it is a boot of its own,
 * without the clock. One asks for it and restarts, the next boot takes the
 * request and the one after it is an ordinary boot again.
 */
typedef enum {
    BOOT_CLOCK = 0,
    BOOT_WIFI_SETUP,   /* the name and the password of a network, over BLE */
    BOOT_WEB_SETTINGS, /* the page of settings in the network */
} boot_mode_t;

esp_err_t   settings_ask_boot(boot_mode_t mode);
boot_mode_t settings_take_boot(void);
