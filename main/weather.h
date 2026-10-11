#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int      temperature; /* degrees Celsius, rounded */
    int      id;          /* the weather condition code of OpenWeatherMap */
    uint32_t symbol;      /* its code point in the Weather Icons font, by day or by night */
    char     city[64];    /* the name of the place, when known */
    int64_t  sunrise, sunset; /* of today there, in seconds of the epoch; 0: not known */
} weather_now_t;

/* Called from the task of the weather every time the weather is got. */
typedef void (*weather_listener_t)(const weather_now_t *now);

/*
 * Starts the task that asks OpenWeatherMap for the weather of the place of
 * the settings, once in a while. It asks nothing while the settings have no
 * key or no place.
 */
bool weather_start(weather_listener_t listener);

/* The code point of the Weather Icons font for a condition code; 0 for an unknown one. */
uint32_t weather_symbol(int id, bool daytime);

/* The color of a temperature: from blue in frost through green to red in heat. */
void weather_temp_to_rgb(int temp, uint8_t *r, uint8_t *g, uint8_t *b);
