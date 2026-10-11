#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "dgx_morph_render.h"
#include "dgx_screen.h"

/* The display of the board, ready to draw on; NULL when it does not start. */
dgx_screen_t *display_init(void);

/*
 * Exactly one task works with the display, so neither a mutex nor a
 * semaphore stands at the bus. The others draw into virtual screens of their
 * own and tell it by a message what to show.
 *
 * A message is sent and its sender goes on with whatever else it has to do.
 * A virtual screen has a mutex: who draws into it holds it while it draws,
 * the display task holds it while it sends the screen to the display. So the
 * one who draws waits only when it comes back to a screen that is still on
 * its way.
 */
bool display_start(void);

/* The mutex of one virtual screen. */
SemaphoreHandle_t display_hold_create(void);
/* Before drawing into the screen, or changing its table of colors. */
void display_take(SemaphoreHandle_t hold);
/* After it, when there is nothing to show. */
void display_give(SemaphoreHandle_t hold);

/*
 * After it, when there is: the screen is given and the display task is asked
 * to show it; returns at once. A virtual screen of 8 bits a pixel is shown at
 * x, y through a table of 256 colors of the display. All of it,
 */
void display_show(SemaphoreHandle_t hold, dgx_screen_t *band, int x, int y, uint16_t *lut);
/*
 * or only the part that has been drawn into since it was shown last. The
 * screen says which: a batch must be open on it, dgx_screen_progress_up()
 * once after it is made. Nothing drawn: the screen is given, nothing is asked.
 */
void display_show_changed(SemaphoreHandle_t hold, dgx_screen_t *band, int x, int y, uint16_t *lut);
/* a frame of dots as far as t on its way; this one returns when the frame has been shown. */
void display_show_glow(SemaphoreHandle_t hold, dgx_morph_glow_t *glow, float t, int x, int y);
