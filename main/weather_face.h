#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dgx_screen.h"

/*
 * The weather: its symbol made of dots at the left, the temperature in
 * handwritten digits at the right, both in the color of the temperature. A
 * symbol flows into the next one; the number is written by a pen, which first
 * takes away the symbols that change. All sizes
 * are counted from the place given.
 */
typedef struct {
    dgx_screen_t *screen;
    int           x, y, width, height; /* the place of the weather on the screen */
    int           symbol_width;        /* of it, at the left, for the symbol; the temperature has the rest */
} weather_face_config_t;

/* Starts the task that draws; the place stays empty until the first weather is shown. */
bool weather_face_start(const weather_face_config_t *config);

/* The weather to show from now on; symbol is a code point of the Weather Icons font, 0 for none. */
void weather_face_show(uint32_t symbol, int temperature);

/*
 * How much of the night it is, 0: day .. 1: night. By night the weather is
 * less bright, as the time beside it is; its color stays the one of the
 * temperature.
 */
void weather_face_set_night(float night);
