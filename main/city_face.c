#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dgx_draw.h"
#include "dgx_font.h"
#include "dgx_hw_font.h"
#include "drivers/vscreen.h"
#include "fonts/font0ss.h"

#include "city_face.h"
#include "display.h"

static const char *TAG = "city_face";

/* The pen is this many cells thick. */
#define PEN_CELLS 9.0f
/* It writes this many cells of effort in a second, and every straight piece of a letter adds this many cells. */
#define TEMPO       1500.0f
#define BEND_EFFORT 6.0f
/* Free at the sides and above and under the line, in pixels. */
#define MARGIN_X 8
#define MARGIN_Y 2
#define NAME_LEN 64
#define INK      255

static city_face_config_t cfg;
static dgx_font_t        *font;
/* the name is written into a band of its own, 8 bits a pixel, and the band is shown by the display task */
static dgx_screen_t      *band;
static SemaphoreHandle_t  hold; /* of the band: taken to draw into it */
static uint16_t           lut[256];
static uint16_t           lut_ink;           /* the ink the table is made for */
static bool               lut_made;
static float              night_wanted;      /* 0: day .. 1: night, under the lock */
static TaskHandle_t       task;
static portMUX_TYPE       lock = portMUX_INITIALIZER_UNLOCKED;
static char               wanted[NAME_LEN];  /* asked for, under the lock */
static char               shown[NAME_LEN];   /* the name asked for that is on the screen, or on its way there */
static char               written[NAME_LEN]; /* in the letters the font has */

/*
 * The font has the Latin letters of English and German and the Russian ones.
 * A letter of another language with a mark over it or through it is written
 * as the letter without the mark: Kraków as Krakow, Málaga as Malaga. The
 * table is U+00C0 .. U+017F, Latin-1 and Latin Extended-A. What is neither
 * there nor in the font is left out.
 */
static const char plain[][3] = {
    "A", "A", "A", "A", "A", "A", "Ae", "C", "E", "E", "E", "E", "I", "I", "I", "I", /* U+00C0 */
    "D", "N", "O", "O", "O", "O", "O", "x", "O", "U", "U", "U", "U", "Y", "Th", "ss", /* U+00D0 */
    "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i", /* U+00E0 */
    "d", "n", "o", "o", "o", "o", "o", "/", "o", "u", "u", "u", "u", "y", "th", "y", /* U+00F0 */
    "A", "a", "A", "a", "A", "a", "C", "c", "C", "c", "C", "c", "C", "c", "D", "d", /* U+0100 */
    "D", "d", "E", "e", "E", "e", "E", "e", "E", "e", "E", "e", "G", "g", "G", "g", /* U+0110 */
    "G", "g", "G", "g", "H", "h", "H", "h", "I", "i", "I", "i", "I", "i", "I", "i", /* U+0120 */
    "I", "i", "Ij", "ij", "J", "j", "K", "k", "k", "L", "l", "L", "l", "L", "l", "L", /* U+0130 */
    "l", "L", "l", "N", "n", "N", "n", "N", "n", "n", "N", "n", "O", "o", "O", "o", /* U+0140 */
    "O", "o", "Oe", "oe", "R", "r", "R", "r", "R", "r", "S", "s", "S", "s", "S", "s", /* U+0150 */
    "S", "s", "T", "t", "T", "t", "T", "t", "U", "u", "U", "u", "U", "u", "U", "u", /* U+0160 */
    "U", "u", "U", "u", "W", "w", "Y", "y", "Y", "Z", "z", "Z", "z", "Z", "z", "s", /* U+0170 */
};

static void to_letters_of_font(char *name, size_t size)
{
    char   folded[NAME_LEN];
    size_t length = 0, idx = 0;
    while (name[idx]) {
        size_t      start = idx;
        uint32_t    cp = decodeUTF8next(name, &idx);
        const char *as = NULL;
        size_t      bytes = idx - start;
        bool        kept = cp < 0x80 || (cp >= 0x400 && cp <= 0x45F) || (cp < 0x100 && strchr("\xC4\xD6\xDC\xE4\xF6\xFC\xDF", (int)cp));
        if (kept) {
            as = name + start;
        } else if (cp >= 0xC0 && cp < 0x180) {
            as = plain[cp - 0xC0];
            bytes = strlen(as);
        }
        /* no space before the name or after another one: something has been left out there */
        if (as && *as == ' ' && (!length || folded[length - 1] == ' ')) continue;
        if (!as || length + bytes >= sizeof(folded)) continue;
        memcpy(folded + length, as, bytes);
        length += bytes;
    }
    folded[length] = 0;
    strlcpy(name, folded, size);
}

/* paper is color 0 of the band and ink is color 255 */
static void make_lut(uint16_t ink)
{
    for (int i = 0; i < 256; ++i) {
        int r = ((cfg.paper >> 11) & 31) + (((ink >> 11) & 31) - ((cfg.paper >> 11) & 31)) * i / 255;
        int g = ((cfg.paper >> 5) & 63) + (((ink >> 5) & 63) - ((cfg.paper >> 5) & 63)) * i / 255;
        int b = (cfg.paper & 31) + ((ink & 31) - (cfg.paper & 31)) * i / 255;
        lut[i] = (uint16_t)(r << 11 | g << 5 | b);
    }
    lut_ink = ink;
    lut_made = true;
}

/*
 * The table of colors for the ink of this moment, between the ink of the day
 * and the ink of the night. The band must be held: the table is a part of
 * what goes to the display. True: the table is another, all of the band is
 * to be shown.
 */
static bool recolor(void)
{
    portENTER_CRITICAL(&lock);
    float night = night_wanted;
    portEXIT_CRITICAL(&lock);
    int      r = (int)lroundf(((cfg.ink >> 11) & 31) * (1 - night) + ((cfg.ink_night >> 11) & 31) * night);
    int      g = (int)lroundf(((cfg.ink >> 5) & 63) * (1 - night) + ((cfg.ink_night >> 5) & 63) * night);
    int      b = (int)lroundf((cfg.ink & 31) * (1 - night) + (cfg.ink_night & 31) * night);
    uint16_t ink = (uint16_t)(r << 11 | g << 5 | b);
    if (lut_made && ink == lut_ink) return false;
    make_lut(ink);
    return true;
}

static void write_name(void)
{
    int left, top, right, bottom;
    display_take(hold);
    recolor();
    dgx_fill_rectangle(band, 0, 0, band->width, band->height, 0);
    display_show(hold, band, cfg.x, cfg.y, lut);
    if (!written[0] || !dgx_hw_text_box(font, written, &left, &top, &right, &bottom)) return;
    /*
     * As large as the height lets. A name too long for the width is not made
     * smaller but narrower: the letters keep their height and are squeezed
     * across, with a thinner pen, as the digits of the time are stretched.
     * A short name is not stretched to the width.
     */
    float by_width = (float)(cfg.width - 2 * MARGIN_X) / (right - left + 1);
    float scale_y = (float)(cfg.height - 2 * MARGIN_Y) / font->hw->symbol_size_y;
    float scale_x = by_width < scale_y ? by_width : scale_y;
    /* the pen is round: one width between the two scales */
    int pen = (int)lroundf(sqrtf(scale_x * scale_y) * PEN_CELLS);
    if (pen < 1) pen = 1;
    /* the line of the font stands in the place, whatever letters the name has */
    int base = (int)lroundf((cfg.height - font->hw->symbol_size_y * scale_y) / 2 + font->hw->base_line * scale_y);
    int x = (int)lroundf((cfg.width - (right - left + 1) * scale_x) / 2 - left * scale_x);

    dgx_hw_pace_t    pace = dgx_hw_pace(font, BEND_EFFORT);
    dgx_hw_writing_t writing;
    dgx_hw_writing_begin_xy(&writing, band, x, base, font, written, scale_x, scale_y, pen, INK, true, &pace);
    int64_t start_us = esp_timer_get_time();
    bool    finished;
    do {
        display_take(hold);
        bool whole = recolor();
        finished = dgx_hw_writing_draw_to(&writing, TEMPO * (esp_timer_get_time() - start_us) * 1e-6f);
        /* only where the pen has been since the last time, unless the ink is another */
        if (whole) {
            display_show(hold, band, cfg.x, cfg.y, lut);
        } else {
            display_show_changed(hold, band, cfg.x, cfg.y, lut);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (!finished);
    ESP_LOGI(TAG, "\"%s\": %.3f x %.3f pixels a cell, pen %d, written in %.1f s", written, scale_x, scale_y, pen,
             (esp_timer_get_time() - start_us) * 1e-6f);
}

static void city_face_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* another ink: the name as it is written, all of it, in the new color */
        display_take(hold);
        if (recolor()) {
            display_show(hold, band, cfg.x, cfg.y, lut);
        } else {
            display_give(hold);
        }
        /* a name asked for while another was being written is written after it */
        for (;;) {
            char name[NAME_LEN];
            portENTER_CRITICAL(&lock);
            memcpy(name, wanted, sizeof(name));
            portEXIT_CRITICAL(&lock);
            if (strcmp(name, shown) == 0) break;
            memcpy(shown, name, sizeof(shown));
            memcpy(written, name, sizeof(written));
            to_letters_of_font(written, sizeof(written));
            write_name();
        }
    }
}

void city_face_set_night(float night)
{
    if (!task) return;
    portENTER_CRITICAL(&lock);
    night_wanted = night < 0 ? 0 : night > 1 ? 1 : night;
    portEXIT_CRITICAL(&lock);
    xTaskNotifyGive(task);
}

void city_face_show(const char *name)
{
    if (!task) return;
    portENTER_CRITICAL(&lock);
    strlcpy(wanted, name ? name : "", sizeof(wanted));
    portEXIT_CRITICAL(&lock);
    xTaskNotifyGive(task);
}

bool city_face_start(const city_face_config_t *config)
{
    cfg = *config;
    font = font0ss();
    if (cfg.height <= 2 * MARGIN_Y || cfg.width <= 2 * MARGIN_X) {
        ESP_LOGE(TAG, "No place for the name in %dx%d", cfg.width, cfg.height);
        return false;
    }
    band = dgx_vscreen_init(cfg.width, cfg.height, 8, DgxScreenRGB);
    if (!band) {
        ESP_LOGE(TAG, "No memory for the name, %dx%d", cfg.width, cfg.height);
        return false;
    }
    hold = display_hold_create();
    if (!hold) return false;
    /* a batch that is never closed: the band keeps count of where it has been drawn into */
    dgx_screen_progress_up(band);
    hold = display_hold_create();
    if (!hold) return false;
    /* a batch that is never closed: the band keeps count of where it has been drawn into */
    dgx_screen_progress_up(band);
    /* paper is color 0 of the band and ink is color 255 */
    for (int i = 0; i < 256; ++i) {
        int r = ((cfg.paper >> 11) & 31) + (((cfg.ink >> 11) & 31) - ((cfg.paper >> 11) & 31)) * i / 255;
        int g = ((cfg.paper >> 5) & 63) + (((cfg.ink >> 5) & 63) - ((cfg.paper >> 5) & 63)) * i / 255;
        int b = (cfg.paper & 31) + ((cfg.ink & 31) - (cfg.paper & 31)) * i / 255;
        lut[i] = (uint16_t)(r << 11 | g << 5 | b);
    }
    return xTaskCreate(city_face_task, "city_face", 4096, NULL, tskIDLE_PRIORITY + 2, &task) == pdPASS;
}
