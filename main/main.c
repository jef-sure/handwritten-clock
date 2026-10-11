#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "dgx_bits.h"
#include "dgx_colors.h"
#include "dgx_draw.h"

#include "city_face.h"
#include "status_face.h"
#include "clock_face.h"
#include "display.h"
#include "net.h"
#include "settings.h"
#include "setup_screen.h"
#include "tzones.h"
#include "weather.h"
#include "weather_face.h"
#include "webserver.h"

static const char *TAG = "main";

/*
 * The BOOT button of the board, a touch button in the case to come: pressed,
 * it opens the page of settings in the network and, there, closes it. How
 * long it is held means nothing: such a button is held by a hand that takes
 * the clock.
 */
/* BOOT is a pin the chip names: GPIO 0 of an ESP32, GPIO 35 of an ESP32-P4, whose GPIO 0 resets the display on Guition JC1060P470 */
#if CONFIG_IDF_TARGET_ESP32P4
#define BUTTON_GPIO GPIO_NUM_35
#else
#define BUTTON_GPIO GPIO_NUM_0
#endif
/* The page of settings waits for somebody so long, and so long for the network before that. */
#define WEB_SETTINGS_TIMEOUT_MS 300000
#define WEB_NETWORK_TIMEOUT_MS  60000
/* The setup of Wi-Fi waits for the phone so long when a network is kept; a device that knows none waits for ever. */
#define WIFI_SETUP_TIMEOUT_MS 180000

/* Until the network gives the time the clock goes from the time the firmware was built. */
static void set_time_of_build(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char                  text[sizeof(app->date) + sizeof(app->time) + 1];
    struct tm             tm = {0};
    snprintf(text, sizeof(text), "%s %s", app->date, app->time);
    if (!strptime(text, "%b %d %Y %H:%M:%S", &tm)) return;
    tm.tm_isdst = -1;
    struct timeval now = {.tv_sec = mktime(&tm)};
    settimeofday(&now, NULL);
}

/* What is on the screen beside the time goes into the night with it. */
static void on_night(float night)
{
    city_face_set_night(night);
    weather_face_set_night(night);
    status_face_set_night(night);
}

/* The first weather has come: the sign that waits for it may go. */
static volatile bool weather_come;

static bool has_weather(void)
{
    return weather_come;
}

static void on_weather(const weather_now_t *now)
{
    weather_come = true;
    weather_face_show(now->symbol, now->temperature);
    city_face_show(now->city);
    clock_face_set_sun((time_t)now->sunrise, (time_t)now->sunset);
}

static void button_init(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&config);
}

static bool button_is_pressed(void)
{
    return gpio_get_level(BUTTON_GPIO) == 0;
}

/* The page of settings is a boot of its own: the button asks for it and restarts. */
static void button_task(void *arg)
{
    (void)arg;
    /* two looks at it in a row: one is a bounce */
    for (int pressed = 0; pressed < 2; pressed = button_is_pressed() ? pressed + 1 : 0) vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "The button is pressed: restart into the page of settings");
    settings_ask_boot(BOOT_WEB_SETTINGS);
    esp_restart();
}

/*
 * The page of settings: the screen shows its address as a QR code and as
 * text until the settings are saved or left, the button is pressed or the
 * time is out. Returns false when the network kept gives no connection: the
 * page cannot be reached then, and neither can its way to forget the network.
 */
static bool web_settings(dgx_screen_t *screen)
{
    char address[16], url[32], state[40];
    int  rssi;
    setup_screen_draw(screen, "Settings", NULL, "connecting…");
    if (net_start() != ESP_OK) return false;
    for (int waited_ms = 0; !net_get_address(address, sizeof(address)); waited_ms += 100) {
        if (waited_ms >= WEB_NETWORK_TIMEOUT_MS) return false;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (web_start() != ESP_OK) return true;
    snprintf(url, sizeof(url), "http://%s/", address);
    ESP_LOGI(TAG, "The page of settings is at %s", url);
    /* the address and how well the access point is heard: a page that loads badly is explained by a weak signal */
    if (net_get_rssi(&rssi)) {
        /* the minus sign of the font, U+2212: its hyphen stands too low before a digit */
        snprintf(state, sizeof(state), "%s  %s%d dB", address, rssi < 0 ? "\xE2\x88\x92" : "", abs(rssi));
        ESP_LOGI(TAG, "Signal %d dBm", rssi);
    } else {
        strlcpy(state, address, sizeof(state));
    }
    setup_screen_draw(screen, "Settings", url, state);
    /* the press that has brought here may still last; the next one closes the page */
    bool released = false;
    for (int waited_ms = 0; waited_ms < WEB_SETTINGS_TIMEOUT_MS && !web_is_done(); waited_ms += 50) {
        bool pressed = button_is_pressed();
        if (released && pressed) break;
        if (!pressed) released = true;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    /* the last page gets to the browser */
    vTaskDelay(pdMS_TO_TICKS(1500));
    web_stop();
    if (web_is_done() && web_clears_all()) {
        esp_err_t ret = settings_erase();
        net_forget();
        ESP_LOGI(TAG, "Everything is forgotten: %s", esp_err_to_name(ret));
    } else if (web_is_done() && web_clears_wifi()) {
        ESP_LOGI(TAG, "The network is forgotten");
        net_forget();
    }
    return true;
}

void app_main(void)
{
    dgx_screen_t *screen = display_init();
    if (!screen) return;
    ESP_LOGI(TAG, "Screen %dx%d", screen->width, screen->height);
    uint16_t paper = (uint16_t)dgx_rgb_to_16(0, 0, 0);
    uint16_t ink = (uint16_t)dgx_rgb_to_16(150, 215, 240);
    dgx_fill_rectangle(screen, 0, 0, screen->width, screen->height, paper);

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(settings_init());
    settings_t settings;
    settings_get(&settings);
    /* with no time zone chosen the clock shows UTC until the weather or the place tells the zone */
    if (!setupTZ(settings.timezone)) {
        if (settings.timezone[0]) ESP_LOGW(TAG, "No time zone \"%s\", the clock shows UTC", settings.timezone);
        setupTZ("Etc/UTC");
    }
    set_time_of_build();

    /*
     * A radio that does not answer is no reason to stop: it may be a chip of
     * its own beside this one. The clock goes then from the time it has, with
     * no setup and no weather, and the sign of Wi-Fi in the top row waits.
     */
    ret = net_init();
    bool radio = ret == ESP_OK;
    if (!radio) ESP_LOGE(TAG, "No Wi-Fi: %s", esp_err_to_name(ret));
    button_init();
    /* a setup has the screen and the memory to itself; the clock comes with the next boot */
    boot_mode_t boot = settings_take_boot();
    if (radio && boot == BOOT_WEB_SETTINGS && net_is_provisioned()) {
        /* no way to the page: the next boot offers another network, and goes on with this one if none is given */
        if (!web_settings(screen)) settings_ask_boot(BOOT_WIFI_SETUP);
        esp_restart();
    }
    if (radio && (boot == BOOT_WIFI_SETUP || !net_is_provisioned())) {
        net_provision(screen, net_is_provisioned() ? WIFI_SETUP_TIMEOUT_MS : 0);
        esp_restart();
    }
    if (radio) {
        ret = net_start();
        if (ret != ESP_OK) ESP_LOGE(TAG, "Wi-Fi does not start: %s", esp_err_to_name(ret));
    }
    /* from here on the display has a task of its own, and nobody else sends anything to it */
    if (!display_start()) {
        ESP_LOGE(TAG, "The display task does not start");
        return;
    }

    /*
     * The places are those of the clock of 2021 this one comes from, on its
     * screen of 320x240: the time 120 high, but hours and minutes alone in
     * the line, seconds small above the minutes: large ones that change all
     * the time draw the eye. Under it the symbol of the weather from x 20,
     * and the temperature not right after it as there but at the right
     * edge; the name of the place in the rest. The row of the
     * weather is 74 high, not 72, and its symbol 80 wide, not 60: dots of two
     * pixels need that. Another screen has the same shares of its size.
     */
    int across = screen->width, down = screen->height;
    clock_face_config_t time_place = {
        .screen = screen,
        .x = 0,
        .y = 0,
        .width = across,
        .height = down * 120 / 240,
        .seconds = CLOCK_SECONDS_SMALL,
        .paper = paper,
        /* silver by day and copper by night, as the clock with the dots has it */
        .ink = (uint16_t)DGX_SILVER(dgx_rgb_to_16),
        .ink_night = (uint16_t)DGX_COPPER(dgx_rgb_to_16),
        /* the name of the place and the weather go into the night with the time */
        .on_night = on_night,
    };
    weather_face_config_t weather_place = {
        .screen = screen,
        .x = across * 20 / 320,
        .y = time_place.height,
        .width = across - across * 20 / 320,
        .height = down * 74 / 240,
        .symbol_width = across * 80 / 320,
    };
    /* the name kept is written at once, before the network says anything */
    city_face_config_t city_place = {
        .screen = screen,
        .x = 0,
        .y = weather_place.y + weather_place.height,
        .width = screen->width,
        .height = screen->height - weather_place.y - weather_place.height,
        .paper = paper,
        .ink = ink,
        /* copper by night, as the time */
        .ink_night = (uint16_t)DGX_COPPER(dgx_rgb_to_16),
    };
    if (city_face_start(&city_place)) city_face_show(settings.place_name[0] ? settings.place_name : settings.city);
    /* after the name of the place: the clock tells it how much of the night it is */
    clock_face_start(&time_place);
    /* what the clock still waits for is shown in the row above the time, beside the seconds */
    status_face_config_t status_place = {
        .screen = screen,
        .paper = paper,
        .ink = (uint16_t)DGX_SILVER(dgx_rgb_to_16),
        .ink_night = (uint16_t)DGX_COPPER(dgx_rgb_to_16),
        .connected = net_is_connected,
        .has_time = net_has_time,
        /* without a key of the weather service or a place no weather comes: nothing waits for it */
        .has_weather = settings.weather_key[0] && (settings.location[0] || settings.auto_location) ? has_weather : NULL,
    };
    if (clock_face_top_row(&status_place.x, &status_place.y, &status_place.width, &status_place.height)) status_face_start(&status_place);
    if (weather_face_start(&weather_place)) weather_start(on_weather);
    xTaskCreate(button_task, "button", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
}
