#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "dgx_bits.h"
#include "dgx_draw.h"
#include "dgx_hw_font.h"
#include "dgx_matrix_morph.h"
#include "dgx_morph.h"
#include "dgx_morph_render.h"
#include "dgx_morph_sources.h"
#include "drivers/vscreen.h"
#include "fonts/font0ss.h"

#include "display.h"
#include "fonts/WeatherIconsRegular24.h"
#include "weather.h"
#include "weather_face.h"

static const char *TAG = "weather_face";

/* The weather becomes the next one in so long. */
#define MORPH_US 1000000LL
/* The pen is this many cells thick. */
#define PEN_CELLS 9.0f
/* The temperature is written by a pen as the name of the place is, and taken away twice as fast: cells of effort in a second. */
#define WRITE_TEMPO 1500.0f
#define ERASE_TEMPO 3000.0f
#define BEND_EFFORT 6.0f
/*
 * "℃" is one symbol as wide and as tall as a digit, made as in the CasusDotView
 * font: a small ring in its upper left corner, and a narrow C lower and to
 * the right. The shares are of the box of a digit. It stands at the right
 * edge of the place, so many cells after the number.
 */
#define DEGREE_SHARE   (3 / 7.0f)  /* the ring, of the width */
#define C_WIDTH_SHARE  (4 / 7.0f)
#define C_HEIGHT_SHARE (10 / 12.0f)
#define UNIT_GAP       12
/*
 * The number is right before it, every symbol in a place of its own that it
 * never leaves: the units, the tens or the minus of a number of one digit,
 * the minus of a number of two. So many cells are free between two places.
 */
#define PLACES    3
#define PLACE_GAP 16

#define INK 255
/* By night the weather is this bright against the day; the hue is not touched. It goes there in so many steps. */
#define NIGHT_LIGHT  0.6f
#define LIGHT_STEPS  64

/*
 * The symbols are those of the Weather Icons font by Erik Flowers, made into
 * main/fonts/WeatherIconsRegular24.c by font2c of dgx:
 *   font2c -f charset.txt weathericons-regular-webfont.ttf 24
 * where charset.txt has the symbols of the table of weather.c. Every pixel
 * of a symbol is a dot, and all the symbols have one box.
 *
 * The dots are drawn by the glow renderer of dgx. A font turned into dots
 * keeps the steps of its one-bit picture: every frame is blurred so many
 * times before it goes to the screen. One pass costs no memory since dgx
 * 0.4.9, where it is made row by row on the way to the screen; more passes
 * take a copy of the frame, a byte a pixel. 0: not blurred at all.
 */
static const int blur_passes = 1;

/*
 * A dot of the old symbol that the new one has no use for does not fade
 * where it is: it flies into the nearest dot that stays and goes out there,
 * not further than so many dots. A cloud that becomes smaller is thus drawn
 * in, not melted.
 */
static const dgx_morph_options_t morph_options = {.orphans = DGX_MORPH_ORPHANS_MERGE, .merge_radius = 8};

typedef struct {
    uint32_t symbol;
    int      temperature;
} shown_t;

typedef struct {
    uint8_t r, g, b;
} rgb_t;

static weather_face_config_t cfg;
static QueueHandle_t         queue;

/* the symbol */
static dgx_font_t       *symbols;
static dgx_morph_glow_t *glow;
static dgx_bit_matrix_t *matrix; /* of the symbol shown */
static int               cell, glow_radius;
static int               symbol_x, symbol_y;

/* the temperature: a frame is drawn in a band, 8 bits a pixel, and sent to the screen whole */
static dgx_font_t   *font;
static dgx_screen_t *band;
static SemaphoreHandle_t band_hold, glow_hold; /* of the band and of the frame of dots: taken to draw into them */
static int           band_x, band_y;
static int           base_y;
static float         scale_x, scale_y;
static int           pen;
static int           digits_top;          /* of the digits, in cells of the font */
static int           degree_r;            /* of the ring, in pixels */
static int           unit_x;              /* of the box of "℃" in the band */
static int           digit_width;         /* of the widest digit, in cells */
static int           c_left;              /* of the letter C of the font, in cells */
static float         c_scale_x, c_scale_y;
static int           c_y;                 /* of its base line in the band */
static float         pitch;               /* of the places, in cells */
static struct {
    int  x;     /* of the place in the band */
    char shown; /* the symbol written there; 0: nothing */
} places[PLACES];
static rgb_t         color;
static bool          has_weather;
static int           light = LIGHT_STEPS;           /* how bright what is shown is, of LIGHT_STEPS */
static portMUX_TYPE  asked_lock = portMUX_INITIALIZER_UNLOCKED;
static shown_t       asked;                         /* the weather asked for last, under the lock */
static bool          has_asked;
static float         night_wanted;                  /* 0: day .. 1: night, under the lock */

/* A color as bright as the hour lets it be: all three parts by one share, so the hue stays. */
static rgb_t lit(rgb_t c)
{
    rgb_t dim = {(uint8_t)(c.r * light / LIGHT_STEPS), (uint8_t)(c.g * light / LIGHT_STEPS), (uint8_t)(c.b * light / LIGHT_STEPS)};
    return dim;
}

static rgb_t mix(rgb_t a, rgb_t b, float t)
{
    rgb_t c = {(uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t), (uint8_t)(a.b + (b.b - a.b) * t)};
    return c;
}

/* The largest dots with which the box of a symbol and the glow around it fit the place. */
static bool symbol_layout(int x, int width)
{
    symbols = WeatherIconsRegular24();
    matrix = dgx_morph_glyph_matrix(symbols, 0); /* no such symbol: an empty box */
    if (!matrix) return false;
    int most = width / matrix->width < cfg.height / matrix->height ? width / matrix->width : cfg.height / matrix->height;
    for (cell = most; cell > 0; --cell) {
        /* the radius the glow renderer gives its discs */
        glow_radius = cell / 2 + cell / 4 > 2 ? cell / 2 + cell / 4 : 2;
        int w = matrix->width * cell + 2 * glow_radius, h = matrix->height * cell + 2 * glow_radius;
        if (w > width || h > cfg.height) continue;
        glow = dgx_morph_glow_create(w, h, cell, cfg.screen->color_bits);
        if (!glow) continue;
        if (blur_passes > 0 && !dgx_morph_glow_set_filter(glow, dgx_morph_glow_blur, (void *)&blur_passes)) {
            ESP_LOGW(TAG, "No memory to blur the symbol");
        }
        symbol_x = x + (width - w) / 2;
        symbol_y = cfg.y + (cfg.height - h) / 2;
        ESP_LOGI(TAG, "Symbol: box %dx%d dots, a dot %d pixels, %dx%d at %d,%d", matrix->width, matrix->height, cell, w, h, symbol_x,
                 symbol_y);
        return true;
    }
    return false;
}

/* A symbol of the number as text; the minus is the minus sign of the font, as high as the middle of a digit, not its hyphen. */
static const char *symbol_text(char symbol, char *text)
{
    if (symbol == '-') return "\xE2\x88\x92"; /* U+2212 */
    text[0] = symbol;
    text[1] = 0;
    return text;
}

/* What puts a symbol in the middle of a place, in cells. */
static float symbol_shift(char symbol)
{
    char one[2];
    int  left, top, right, bottom;
    if (!dgx_hw_text_box(font, symbol_text(symbol, one), &left, &top, &right, &bottom)) return 0;
    return (pitch - (right - left + 1)) / 2 - left;
}

/* Digits as tall as the place; as wide too, unless three places and "℃" need them narrower. */
static bool temperature_layout(int x, int width)
{
    int left, right, top, bottom, c_right, c_top, c_bottom;
    font = font0ss();
    if (!dgx_hw_text_box(font, "0123456789", &left, &top, &right, &bottom)) return false;
    int tall = bottom - top + 1;
    digits_top = top;
    for (int d = 0; d < 10; ++d) {
        char digit[2] = {(char)('0' + d), 0};
        if (!dgx_hw_text_box(font, digit, &left, &top, &right, &bottom)) return false;
        if (right - left + 1 > digit_width) digit_width = right - left + 1;
    }
    if (!dgx_hw_text_box(font, "C", &c_left, &c_top, &c_right, &c_bottom)) return false;
    pitch = digit_width + PLACE_GAP;
    pen = 1;
    for (int pass = 0; pass < 2; ++pass) {
        scale_y = (float)(cfg.height - 3 * pen) / tall;
        scale_x = (float)(width - 2 * pen) / (PLACES * pitch + UNIT_GAP + digit_width);
        if (scale_x > scale_y) scale_x = scale_y;
        if (!(scale_x > 0 && scale_y > 0)) return false;
        pen = (int)lroundf(scale_x * PEN_CELLS);
        if (pen < 1) pen = 1;
    }
    /* the ring is round: by the size across, and no thinner than its own line */
    degree_r = (int)lroundf(digit_width * DEGREE_SHARE * scale_x / 2);
    if (degree_r < pen + 1) degree_r = pen + 1;
    band = dgx_vscreen_init(width, cfg.height, 8, DgxScreenRGB);
    /* a batch that is never closed: the band keeps count of where it has been drawn into */
    if (band) dgx_screen_progress_up(band);
    if (!band) return false;
    band_x = x;
    band_y = cfg.y;
    base_y = (int)lroundf((cfg.height - tall * scale_y) / 2 - digits_top * scale_y);
    unit_x = width - pen - (int)lroundf(digit_width * scale_x);
    for (int i = 0; i < PLACES; ++i) places[i].x = unit_x - (int)lroundf((UNIT_GAP + (PLACES - i) * pitch) * scale_x);
    /* the C of the font squeezed into its part of the box, its foot where the feet of the digits are */
    c_scale_x = scale_x * digit_width * C_WIDTH_SHARE / (c_right - c_left + 1);
    c_scale_y = scale_y * tall * C_HEIGHT_SHARE / (c_bottom - c_top + 1);
    c_y = base_y + (int)lroundf((digits_top + tall - 1) * scale_y - c_bottom * c_scale_y);
    ESP_LOGI(TAG, "Temperature: %.3f x %.3f pixels a cell, pen %d, band %dx%d at %d,%d", scale_x, scale_y, pen, band->width,
             band->height, band_x, band_y);
    return true;
}

/* "℃": the ring in the upper left corner of its box, the C in the rest of it */
static void draw_ring(void)
{
    int cx = unit_x + degree_r;
    int cy = base_y + (int)lroundf(digits_top * scale_y) + degree_r;
    dgx_solid_circle(band, cx, cy, degree_r, INK);
    if (degree_r > pen) dgx_solid_circle(band, cx, cy, degree_r - pen, 0);
}

static int c_x(void)
{
    return unit_x + (int)lroundf(digit_width * (1 - C_WIDTH_SHARE) * scale_x - c_left * c_scale_x);
}

/*
 * The band, held by the caller, goes to the screen in the color of the ink:
 * all of it when the ink is another, else only where it has been drawn into.
 */
static void band_present(rgb_t ink, bool whole)
{
    static uint16_t lut[256];
    ink = lit(ink);
    for (int i = 0; i < 256; ++i) lut[i] = (uint16_t)dgx_rgb_to_16(ink.r * i / 255, ink.g * i / 255, ink.b * i / 255);
    if (whole) {
        display_show(band_hold, band, band_x, band_y, lut);
    } else {
        display_show_changed(band_hold, band, band_x, band_y, lut);
    }
}

/*
 * A text is written by a pen into the band, and the band goes to the screen
 * as the pen goes. Written with the paper, the pen takes away what it has
 * written before: the same way with the same thickness.
 */
static void write_in_band(int x, int y, const char *text, float sx, float sy, uint32_t paint, rgb_t ink)
{
    dgx_hw_pace_t    pace = dgx_hw_pace(font, BEND_EFFORT);
    dgx_hw_writing_t writing;
    /* the edge of the pen is soft; with the paper for paint such a pen takes the ink off every pixel it touches */
    dgx_hw_writing_begin_xy(&writing, band, x, y, font, text, sx, sy, DGX_PEN_SOFT | pen, paint, false, &pace);
    int64_t start_us = esp_timer_get_time(), pace_us = 0;
    bool    finished;
    do {
        display_pace(&pace_us);
        display_take(band_hold);
        finished = dgx_hw_writing_draw_to(&writing, (paint ? WRITE_TEMPO : ERASE_TEMPO) * (esp_timer_get_time() - start_us) * 1e-6f);
        band_present(ink, false);
        vTaskDelay(1);
    } while (!finished);
}

/* A symbol of the number in its place, written or taken away. */
static void write_symbol(int place, char symbol, uint32_t paint, rgb_t ink)
{
    char one[2];
    write_in_band(places[place].x + (int)lroundf(symbol_shift(symbol) * scale_x), base_y, symbol_text(symbol, one), scale_x, scale_y, paint, ink);
}

static void symbol_frame(const dgx_morph_t *morph, float t, rgb_t ink)
{
    static uint32_t lut[256];
    display_take(glow_hold);
    ink = lit(ink);
    for (int i = 0; i < 256; ++i) lut[i] = dgx_rgb_to_16(ink.r * i / 255, ink.g * i / 255, ink.b * i / 255);
    dgx_morph_glow_set_lut(glow, lut);
    dgx_morph_draw(morph, t, glow_radius, glow_radius, cell, true, dgx_morph_glow_dot, glow);
    display_show_glow(glow_hold, glow, t, symbol_x, symbol_y);
}

/* The weather shown goes into another one. */
static void turn(const shown_t *next)
{
    rgb_t next_color;
    weather_temp_to_rgb(next->temperature, &next_color.r, &next_color.g, &next_color.b);
    if (!has_weather) color = next_color;

    /* another hour: what is written is shown as bright as it lets, before anything is written anew */
    portENTER_CRITICAL(&asked_lock);
    float night = night_wanted;
    portEXIT_CRITICAL(&asked_lock);
    int  level = (int)lroundf((1 - (1 - NIGHT_LIGHT) * night) * LIGHT_STEPS);
    bool relight = has_weather && level != light;
    light = level;
    if (relight) {
        display_take(band_hold);
        band_present(color, true);
    }

    /* the units at the right, the tens before them, the minus right before the number */
    int  degrees = next->temperature < -99 ? 99 : next->temperature > 99 ? 99 : abs(next->temperature);
    char symbols_next[PLACES] = {0, 0, (char)('0' + degrees % 10)};
    if (degrees > 9) symbols_next[1] = (char)('0' + degrees / 10);
    if (next->temperature < 0) symbols_next[degrees > 9 ? 0 : 1] = '-';
    /*
     * The temperature changes seldom, and it is always a pen that writes it:
     * the symbols that change are taken away by the pen the way they were
     * written, then the new ones are written. The first one is followed by "℃".
     */
    if (!has_weather) {
        display_take(band_hold);
        dgx_fill_rectangle(band, 0, 0, band->width, band->height, 0);
        display_give(band_hold);
    }
    for (int i = 0; i < PLACES; ++i) {
        if (places[i].shown && places[i].shown != symbols_next[i]) write_symbol(i, places[i].shown, 0, color);
    }
    for (int i = 0; i < PLACES; ++i) {
        if (symbols_next[i] && places[i].shown != symbols_next[i]) write_symbol(i, symbols_next[i], INK, color);
        places[i].shown = symbols_next[i];
    }
    if (!has_weather) {
        display_take(band_hold);
        draw_ring();
        display_give(band_hold);
        write_in_band(c_x(), c_y, "C", c_scale_x, c_scale_y, INK, color);
    }

    /* a symbol the font has not is an empty box: the dots gather in its middle and fade */
    dgx_bit_matrix_t *next_matrix = dgx_morph_glyph_matrix(symbols, next->symbol);
    bool              symbol_changes = next_matrix && !dgx_matrix_equals(matrix, next_matrix);
    dgx_morph_t      *symbol = next_matrix ? dgx_morph_create_with(matrix, next_matrix, dgx_morph_sources_cells, NULL, &morph_options) : NULL;
    if (!symbol) ESP_LOGE(TAG, "No memory for a morph");

    /* what is written takes the color of the new temperature while the symbol becomes the next one */
    bool recolor = memcmp(&color, &next_color, sizeof(color)) != 0;
    /* a symbol that stays is drawn once more, as bright as the hour lets */
    if (relight && !symbol_changes && !recolor && symbol) symbol_frame(symbol, 1, color);
    if (symbol_changes || recolor) {
        int64_t start_us = esp_timer_get_time(), now_us, symbol_us = 0, longest_us = 0, pace_us = 0;
        int     frames = 0;
        float   t;
        do {
            display_pace(&pace_us);
            now_us = esp_timer_get_time();
            t = dgx_morph_progress(start_us, now_us, MORPH_US);
            float eased = dgx_morph_ease(DGX_MORPH_SMOOTHSTEP, t);
            rgb_t ink = mix(color, next_color, eased);
            if (recolor) {
                display_take(band_hold);
                band_present(ink, true);
            }
            if (symbol && (symbol_changes || recolor)) {
                /* drawn and sent; the wait for the frame before it to be shown is in this time as well */
                int64_t frame_us = esp_timer_get_time();
                symbol_frame(symbol, t, ink);
                frame_us = esp_timer_get_time() - frame_us;
                symbol_us += frame_us;
                if (frame_us > longest_us) longest_us = frame_us;
            }
            ++frames;
            vTaskDelay(1);
        } while (t < 1);
        if (symbol) {
            ESP_LOGI(TAG, "Symbol U+%04lx: %d frames in %lld ms, a frame takes %lld us, the longest one %lld us", (unsigned long)next->symbol, frames,
                     (esp_timer_get_time() - start_us) / 1000, symbol_us / frames, longest_us);
        }
    }

    dgx_morph_destroy(&symbol);
    if (next_matrix) {
        dgx_matrix_destroy(&matrix);
        matrix = next_matrix;
    }
    color = next_color;
    has_weather = true;
}

static void weather_face_task(void *arg)
{
    (void)arg;
    for (;;) {
        shown_t next;
        if (xQueueReceive(queue, &next, portMAX_DELAY) == pdTRUE) turn(&next);
    }
}

void weather_face_show(uint32_t symbol, int temperature)
{
    shown_t next = {.symbol = symbol, .temperature = temperature};
    portENTER_CRITICAL(&asked_lock);
    asked = next;
    has_asked = true;
    portEXIT_CRITICAL(&asked_lock);
    if (queue) xQueueOverwrite(queue, &next);
}

void weather_face_set_night(float night)
{
    portENTER_CRITICAL(&asked_lock);
    night_wanted = night < 0 ? 0 : night > 1 ? 1 : night;
    shown_t again = asked;
    bool    known = has_asked;
    portEXIT_CRITICAL(&asked_lock);
    /* the weather asked for last is asked for once more: the task looks at the hour when it takes it */
    if (queue && known) xQueueOverwrite(queue, &again);
}

bool weather_face_start(const weather_face_config_t *config)
{
    cfg = *config;
    int symbol_width = cfg.symbol_width;
    if (!symbol_layout(cfg.x, symbol_width) || !temperature_layout(cfg.x + symbol_width, cfg.width - symbol_width)) {
        ESP_LOGE(TAG, "No place for the weather in %dx%d, or no memory", cfg.width, cfg.height);
        return false;
    }
    band_hold = display_hold_create();
    glow_hold = display_hold_create();
    if (!band_hold || !glow_hold) return false;
    queue = xQueueCreate(1, sizeof(shown_t));
    if (!queue) return false;
    return xTaskCreate(weather_face_task, "weather_face", 4096, NULL, tskIDLE_PRIORITY + 2, NULL) == pdPASS;
}
