#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dgx_draw.h"
#include "dgx_hw_font.h"
#include "drivers/vscreen.h"
#include "fonts/font0ss.h"

#include "display.h"
#include "status_face.h"

static const char *TAG = "status_face";

#define INK 255
/* The pen is this many cells thick, and writes this many cells of effort in a second; it takes away twice as fast. */
#define PEN_CELLS   9.0f
#define WRITE_TEMPO 600.0f
#define ERASE_TEMPO 1200.0f
#define BEND_EFFORT 6.0f
/* A letter stands written so long, and its place is empty so long, before the pen comes back. */
#define STAND_US 400000LL
#define EMPTY_US 250000LL
/* The sign of Wi-Fi has another arc every so long; with all of them it stands for two turns. */
#define ARC_US 300000LL
#define ARCS   3
/* The signs are looked at so often. */
#define STEP_MS 20

enum { SIGN_WIFI, SIGN_TIME, SIGN_WEATHER, SIGNS };

/* a letter on its way: written, standing, taken away, gone */
typedef enum { PEN_WRITES, PEN_STANDS, PEN_ERASES, PEN_AWAY } pen_stage_t;

typedef struct {
    bool             gone;    /* what it waits for has come: the place is empty for good */
    bool             drawn;   /* something of it is in the band */
    int              x;       /* of its place in the band */
    /* the letters */
    const char      *text;
    pen_stage_t      stage;
    int64_t          since_us;
    dgx_hw_writing_t writing;
    /* the sign of Wi-Fi */
    int              arcs;    /* drawn now; -1: nothing */
} sign_t;

static status_face_config_t cfg;
static dgx_font_t          *font;
static dgx_screen_t        *band;
static SemaphoreHandle_t    hold;
static uint16_t             lut[256];
static uint16_t             lut_ink;
static bool                 lut_made;
static portMUX_TYPE         lock = portMUX_INITIALIZER_UNLOCKED;
static float                night_wanted; /* 0: day .. 1: night, under the lock */
static sign_t               signs[SIGNS];
static int                  place;        /* width of the place of a sign */
static int                  pen;
static float                scale;        /* of the letters, pixels in a cell */
static int                  base_y;       /* of their line in the band */

/*
 * The table of colors for the ink of this moment. The band must be held: the
 * table is a part of what goes to the display. True: the table is another.
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
    /* paper is color 0 of the band and ink is color 255 */
    for (int i = 0; i < 256; ++i) {
        int lr = ((cfg.paper >> 11) & 31) + (((ink >> 11) & 31) - ((cfg.paper >> 11) & 31)) * i / 255;
        int lg = ((cfg.paper >> 5) & 63) + (((ink >> 5) & 63) - ((cfg.paper >> 5) & 63)) * i / 255;
        int lb = (cfg.paper & 31) + ((ink & 31) - (cfg.paper & 31)) * i / 255;
        lut[i] = (uint16_t)(lr << 11 | lg << 5 | lb);
    }
    lut_ink = ink;
    lut_made = true;
    return true;
}

static void clear(sign_t *sign)
{
    if (!sign->drawn) return;
    dgx_fill_rectangle(band, sign->x, 0, place, band->height, 0);
    sign->drawn = false;
}

/* The pen at the start of the letter, to write it or to take it away the way it was written. */
static void pen_begin(sign_t *sign, pen_stage_t stage, int64_t now_us)
{
    int left, top, right, bottom;
    sign->stage = stage;
    sign->since_us = now_us;
    if (stage != PEN_WRITES && stage != PEN_ERASES) return;
    if (!dgx_hw_text_box(font, sign->text, &left, &top, &right, &bottom)) return;
    static dgx_hw_pace_t pace;
    pace = dgx_hw_pace(font, BEND_EFFORT);
    int x = sign->x + (int)lroundf((place - (right - left + 1) * scale) / 2 - left * scale);
    dgx_hw_writing_begin_xy(&sign->writing, band, x, base_y, font, sign->text, scale, scale, DGX_PEN_SOFT | pen, stage == PEN_WRITES ? INK : 0, false,
                            &pace);
}

/* A letter: written by the pen, standing, taken away by it, away for a while, and again. */
static void letter_step(sign_t *sign, int64_t now_us)
{
    float seconds = (now_us - sign->since_us) * 1e-6f;
    switch (sign->stage) {
    case PEN_WRITES:
        sign->drawn = true;
        if (dgx_hw_writing_draw_to(&sign->writing, WRITE_TEMPO * seconds)) pen_begin(sign, PEN_STANDS, now_us);
        break;
    case PEN_STANDS:
        if (now_us - sign->since_us >= STAND_US) pen_begin(sign, PEN_ERASES, now_us);
        break;
    case PEN_ERASES:
        if (dgx_hw_writing_draw_to(&sign->writing, ERASE_TEMPO * seconds)) {
            /* a pen that takes away leaves nothing, but a place cleared is sure to be empty */
            clear(sign);
            pen_begin(sign, PEN_AWAY, now_us);
        }
        break;
    case PEN_AWAY:
        if (now_us - sign->since_us >= EMPTY_US) pen_begin(sign, PEN_WRITES, now_us);
        break;
    }
}

/* The sign of Wi-Fi: a dot, and so many arcs over it. */
static void wifi_draw(sign_t *sign, int arcs)
{
    if (arcs == sign->arcs) return;
    dgx_fill_rectangle(band, sign->x, 0, place, band->height, 0);
    int h = band->height, dot = pen > 1 ? pen / 2 + 1 : 1;
    int cx = sign->x + place / 2, cy = h - 1 - dot;
    dgx_solid_circle(band, cx, cy, dot, INK);
    for (int i = 1; i <= arcs; ++i) {
        /* a quarter of a circle as one curve of three points: its middle point is where the two tangents meet */
        float          r = (float)(cy - pen) * i / (ARCS * 1.06f);
        dgx_point_2d_t arc[3] = {
            {(int16_t)lroundf(cx - r * 0.7071f), (int16_t)lroundf(cy - r * 0.7071f)},
            {(int16_t)cx, (int16_t)lroundf(cy - r * 1.4142f)},
            {(int16_t)lroundf(cx + r * 0.7071f), (int16_t)lroundf(cy - r * 0.7071f)},
        };
        dgx_draw_bezier3(band, arc, 0, DGX_PEN_SOFT | pen, INK);
    }
    sign->arcs = arcs;
    sign->drawn = true;
}

static void status_face_task(void *arg)
{
    (void)arg;
    int64_t start_us = esp_timer_get_time();
    for (;;) {
        int64_t now_us = esp_timer_get_time();
        bool    connected = cfg.connected && cfg.connected();
        display_take(hold);
        bool whole = recolor();

        /* the network: until it is there for the first time */
        sign_t *wifi = &signs[SIGN_WIFI];
        if (!wifi->gone) {
            if (connected) {
                clear(wifi);
                wifi->gone = true;
            } else {
                /* no arc, one, two, three, and three once more */
                int turn = (int)((now_us - start_us) / ARC_US) % (ARCS + 2);
                wifi_draw(wifi, turn > ARCS ? ARCS : turn);
            }
        }
        /* the time and the weather are asked for when the network is there */
        for (int i = SIGN_TIME; i < SIGNS; ++i) {
            sign_t *sign = &signs[i];
            if (sign->gone || !wifi->gone) continue;
            bool come = i == SIGN_TIME ? (cfg.has_time && cfg.has_time()) : (cfg.has_weather && cfg.has_weather());
            if (come) {
                clear(sign);
                sign->gone = true;
                continue;
            }
            if (sign->stage == PEN_AWAY && !sign->since_us) pen_begin(sign, PEN_WRITES, now_us);
            letter_step(sign, now_us);
        }

        if (whole) {
            display_show(hold, band, cfg.x, cfg.y, lut);
        } else {
            display_show_changed(hold, band, cfg.x, cfg.y, lut);
        }
        if (signs[SIGN_WIFI].gone && signs[SIGN_TIME].gone && signs[SIGN_WEATHER].gone) break;
        vTaskDelay(pdMS_TO_TICKS(STEP_MS));
    }
    ESP_LOGI(TAG, "The network, the time and the weather are there %.1f s after the start: no sign is left",
             (esp_timer_get_time() - start_us) * 1e-6f);
    /* the band is not needed any more, but the display task may still have it on its way */
    display_take(hold);
    display_give(hold);
    dgx_screen_destroy(&band);
    vTaskDelete(NULL);
}

void status_face_set_night(float night)
{
    portENTER_CRITICAL(&lock);
    night_wanted = night < 0 ? 0 : night > 1 ? 1 : night;
    portEXIT_CRITICAL(&lock);
}

bool status_face_start(const status_face_config_t *config)
{
    cfg = *config;
    font = font0ss();
    /* a place is one and a half times as wide as the row is tall */
    place = cfg.height * 3 / 2;
    if (cfg.height < 8 || cfg.width < SIGNS * place) {
        ESP_LOGW(TAG, "No place for the signs in %dx%d", cfg.width, cfg.height);
        return false;
    }
    int left, top, right, bottom;
    if (!dgx_hw_text_box(font, "W", &left, &top, &right, &bottom)) return false;
    /* a capital letter as tall as the row, less a pixel above and under it */
    scale = (float)(cfg.height - 2) / (bottom - top + 1);
    pen = (int)lroundf(scale * PEN_CELLS);
    if (pen < 1) pen = 1;
    scale = (float)(cfg.height - 2 - pen) / (bottom - top + 1);
    base_y = 1 + (pen + 1) / 2 - (int)lroundf(top * scale);
    band = dgx_vscreen_init(SIGNS * place, cfg.height, 8, DgxScreenRGB);
    hold = band ? display_hold_create() : NULL;
    if (!band || !hold) {
        ESP_LOGE(TAG, "No memory for the signs");
        return false;
    }
    /* a batch that is never closed: the band keeps count of where it has been drawn into */
    dgx_screen_progress_up(band);
    for (int i = 0; i < SIGNS; ++i) {
        signs[i].x = i * place;
        signs[i].arcs = -1;
        signs[i].stage = PEN_AWAY;
    }
    signs[SIGN_TIME].text = "Z";
    signs[SIGN_WEATHER].text = "W";
    /* no weather is waited for: its place stays empty */
    if (!cfg.has_weather) signs[SIGN_WEATHER].gone = true;
    ESP_LOGI(TAG, "Signs: %dx%d at %d,%d, a place %d wide, letters %.3f pixels a cell, pen %d", band->width, band->height, cfg.x, cfg.y, place,
             scale, pen);
    return xTaskCreate(status_face_task, "status_face", 4096, NULL, tskIDLE_PRIORITY + 2, NULL) == pdPASS;
}
