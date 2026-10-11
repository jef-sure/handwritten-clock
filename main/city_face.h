#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "dgx_screen.h"

/*
 * The name of the place, written by a pen: a letter after a letter, as a
 * hand writes it. All sizes are counted from the place given.
 */
typedef struct {
    dgx_screen_t *screen;
    int           x, y, width, height; /* the place of the name on the screen */
    uint16_t      paper;               /* color of the screen */
    uint16_t      ink, ink_night;      /* of the name by day and by night */
} city_face_config_t;

/* Starts the task that writes; the place stays empty until a name is shown. */
bool city_face_start(const city_face_config_t *config);

/* The name to show from now on; the same name is not written again. */
void city_face_show(const char *name);

/* How much of the night it is, 0: day .. 1: night: the name takes its color between the two. */
void city_face_set_night(float night);
