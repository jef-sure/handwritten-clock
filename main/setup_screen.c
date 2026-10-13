#include <math.h>
#include <stdbool.h>
#include <stdlib.h>

#include "esp_log.h"
#include "qrcodegen.h"

#include "dgx_bits.h"
#include "dgx_draw.h"
#include "dgx_hw_font.h"
#include "fonts/font0ss.h"

#include "setup_screen.h"

/* the text is handwritten; a pen of one pixel is too thin a black line on the white */
#define TEXT_PEN_CELLS 9.0f
#define TEXT_PEN_LEAST 2
#define TEXT_MARGIN    8
/* the largest code: version 10 holds 271 bytes at the lowest correction, several times what a setup shows */
#define CODE_VERSION_MOST 10

static const char *TAG = "setup_screen";

/* The heights of the title above the code and of the state under it. */
static int title_strip(const dgx_screen_t *screen)
{
    return screen->height / 6;
}

static int state_strip(const dgx_screen_t *screen)
{
    return screen->height / 7;
}

/* A line in the middle of a strip of the screen, as large as the strip lets. */
static void draw_line_of_text(dgx_screen_t *screen, int y, int height, const char *text)
{
    dgx_font_t *font = font0ss();
    int         left, top, right, bottom;
    dgx_fill_rectangle(screen, 0, y, screen->width, height, dgx_rgb_to_16(255, 255, 255));
    if (!text || !dgx_hw_text_box(font, text, &left, &top, &right, &bottom)) return;
    float by_width = (float)(screen->width - 2 * TEXT_MARGIN) / (right - left + 1);
    float by_height = (float)(height - 4) / font->hw->symbol_size_y;
    float scale = by_width < by_height ? by_width : by_height;
    int   pen = (int)lroundf(scale * TEXT_PEN_CELLS);
    if (pen < TEXT_PEN_LEAST) pen = TEXT_PEN_LEAST;
    /* the line of the font stands in the strip, whatever letters the text has */
    int base = y + (int)lroundf((height - font->hw->symbol_size_y * scale) / 2 + font->hw->base_line * scale);
    int x = (int)lroundf((screen->width - (right - left + 1) * scale) / 2 - left * scale);
    dgx_hw_draw_text(screen, x, base, font, text, scale, pen, 0, true);
}

void setup_screen_state(dgx_screen_t *screen, const char *state)
{
    /* the strip and its text are one picture: a display that shows a batch whole shows them together */
    dgx_screen_progress_up(screen);
    draw_line_of_text(screen, screen->height - state_strip(screen), state_strip(screen), state);
    dgx_screen_progress_down(screen);
}

void setup_screen_draw(dgx_screen_t *screen, const char *title, const char *code, const char *state)
{
    /* one batch: the modules of the code are hundreds of rectangles, and a batch of its own each would take a frame each */
    dgx_screen_progress_up(screen);
    dgx_fill_rectangle(screen, 0, 0, screen->width, screen->height, dgx_rgb_to_16(255, 255, 255));
    draw_line_of_text(screen, 0, title_strip(screen), title);
    setup_screen_state(screen, state);
    if (code) {
        /* the library gives the code as a square of modules in a buffer; it needs a second buffer of the same size to work in */
        size_t   length = qrcodegen_BUFFER_LEN_FOR_VERSION(CODE_VERSION_MOST);
        uint8_t *modules = malloc(length), *work = malloc(length);
        if (modules && work &&
            qrcodegen_encodeText(code, work, modules, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, CODE_VERSION_MOST, qrcodegen_Mask_AUTO, true)) {
            int size = qrcodegen_getSize(modules);
            int top = title_strip(screen), bottom = screen->height - state_strip(screen);
            int room = bottom - top < screen->width ? bottom - top : screen->width;
            /* a quiet zone of two modules at every side */
            int module = room / (size + 4);
            if (module < 1) module = 1;
            int x0 = (screen->width - size * module) / 2;
            int y0 = top + (bottom - top - size * module) / 2;
            for (int x = 0; x < size; ++x) {
                for (int y = 0; y < size; ++y) {
                    if (qrcodegen_getModule(modules, x, y)) dgx_fill_rectangle(screen, x0 + x * module, y0 + y * module, module, module, 0);
                }
            }
        } else {
            ESP_LOGE(TAG, "No QR code of \"%s\": no memory, or the text is too long", code);
        }
        free(work);
        free(modules);
    }
    dgx_screen_progress_down(screen);
}
