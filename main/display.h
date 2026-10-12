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
 * Once, when NVS is there and before anything else is drawn: a display on
 * MIPI-DSI is given the numbers of this board, measured at the first start
 * and kept in NVS. To any other display it means nothing.
 */
void display_tune(void);

/*
 * Exactly one task works with the display, so neither a mutex nor a
 * semaphore stands at the bus. The others draw into virtual screens of their
 * own and tell it by a message what to show.
 *
 * Nothing is locked at a virtual screen either: it has one owner at a time.
 * The task that draws into it has it. A message to show it hands it to the
 * display task, and that one gives it back when the screen is on the
 * display. A message is sent and its sender goes on with whatever else it
 * has to do; it waits only when it comes back to a screen that has not come
 * back yet. A "hold" is the word that the screen is with its owner.
 *
 * So nobody draws faster than the display takes pictures. Behind SPI a
 * screen takes its time to get there. A display that shows by frames, one
 * on MIPI-DSI, takes a picture at once: there display_take() guards the
 * time, and a screen is not drawn anew sooner than a frame after it was
 * drawn last.
 */
bool display_start(void);

/* The hold of one virtual screen. */
SemaphoreHandle_t display_hold_create(void);
/* Before drawing into the screen, or changing its table of colors: waits until the screen is back. */
void display_take(SemaphoreHandle_t hold);
/* After it, when there is nothing to show. */
void display_give(SemaphoreHandle_t hold);

/*
 * After it, when there is: the screen goes to the display task, which is
 * asked to show it; returns at once. A virtual screen of 8 bits a pixel is
 * shown at x, y through a table of 256 colors of the display. All of it,
 */
void display_show(SemaphoreHandle_t hold, dgx_screen_t *band, int x, int y, uint16_t *lut);
/*
 * or only the part that has been drawn into since it was shown last. The
 * screen says which: a batch must be open on it, dgx_screen_progress_up()
 * once after it is made. Nothing drawn: the screen stays with its owner,
 * nothing is asked.
 */
void display_show_changed(SemaphoreHandle_t hold, dgx_screen_t *band, int x, int y, uint16_t *lut);
/* a frame of dots as far as t on its way; showing takes the frame away, the next one is drawn into an empty one. */
void display_show_glow(SemaphoreHandle_t hold, dgx_morph_glow_t *glow, float t, int x, int y);
