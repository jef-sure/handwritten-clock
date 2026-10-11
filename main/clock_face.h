#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "dgx_screen.h"

/*
 * The time in handwritten digits: every digit stands in a cell of its own and
 * becomes the next one there. All sizes are counted from the place given.
 */
typedef enum {
    CLOCK_SECONDS_NONE,
    CLOCK_SECONDS_SMALL, /* in the corner above the minutes: there for one who looks for them */
    CLOCK_SECONDS_LINE,  /* as large as hours and minutes, after a colon of their own */
} clock_seconds_t;

typedef struct {
    dgx_screen_t   *screen;
    int             x, y, width, height; /* the place of the time on the screen */
    clock_seconds_t seconds;
    uint16_t        paper;      /* color of the screen */
    uint16_t        ink, ink_night; /* of the time by day and by night; one goes into the other around sunrise and sunset */
    /*
     * Told how much of the night it is, 0: day .. 1: night, whenever that
     * changes: what else is on the screen takes its color of the night with
     * the time. Called from the task of the clock; NULL: nobody is told.
     */
    void (*on_night)(float night);
} clock_face_config_t;

/* When the sun rises and sets today, in seconds of the epoch; until it is told the time has its color of the day. */
void clock_face_set_sun(time_t sunrise, time_t sunset);

/* Starts the task that shows the time of the system clock; false: no memory, or the place is too small. */
bool clock_face_start(const clock_face_config_t *config);

/*
 * The part of the row above the time that the seconds leave free, after the
 * clock is started; false: there is no such row, the seconds are not small.
 */
bool clock_face_top_row(int *x, int *y, int *width, int *height);
