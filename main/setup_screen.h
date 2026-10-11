#pragma once

#include "dgx_screen.h"

/*
 * The screen of a setup: a title at the top, a QR code in the middle, a line
 * of state at the bottom, black on white. The setup of Wi-Fi and the page of
 * settings both show it; nothing else draws meanwhile.
 */

/* Draws the whole screen; code NULL leaves the middle empty. */
void setup_screen_draw(dgx_screen_t *screen, const char *title, const char *code, const char *state);

/* Writes another state under the code. */
void setup_screen_state(dgx_screen_t *screen, const char *state);
