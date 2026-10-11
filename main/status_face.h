#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dgx_screen.h"

/*
 * What the clock waits for after it is turned on, each sign in a place of its
 * own in the row above the time: the sign of Wi-Fi while the network is being
 * connected, then a "Z" while the time is asked from the network, and a "W"
 * while the weather is. The sign of Wi-Fi comes arc after arc; the letters
 * are written by a pen, taken away and written again. A sign goes away when
 * what it waits for has come for the first time, and when all of them have
 * gone the row is empty: everything works as it is meant to.
 */
typedef struct {
    dgx_screen_t *screen;
    int           x, y, width, height; /* the free part of the row */
    uint16_t      paper;               /* color of the screen */
    uint16_t      ink, ink_night;      /* of the signs by day and by night */
    bool (*connected)(void);           /* the network is there */
    bool (*has_time)(void);            /* the time is the time of the network */
    bool (*has_weather)(void);         /* the first weather has come; NULL: no weather is waited for */
} status_face_config_t;

/* Starts the task that draws the signs; it ends by itself when the last sign has gone. */
bool status_face_start(const status_face_config_t *config);

/* How much of the night it is, 0: day .. 1: night: the signs take their color between the two. */
void status_face_set_night(float night);
