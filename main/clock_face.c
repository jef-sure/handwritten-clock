#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "dgx_draw.h"
#include "dgx_hw_font.h"
#include "drivers/vscreen.h"
#include "fonts/font0ss.h"

#include "clock_face.h"
#include "display.h"

static const char *TAG = "clock_face";

/*
 * A digit becomes the next one in so long, and is ready when the second
 * comes. Seconds go faster: a digit that changes every second seemed to run
 * without ever standing still when it took half of the second.
 */
#define BIG_MORPH_US   500000LL
#define SMALL_MORPH_US 300000LL
/*
 * A way that is started this much late still ends with the second: a fifth
 * of its time is a tenth of the way, and the jump over it is not seen.
 */
#define LATE(way_us) ((way_us) / 5)
/* When nothing moves the clock is looked at this often: it may have been set. */
#define LOOK_US 100000LL
/* The pen is this many cells thick. */
#define PEN_CELLS 9.0f
/*
 * The first time is written by a pen, digit after digit, as the name of the
 * place is: so many cells of effort in a second, and every straight piece of
 * a digit adds so many cells. Digits are large and few: the pen is faster
 * than in a line of text.
 */
#define WRITE_TEMPO 3000.0f
#define BEND_EFFORT 6.0f
/* The colon takes this share of the cell of a digit; with seconds in the line it has a cell of its own, eight cells in all. */
#define COLON_SHARE 0.45f
/* Seconds are this small beside hours and minutes: there for one who looks for them. */
#define SECONDS_SHARE (1 / 6.0f)

#define INK 255
/* The color of the day goes into the color of the night, and back, from so long before sunset or sunrise till so long after it. */
#define DUSK_S 1800

typedef struct {
    dgx_hw_morph_text_t *morph;  /* of one symbol: its strokes are paired by the shortest way */
    int                  x, w;   /* of the cell in the band, and how wide it is */
    int                  shown;  /* the digit the morph ends with; -1: none yet */
    bool                 moving; /* the morph is on its way in this second */
} cell_t;

/* Digits of one size: a frame of them is drawn in a band, 8 bits a pixel, and sent to the screen whole. */
typedef struct {
    dgx_screen_t     *band;
    SemaphoreHandle_t hold;   /* of the band: taken to draw into it */
    int           x, y;   /* of the band on the screen */
    int           base_y; /* the base line of the digits in the band */
    float         scale_x, scale_y; /* pixels in a cell of the font across and down */
    int           pen;
    int           number;
    cell_t        cells[6];
    float         shift[10]; /* what puts each digit in the middle of its cell, in cells of the font */
    int           colon_x[2], colon_y[2], colon_r; /* a colon after every two digits; colon_r 0: none */
} group_t;

static clock_face_config_t cfg;
static dgx_font_t         *font;
static uint16_t            lut[256];
static uint16_t            lut_ink; /* the ink the table is made for */
static float               night_told = -1; /* what on_night was told last; -1: nothing yet */
static portMUX_TYPE        sun_lock = portMUX_INITIALIZER_UNLOCKED;
static time_t              sun_rise, sun_set; /* 0: not known */
static group_t             big;   /* hours and minutes, and seconds when they are in the line */
static group_t             small; /* seconds, in the corner above the minutes */
static int                 left[10], right[10], top, tall, widest; /* the boxes of the digits, in cells of the font */

static float progress(int64_t start_us, int64_t now_us, int64_t duration_us)
{
    if (now_us <= start_us) return 0;
    if (now_us - start_us >= duration_us) return 1;
    float t = (float)(now_us - start_us) / (float)duration_us;
    return t * t * (3 - 2 * t);
}

/* A band for so many digits, each in a cell pitch wide; its place on the screen is set by the caller. */
static bool group_make(group_t *group, int number, float pitch, float colon, float scale_x, float scale_y, int pen)
{
    /* half a pen is the stroke itself, the rest is room: a curve on its way may go beyond both digits */
    int width = (int)lroundf(number * pitch + (number / 2 - 1) * colon), height = (int)lroundf(tall * scale_y) + 3 * pen;
    group->band = dgx_vscreen_init(width, height, 8, DgxScreenRGB);
    if (!group->band) return false;
    group->hold = display_hold_create();
    if (!group->hold) return false;
    /* a batch that is never closed: the band keeps count of where it has been drawn into */
    dgx_screen_progress_up(group->band);
    group->scale_x = scale_x;
    group->scale_y = scale_y;
    group->pen = pen;
    group->number = number;
    group->base_y = (int)lroundf((height - tall * scale_y) / 2 - top * scale_y);
    for (int d = 0; d < 10; ++d) group->shift[d] = (pitch / scale_x - (right[d] - left[d] + 1)) / 2 - left[d];
    float x = 0;
    for (int i = 0; i < number; ++i) {
        if (i && i % 2 == 0) {
            group->colon_x[i / 2 - 1] = (int)lroundf(x + colon / 2);
            x += colon;
        }
        group->cells[i].x = (int)lroundf(x);
        group->cells[i].w = (int)lroundf(x + pitch) - group->cells[i].x;
        group->cells[i].shown = -1;
        x += pitch;
    }
    group->colon_y[0] = group->base_y + (int)lroundf((top + tall / 3.0f) * scale_y);
    group->colon_y[1] = group->base_y + (int)lroundf((top + 2 * tall / 3.0f) * scale_y);
    group->colon_r = colon > 0 ? (int)lroundf(pen * 0.75f) : 0;
    return true;
}

/*
 * Hours and minutes fill the place: the widest digit takes a cell across, the
 * tallest one the height, each axis at its own size. Seconds stand in a row
 * above them, at the right end.
 */
static bool layout(void)
{
    int lowest = 0;
    for (int d = 0; d < 10; ++d) {
        char text[2] = {(char)('0' + d), 0};
        int  t, b;
        if (!dgx_hw_text_box(font, text, &left[d], &t, &right[d], &b)) return false;
        if (d == 0 || t < top) top = t;
        if (d == 0 || b > lowest) lowest = b;
        if (right[d] - left[d] + 1 > widest) widest = right[d] - left[d] + 1;
    }
    tall = lowest - top + 1;
    bool  small_seconds = cfg.seconds == CLOCK_SECONDS_SMALL, line = cfg.seconds == CLOCK_SECONDS_LINE;
    float share = small_seconds ? SECONDS_SHARE : 0;
    float colon_share = line ? 1 : COLON_SHARE;
    int   number = line ? 6 : 4;
    float pitch = cfg.width / (number + (number / 2 - 1) * colon_share);
    float scale_x = 0, scale_y = 0;
    int   pen = 1, small_pen = 1;
    for (int pass = 0; pass < 2; ++pass) {
        scale_x = (pitch - 2 * pen) / widest;
        scale_y = (float)(cfg.height - 3 * pen - (small_seconds ? 3 * small_pen : 0)) / (tall * (1 + share));
        if (!(scale_x > 0 && scale_y > 0)) return false;
        /* the pen is round: it goes by the size between the two, 5 pixels for a digit of 40x120 as in the clock of 2021 */
        float mean = sqrtf(scale_x * scale_y);
        pen = (int)lroundf(mean * PEN_CELLS);
        if (pen < 1) pen = 1;
        small_pen = (int)lroundf(mean * share * PEN_CELLS);
        if (small_pen < 1) small_pen = 1;
    }
    if (!group_make(&big, number, pitch, pitch * colon_share, scale_x, scale_y, pen)) return false;
    int height = big.band->height;
    if (small_seconds) {
        if (!group_make(&small, 2, widest * scale_x * share + 3 * small_pen, 0, scale_x * share, scale_y * share, small_pen)) return false;
        height += small.band->height;
        small.x = cfg.x + cfg.width - small.band->width - pen;
        small.y = cfg.y + (cfg.height - height) / 2;
    }
    big.x = cfg.x + (cfg.width - big.band->width) / 2;
    big.y = cfg.y + (cfg.height - height) / 2 + height - big.band->height;
    return true;
}

/* The cell goes from the digit it shows to another one, -1: to none; a digit that stays is left as it is. */
static bool cell_turn(const group_t *group, cell_t *cell, int digit)
{
    cell->moving = false;
    if (cell->shown == digit) return true;
    char from[2] = {cell->shown < 0 ? 0 : (char)('0' + cell->shown), 0};
    char to[2] = {digit < 0 ? 0 : (char)('0' + digit), 0};
    dgx_hw_morph_text_destroy(&cell->morph);
    cell->morph = dgx_hw_morph_text_create(font, from, to, false);
    if (!cell->morph) return false;
    /* a digit grows from nothing, and goes into it, in its own place */
    dgx_hw_morph_text_shift(cell->morph, group->shift[cell->shown < 0 ? digit : cell->shown], 0, group->shift[digit < 0 ? cell->shown : digit], 0);
    cell->shown = digit;
    cell->moving = true;
    return true;
}

/* Gives the group its next digits; false when none of them changes: nothing to draw. */
static bool group_turn(group_t *group, const int *digits)
{
    bool moving = false;
    for (int i = 0; i < group->number; ++i) {
        if (!cell_turn(group, &group->cells[i], digits[i])) ESP_LOGE(TAG, "No memory for a morph");
        moving = moving || group->cells[i].moving;
    }
    return moving;
}

/*
 * A frame of the way that started at start_us and takes duration_us. How
 * far the digits are on it is read from the clock when the band is back
 * with this task: the wait for the band is not a part of the way. Only the
 * cells of the digits that move are cleared and drawn anew, and only they
 * are to be shown: a digit on its way to another never leaves its cell.
 * That is measured for all pairs of digits of font0ss, in both sizes of the
 * clock; a digit that stays, and the colon, are left as they were drawn.
 * Returns true when the frame is the end of the way; drawn_us, when given,
 * grows by what the frame took to draw.
 */
static bool group_frame(group_t *group, int64_t start_us, int64_t duration_us, int64_t *drawn_us)
{
    dgx_screen_t *band = group->band;
    display_take(group->hold);
    int64_t now_us = esp_timer_get_time();
    float   t = progress(start_us, now_us, duration_us);
    for (int i = 0; i < group->number; ++i) {
        const cell_t *cell = &group->cells[i];
        if (!cell->moving || !cell->morph || !cell->morph->length) continue;
        dgx_fill_rectangle(band, cell->x, 0, cell->w, band->height, 0);
        dgx_hw_morph_draw_xy(cell->morph->letters[0], t, band, cell->x, group->base_y, group->scale_x, group->scale_y, group->pen, INK);
    }
    display_show_changed(group->hold, band, group->x, group->y, lut);
    if (drawn_us) *drawn_us += esp_timer_get_time() - now_us;
    return now_us - start_us >= duration_us;
}

/* The band as it is, all of it: its table of colors has changed. */
static void group_show(group_t *group)
{
    display_take(group->hold);
    display_show(group->hold, group->band, group->x, group->y, lut);
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
}

/* How much of the night it is at this moment: 0 by day, 1 by night, between them near sunrise and sunset. */
static float night_at(time_t now)
{
    portENTER_CRITICAL(&sun_lock);
    time_t rise = sun_rise, set = sun_set;
    portEXIT_CRITICAL(&sun_lock);
    if (!rise || !set) return 0;
    if (llabs((long long)(now - rise)) < DUSK_S) return 1 - (float)(now - (rise - DUSK_S)) / (2 * DUSK_S);
    if (llabs((long long)(now - set)) < DUSK_S) return (float)(now - (set - DUSK_S)) / (2 * DUSK_S);
    return now >= rise && now < set ? 0 : 1;
}

/* The ink of the time for so much of the night. */
static uint16_t ink_of(float night)
{
    int r = (int)lroundf(((cfg.ink >> 11) & 31) * (1 - night) + ((cfg.ink_night >> 11) & 31) * night);
    int g = (int)lroundf(((cfg.ink >> 5) & 63) * (1 - night) + ((cfg.ink_night >> 5) & 63) * night);
    int b = (int)lroundf((cfg.ink & 31) * (1 - night) + (cfg.ink_night & 31) * night);
    return (uint16_t)(r << 11 | g << 5 | b);
}

void clock_face_set_sun(time_t sunrise, time_t sunset)
{
    portENTER_CRITICAL(&sun_lock);
    sun_rise = sunrise;
    sun_set = sunset;
    portEXIT_CRITICAL(&sun_lock);
}

/* The digits the group has been given are written by a pen into an empty band, the colon in its turn. */
static void group_write(group_t *group)
{
    dgx_screen_t *band = group->band;
    dgx_hw_pace_t pace = dgx_hw_pace(font, BEND_EFFORT);
    /* the band is held from here to the end, but while what is written so far is shown */
    display_take(group->hold);
    dgx_fill_rectangle(band, 0, 0, band->width, band->height, 0);
    for (int i = 0; i < group->number; ++i) {
        cell_t *cell = &group->cells[i];
        if (i && i % 2 == 0 && group->colon_r) {
            dgx_solid_circle(band, group->colon_x[i / 2 - 1], group->colon_y[0], group->colon_r, INK);
            dgx_solid_circle(band, group->colon_x[i / 2 - 1], group->colon_y[1], group->colon_r, INK);
        }
        /* what is written is then drawn as the end of a morph */
        cell->moving = false;
        if (cell->shown < 0) continue;
        char             text[2] = {(char)('0' + cell->shown), 0};
        dgx_hw_writing_t writing;
        dgx_hw_writing_begin_xy(&writing, band, cell->x + (int)lroundf(group->shift[cell->shown] * group->scale_x), group->base_y, font, text,
                                group->scale_x, group->scale_y, group->pen, INK, false, &pace);
        int64_t start_us = esp_timer_get_time();
        bool    finished;
        do {
            finished = dgx_hw_writing_draw_to(&writing, WRITE_TEMPO * (esp_timer_get_time() - start_us) * 1e-6f);
            /* only where the pen has been since the last time */
            display_show_changed(group->hold, band, group->x, group->y, lut);
            vTaskDelay(1);
            display_take(group->hold);
        } while (!finished);
    }
    display_show_changed(group->hold, band, group->x, group->y, lut);
}

/* The digits of the time, cell by cell; an hour before ten has no digit in front of it: 0:00, not 00:00. */
static void digits_of(const struct tm *tm, int digits[6])
{
    digits[0] = tm->tm_hour < 10 ? -1 : tm->tm_hour / 10;
    digits[1] = tm->tm_hour % 10;
    digits[2] = tm->tm_min / 10;
    digits[3] = tm->tm_min % 10;
    digits[4] = tm->tm_sec / 10;
    digits[5] = tm->tm_sec % 10;
}

static void clock_face_task(void *arg)
{
    (void)arg;
    /* the empty screen is filled by a pen; after that a digit becomes the next one */
    {
        struct timeval now;
        struct tm      tm;
        gettimeofday(&now, NULL);
        localtime_r(&now.tv_sec, &tm);
        int     digits[6];
        digits_of(&tm, digits);
        int    *seconds = digits + 4;
        int64_t start_us = esp_timer_get_time();
        group_turn(&big, digits);
        group_write(&big);
        if (cfg.seconds == CLOCK_SECONDS_SMALL) {
            group_turn(&small, seconds);
            group_write(&small);
        }
        ESP_LOGI(TAG, "%d:%02d is written in %.1f s", tm.tm_hour, tm.tm_min, (esp_timer_get_time() - start_us) * 1e-6f);
    }
    int64_t big_start_us = 0, small_start_us = 0, drawing_us = 0;
    bool    big_on = false, small_on = false; /* the band is on its way */
    time_t  looked = 0, inked = 0; /* the second the digits of the large band, and the ink, were last looked at for */
    int     frames = 0;
    for (;;) {
        struct timeval now;
        gettimeofday(&now, NULL);
        /* the frames go by this clock: the other one may be set at any moment */
        int64_t now_us = esp_timer_get_time();
        /* the second in which a way of the large band ends, if it starts now */
        time_t then = now.tv_sec + (now.tv_usec + BIG_MORPH_US) / 1000000LL;
        if (!big_on && !small_on && then != inked) {
            inked = then;
            /* another ink: what does not move is shown in it as well */
            float night = night_at(then);
            if (cfg.on_night && night != night_told) {
                night_told = night;
                cfg.on_night(night);
            }
            uint16_t ink = ink_of(night);
            if (ink != lut_ink) {
                /* the table belongs to both bands: it changes when neither is on its way */
                display_take(big.hold);
                if (small.hold) display_take(small.hold);
                make_lut(ink);
                if (small.hold) display_give(small.hold);
                display_give(big.hold);
                /* both bands whole, in the new colors: the frames below show only the cells that move */
                if (cfg.seconds == CLOCK_SECONDS_SMALL) group_show(&small);
                group_show(&big);
            }
        }
        /*
         * The large band goes to what the time is when its way ends. A
         * minute that comes is seen as long before it as the way takes, and
         * the way ends with the second. A clock that has been set, which
         * the network does at any moment of a second, or another time zone,
         * is seen at once: the way starts now and is as long as any other,
         * not what is left of it till the second.
         */
        if (!big_on && then != looked) {
            looked = then;
            struct tm tm;
            localtime_r(&then, &tm);
            int digits[6];
            digits_of(&tm, digits);
            if (group_turn(&big, digits)) {
                int64_t late_us = now.tv_usec + BIG_MORPH_US - 1000000LL;
                big_start_us = late_us >= 0 && late_us <= LATE(BIG_MORPH_US) ? now_us - late_us : now_us;
                big_on = true;
                frames = 0;
                drawing_us = 0;
            }
        }
        /*
         * Small seconds do not hurry after a clock that has been set: their
         * way starts where it ends with the second, and when that moment is
         * missed they wait for the next one.
         */
        if (cfg.seconds == CLOCK_SECONDS_SMALL && !small_on) {
            int64_t late_us = now.tv_usec + SMALL_MORPH_US - 1000000LL;
            if (late_us >= 0 && late_us <= LATE(SMALL_MORPH_US)) {
                time_t    next = now.tv_sec + 1;
                struct tm tm;
                localtime_r(&next, &tm);
                int digits[6];
                digits_of(&tm, digits);
                if (group_turn(&small, digits + 4)) {
                    small_start_us = now_us - late_us;
                    small_on = true;
                }
            }
        }
        if (big_on || small_on) {
            /* each band goes its own way; the last frame of a way is its end */
            if (big_on) {
                ++frames;
                if (group_frame(&big, big_start_us, BIG_MORPH_US, &drawing_us)) {
                    big_on = false;
                    ESP_LOGD(TAG, "%d frames of the large band in %lld ms, a frame takes %lld us to draw", frames,
                             (esp_timer_get_time() - big_start_us) / 1000, drawing_us / frames);
                }
            }
            if (small_on && group_frame(&small, small_start_us, SMALL_MORPH_US, NULL)) small_on = false;
            vTaskDelay(1);
            continue;
        }
        /* nothing moves: till a way may start, and not for long, for the clock may be set meanwhile */
        int64_t wait_us = 1000000LL - BIG_MORPH_US - now.tv_usec;
        if (wait_us <= 0) wait_us += 1000000LL;
        if (cfg.seconds == CLOCK_SECONDS_SMALL) {
            int64_t small_us = 1000000LL - SMALL_MORPH_US - now.tv_usec;
            if (small_us <= 0) small_us += 1000000LL;
            if (small_us < wait_us) wait_us = small_us;
        }
        if (wait_us > LOOK_US) wait_us = LOOK_US;
        vTaskDelay(pdMS_TO_TICKS(wait_us / 1000) + 1);
    }
}

bool clock_face_top_row(int *x, int *y, int *width, int *height)
{
    if (cfg.seconds != CLOCK_SECONDS_SMALL || !small.band) return false;
    *x = cfg.x + big.pen;
    *y = small.y;
    *width = small.x - big.pen - *x;
    *height = small.band->height;
    return *width > 0;
}

bool clock_face_start(const clock_face_config_t *config)
{
    cfg = *config;
    font = font0ss();
    if (!layout()) {
        ESP_LOGE(TAG, "No place for the time in %dx%d, or no memory", cfg.width, cfg.height);
        return false;
    }
    make_lut(cfg.ink);
    ESP_LOGI(TAG, "Place %dx%d: %.3f x %.3f pixels a cell, pen %d, band %dx%d at %d,%d", cfg.width, cfg.height, big.scale_x, big.scale_y, big.pen,
             big.band->width, big.band->height, big.x, big.y);
    if (cfg.seconds == CLOCK_SECONDS_SMALL) {
        ESP_LOGI(TAG, "Seconds: %.3f x %.3f pixels a cell, pen %d, band %dx%d at %d,%d", small.scale_x, small.scale_y, small.pen, small.band->width,
                 small.band->height, small.x, small.y);
    }
    return xTaskCreate(clock_face_task, "clock_face", 4096, NULL, tskIDLE_PRIORITY + 2, NULL) == pdPASS;
}
