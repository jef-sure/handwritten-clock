#pragma once

#include <stdbool.h>

#include "settings.h"

typedef struct {
    float lat, lon; /* OpenWeatherMap takes a place by its coordinates */
    char  city[64]; /* its name, when known */
} location_t;

/*
 * Where the device is. With the place found by the address in the Internet,
 * ipinfo.io is asked, once in a while, and the time zone it names becomes the
 * time zone of the clock. Otherwise the place is that of the settings:
 * coordinates as they are, "52.52,13.40", or a name or a postal code with a
 * country, which the geocoder of OpenWeatherMap turns into coordinates; they
 * are found once and kept with the settings.
 * Returns false while the place is not known. Needs 8 KB of stack.
 */
bool location_get(const settings_t *settings, location_t *location);
