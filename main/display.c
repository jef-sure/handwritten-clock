#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "bus/dgx_bus_protocols.h"
#if CONFIG_DGX_ENABLE_DSI
#include "bus/dgx_dsi_esp32.h"
#endif
#include "drivers/vscreen.h"

#include "display.h"

static const char *TAG = "display";

/* What to show: a virtual screen through its table of colors, or a frame of dots. */
typedef struct {
    dgx_screen_t     *band;
    uint16_t         *lut;
    dgx_morph_glow_t *glow;
    float             t;
    int               x, y;
    int               left, top, width, height; /* the part of the virtual screen to show; width 0: all of it */
    SemaphoreHandle_t hold; /* of the virtual screen: it comes with the message and goes back when the screen is on the display */
} display_message_t;

/* as many messages may wait as there are virtual screens: a screen that is on its way cannot be sent once more */
#define MESSAGES 8

static dgx_screen_t        *display;
static dgx_bus_protocols_t *display_bus;
static QueueHandle_t        queue;
static void put(const display_message_t *message)
{
    if (message->band && message->width) {
        dgx_vscreen8_region_to_screen16(display, message->x + message->left, message->y + message->top, message->band, message->left,
                                        message->top, message->width, message->height, message->lut, false);
    } else if (message->band) {
        dgx_vscreen8_to_screen16(display, message->x, message->y, message->band, message->lut, false);
    }
    if (message->glow) dgx_morph_glow_present(message->glow, message->t, display, message->x, message->y);
    /* the screen is its owner's again */
    xSemaphoreGive(message->hold);
}

static void display_task(void *arg)
{
    (void)arg;
    display_message_t message;
    for (;;) {
        if (!xQueueReceive(queue, &message, portMAX_DELAY)) continue;
        /*
         * What waits by now goes to the display as one batch: the time, the
         * weather and the name of the place that came together are one
         * picture. To a display behind SPI a batch means nothing. Only what
         * waits now is taken: those who ask do not stop asking.
         */
        int more = (int)uxQueueMessagesWaiting(queue);
        dgx_screen_progress_up(display);
        put(&message);
        while (more-- > 0 && xQueueReceive(queue, &message, 0)) put(&message);
        dgx_screen_progress_down(display);
    }
}

SemaphoreHandle_t display_hold_create(void)
{
    /* a binary semaphore as the word that the screen is back, see display.h; made empty, so the word is said once here */
    SemaphoreHandle_t hold = xSemaphoreCreateBinary();
    if (hold) xSemaphoreGive(hold);
    return hold;
}

void display_take(SemaphoreHandle_t hold)
{
    xSemaphoreTake(hold, portMAX_DELAY);
}

void display_pace(int64_t *next_us)
{
    /* how long a frame takes the bus of a display on MIPI-DSI tells; a display behind SPI has no frames */
    uint32_t period = 0;
#if CONFIG_DGX_ENABLE_DSI
    if (display_bus && display_bus->bus_type == DGX_BUS_DSI) period = dgx_dsi_frame_us(display_bus);
#endif
    if (!period) return;
    int64_t now_us = esp_timer_get_time();
    if (*next_us <= now_us) {
        /* the first picture of a row, or one that took longer than a frame: the count starts here */
        *next_us = now_us + period;
        return;
    }
    do {
        vTaskDelay(1);
    } while (esp_timer_get_time() < *next_us);
    /* from moment to moment, not from waking to waking: a tick of the system late does not add up */
    *next_us += period;
}

void display_give(SemaphoreHandle_t hold)
{
    xSemaphoreGive(hold);
}

/* The screen goes with the message: it is not its sender's until the display task gives it back. */
static void show(display_message_t *message)
{
    if (!queue) {
        ESP_LOGE(TAG, "Nothing is shown: the display task is not started");
        xSemaphoreGive(message->hold);
        return;
    }
    xQueueSend(queue, message, portMAX_DELAY);
}

void display_show(SemaphoreHandle_t hold, dgx_screen_t *band, int x, int y, uint16_t *lut)
{
    display_message_t message = {.band = band, .lut = lut, .x = x, .y = y, .hold = hold};
    /* all of it goes: what has changed in it is not asked for again */
    int left, top, width, height;
    dgx_screen_take_dirty(band, &left, &top, &width, &height);
    show(&message);
}

void display_show_changed(SemaphoreHandle_t hold, dgx_screen_t *band, int x, int y, uint16_t *lut)
{
    display_message_t message = {.band = band, .lut = lut, .x = x, .y = y, .hold = hold};
    if (!dgx_screen_take_dirty(band, &message.left, &message.top, &message.width, &message.height)) {
        xSemaphoreGive(hold);
        return;
    }
    show(&message);
}

void display_show_glow(SemaphoreHandle_t hold, dgx_morph_glow_t *glow, float t, int x, int y)
{
    /*
     * Showing a frame of dots takes it away: the dots of the next one are
     * drawn into an empty frame. Two frames drawn one over another would be
     * shown as one, and then as nothing. That cannot be: the frame is not
     * its owner's until it has been shown.
     */
    display_message_t message = {.glow = glow, .t = t, .x = x, .y = y, .hold = hold};
    show(&message);
}

void display_tune(void)
{
#if CONFIG_DGX_ENABLE_DSI
    if (!display_bus || display_bus->bus_type != DGX_BUS_DSI) return;
    /*
     * When a piece of a picture goes by the hardware of the chip and when
     * by the processor, and how much room the bus leaves before the scan of
     * the display, is decided by numbers that depend on the chip, its
     * memory and the display. The bus starts with those of the board dgx
     * was written on, and measures the board at hand when asked: in memory
     * of its own, nothing on the screen changes, 0.07 s. So it is done
     * once, the numbers are kept, and at the next start the bus gets them
     * from there. They are kept where any program of dgx on this board
     * keeps them, as its example dsi_demo does: numbers of another panel
     * or color depth the bus does not take, and the board is measured anew.
     */
    dgx_dsi_tuning_t tuning;
    size_t           size = sizeof(tuning);
    nvs_handle_t     nvs;
    if (nvs_open("dgx", NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGW(TAG, "No NVS: the DSI bus keeps the numbers it has");
        return;
    }
    if (nvs_get_blob(nvs, "dsi_tuning", &tuning, &size) == ESP_OK && size == sizeof(tuning) && dgx_dsi_tune(display_bus, &tuning)) {
        ESP_LOGI(TAG, "The DSI bus has the numbers of this board kept in NVS");
    } else if (dgx_dsi_bench(display_bus, &tuning) && dgx_dsi_tune(display_bus, &tuning)) {
        nvs_set_blob(nvs, "dsi_tuning", &tuning, sizeof(tuning));
        nvs_commit(nvs);
        ESP_LOGI(TAG, "The board is measured, the numbers of the DSI bus are kept in NVS");
    } else {
        ESP_LOGW(TAG, "The board could not be measured: the DSI bus keeps the numbers it has");
    }
    nvs_close(nvs);
#endif
}

bool display_start(void)
{
    if (queue) return true;
    if (!display) return false;
    queue = xQueueCreate(MESSAGES, sizeof(display_message_t));
    if (!queue) return false;
    /* above those who ask: what is asked for goes to the display at once */
    return xTaskCreate(display_task, "display", 4096, NULL, tskIDLE_PRIORITY + 3, NULL) == pdPASS;
}

/*
 * The display of the board is the one whose driver is on in the configuration.
 * Everything above knows nothing of it but a screen of 16 bits a pixel.
 */
#if CONFIG_DGX_ENABLE_JD9165

#include "bus/dgx_dsi_esp32.h"
#include "drivers/jd9165.h"

/* Guition JC1060P470: JD9165 1024x600 on two lanes of MIPI-DSI */
static const int LCD_DSI_LANES = 2;
static const int LCD_DSI_LANE_MBPS = 750;
static const int LCD_PHY_LDO_CHAN = 3; /* the regulator of the chip that powers the DSI PHY */
static const int LCD_PHY_LDO_MV = 2500;
static const int LCD_PIN_NUM_RST = 0;
static const int LCD_PIN_NUM_BACKLIGHT = 23; /* on at high level */

dgx_screen_t *display_init(void)
{
    dgx_bus_protocols_t *bus = dgx_dsi_init(0, LCD_DSI_LANES, LCD_DSI_LANE_MBPS, LCD_PHY_LDO_CHAN, LCD_PHY_LDO_MV);
    if (!bus) {
        ESP_LOGE(TAG, "dgx_dsi_init failed");
        return NULL;
    }
    /* two frame buffers: what the display task puts as one batch is seen whole, from the next frame on */
    dgx_screen_t *screen = dgx_jd9165_init(bus, (gpio_num_t)LCD_PIN_NUM_RST, 16, DgxScreenRGB, DgxDsiTwoFrames);
    if (!screen) {
        ESP_LOGE(TAG, "dgx_jd9165_init failed");
        bus->dispose(bus);
        return NULL;
    }
    dgx_jd9165_orientation(screen, DgxScreenLeftRight, DgxScreenTopBottom, false);
    /* the light goes on over a black frame buffer, not over what the panel had before */
    gpio_set_direction((gpio_num_t)LCD_PIN_NUM_BACKLIGHT, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)LCD_PIN_NUM_BACKLIGHT, 1);
    display = screen;
    display_bus = bus;
    return screen;
}

#elif CONFIG_DGX_ENABLE_SPI_ILI_9341

#include "driver/spi_common.h"

#include "bus/dgx_spi_esp32.h"
#include "drivers/ili9341.h"

/* CYD (ESP32-2432S028): ILI9341 320x240 on its own SPI bus */
#define LCD_HOST SPI2_HOST

static const int LCD_PIN_NUM_MOSI = 13;
static const int LCD_PIN_NUM_MISO = 12;
static const int LCD_PIN_NUM_CLK = 14;
static const int LCD_PIN_NUM_CS = 15;
static const int LCD_PIN_NUM_DC = 2;
static const int LCD_PIN_NUM_RST = GPIO_NUM_NC;
static const int LCD_PIN_NUM_BACKLIGHT = 21;
static const int LCD_SPI_CLOCK_MHZ = 40;

dgx_screen_t *display_init(void)
{
    dgx_bus_protocols_t *bus = dgx_spi_init(
        LCD_HOST, SPI_DMA_CH_AUTO,
        (gpio_num_t)LCD_PIN_NUM_MOSI, (gpio_num_t)LCD_PIN_NUM_MISO,
        (gpio_num_t)LCD_PIN_NUM_CLK, (gpio_num_t)LCD_PIN_NUM_CS,
        (gpio_num_t)LCD_PIN_NUM_DC, LCD_SPI_CLOCK_MHZ * 1000 * 1000, 0);
    if (!bus) {
        ESP_LOGE(TAG, "dgx_spi_init failed");
        return NULL;
    }
    dgx_screen_t *screen = dgx_ili9341_init(
        bus, (gpio_num_t)LCD_PIN_NUM_RST, (gpio_num_t)LCD_PIN_NUM_BACKLIGHT, 16, DgxScreenRGB);
    if (!screen) {
        ESP_LOGE(TAG, "dgx_ili9341_init failed");
        bus->dispose(bus);
        return NULL;
    }
    /* landscape */
    dgx_ili9341_orientation(screen, DgxScreenRightLeft, DgxScreenTopBottom, true);
    display = screen;
    display_bus = bus;
    return screen;
}

#else
#error "No display: turn on the dgx driver of the display of the board, see sdkconfig.defaults.<target>"
#endif
