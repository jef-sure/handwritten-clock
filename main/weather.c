#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mjson.h"

#include "http_get.h"
#include "location.h"
#include "net.h"
#include "settings.h"
#include "tzones.h"
#include "weather.h"

static const char *TAG = "weather";

/* The weather is asked for so often; after a failure it is asked again sooner. */
#define WEATHER_PERIOD_MS 300000
#define WEATHER_RETRY_MS  30000
/* The answer about the weather now is some 600 bytes. */
#define WEATHER_BODY_SIZE 1024
/*
 * 1: nothing is asked from the network, every few seconds the next condition
 * of the table is shown with another temperature. To see how a symbol
 * becomes another and how the pen writes the numbers.
 */
#define WEATHER_DEMO 0

typedef struct {
    int      id;
    uint32_t day, night;
} condition_t;

/* https://openweathermap.org/weather-conditions with the symbols of the Weather Icons font */
static const condition_t conditions[] = {
    {200, 0xf010, 0xf03b}, /* thunderstorm with light rain */
    {201, 0xf010, 0xf03b}, /* thunderstorm with rain */
    {202, 0xf010, 0xf03b}, /* thunderstorm with heavy rain */
    {210, 0xf010, 0xf03b}, /* light thunderstorm */
    {211, 0xf010, 0xf03b}, /* thunderstorm */
    {212, 0xf010, 0xf03b}, /* heavy thunderstorm */
    {221, 0xf010, 0xf03b}, /* ragged thunderstorm */
    {230, 0xf010, 0xf03b}, /* thunderstorm with light drizzle */
    {231, 0xf010, 0xf03b}, /* thunderstorm with drizzle */
    {232, 0xf010, 0xf03b}, /* thunderstorm with heavy drizzle */
    {300, 0xf008, 0xf036}, /* light intensity drizzle */
    {301, 0xf008, 0xf036}, /* drizzle */
    {302, 0xf00b, 0xf039}, /* heavy intensity drizzle */
    {310, 0xf00b, 0xf039}, /* light intensity drizzle rain */
    {311, 0xf00b, 0xf039}, /* drizzle rain */
    {312, 0xf008, 0xf036}, /* heavy intensity drizzle rain */
    {313, 0xf009, 0xf037}, /* shower rain and drizzle */
    {314, 0xf009, 0xf037}, /* heavy shower rain and drizzle */
    {321, 0xf009, 0xf037}, /* shower drizzle */
    {500, 0xf009, 0xf037}, /* light rain */
    {501, 0xf009, 0xf037}, /* moderate rain */
    {502, 0xf009, 0xf037}, /* heavy intensity rain */
    {503, 0xf009, 0xf037}, /* very heavy rain */
    {504, 0xf009, 0xf037}, /* extreme rain */
    {511, 0xf0b2, 0xf0b3}, /* freezing rain */
    {520, 0xf009, 0xf037}, /* light intensity shower rain */
    {521, 0xf009, 0xf037}, /* shower rain */
    {522, 0xf009, 0xf037}, /* heavy intensity shower rain */
    {531, 0xf009, 0xf037}, /* ragged shower rain */
    {600, 0xf00a, 0xf038}, /* light snow */
    {601, 0xf00a, 0xf038}, /* snow */
    {602, 0xf00a, 0xf038}, /* heavy snow */
    {611, 0xf0b2, 0xf0b3}, /* sleet */
    {612, 0xf0b2, 0xf0b3}, /* light shower sleet */
    {613, 0xf0b2, 0xf0b3}, /* shower sleet */
    {615, 0xf00a, 0xf038}, /* light rain and snow */
    {616, 0xf00a, 0xf038}, /* rain and snow */
    {620, 0xf00a, 0xf038}, /* light shower snow */
    {621, 0xf00a, 0xf038}, /* shower snow */
    {622, 0xf00a, 0xf038}, /* heavy shower snow */
    {701, 0xf003, 0xf014}, /* mist */
    {711, 0xf062, 0xf062}, /* smoke */
    {721, 0xf0b6, 0xf0b6}, /* haze */
    {731, 0xf063, 0xf063}, /* sand/ dust whirls */
    {741, 0xf003, 0xf014}, /* fog */
    {751, 0xf082, 0xf082}, /* sand */
    {761, 0xf063, 0xf063}, /* dust */
    {762, 0xf0c8, 0xf0c8}, /* volcanic ash */
    {771, 0xf021, 0xf021}, /* squalls */
    {781, 0xf056, 0xf056}, /* tornado */
    {800, 0xf00d, 0xf02e}, /* clear sky */
    {801, 0xf00c, 0xf083}, /* few clouds */
    {802, 0xf00c, 0xf083}, /* scattered clouds */
    {803, 0xf002, 0xf031}, /* broken clouds */
    {804, 0xf002, 0xf031}, /* overcast clouds */
};

static weather_listener_t listener;

uint32_t weather_symbol(int id, bool daytime)
{
    for (size_t i = 0; i < sizeof(conditions) / sizeof(conditions[0]); ++i) {
        if (conditions[i].id == id) return daytime ? conditions[i].day : conditions[i].night;
    }
    return 0;
}

static uint8_t clamp(float v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

void weather_temp_to_rgb(int temp, uint8_t *r, uint8_t *g, uint8_t *b)
{
    float fr, fg, fb;
    if (temp >= 23) {
        float t = (temp - 23) / 27.0f;
        fg = (1 - t) * 200;
        fr = 200 + t * 50;
        fb = 0;
    } else {
        float t = (23 - temp) / 33.0f;
        fb = t * 200;
        fg = sqrtf(1 - t >= 0 ? 1 - t : 0) * 200;
        fr = 220 - sqrtf(t / 2) * 200;
    }
    /* the same brightness whatever the color is */
    uint8_t most = clamp(fr) > clamp(fg) ? clamp(fr) : clamp(fg);
    if (clamp(fb) > most) most = clamp(fb);
    float scale = 224.0f / (most ? most : 1);
    *r = clamp(fr * scale);
    *g = clamp(fg * scale);
    *b = clamp(fb * scale);
}

#if WEATHER_DEMO
/* Made-up weather: all the conditions in turn, by day and by night, the temperature from frost to heat and back. */
static void weather_task(void *arg)
{
    (void)arg;
    const int number = sizeof(conditions) / sizeof(conditions[0]);
    for (int i = 0;; ++i) {
        int           step = i % 24;
        weather_now_t now = {
            .id = conditions[i * 7 % number].id,
            .temperature = -25 + 5 * (step < 12 ? step : 24 - step),
            .city = "Made up",
        };
        now.symbol = weather_symbol(now.id, i / number % 2 == 0);
        ESP_LOGI(TAG, "Made up: %d C, condition %d, symbol U+%04lx", now.temperature, now.id, (unsigned long)now.symbol);
        listener(&now);
        vTaskDelay(pdMS_TO_TICKS(4000));
    }
}
#else
/*
 * A time zone not chosen is found out from the answer: it has the country of
 * the place and how far the place is now from UTC. The zone of that country
 * which is as far from UTC, nearest to the place, is kept in the settings
 * with its rules of summer time. Which zone is so far from UTC depends on
 * the day, so the network must have given the time first. Until the table
 * gives one the clock goes by the shift alone. A place found by the address gets its zone from there.
 */
static void take_time_zone(const settings_t *settings, const location_t *location, const char *data, int length)
{
    double shift;
    char   country[8] = "";
    if (settings->timezone[0] || settings->auto_location || !mjson_get_number(data, length, "$.timezone", &shift)) return;
    mjson_get_string(data, length, "$.sys.country", country, sizeof(country));
    const TZName_t *zone = net_has_time() ? findTZbyPlace(country, location->lat, location->lon, (int)shift) : NULL;
    if (zone) {
        settings_t kept;
        settings_get(&kept);
        strlcpy(kept.timezone, zone->name, sizeof(kept.timezone));
        setupTZ(zone->name);
        esp_err_t ret = settings_update(&kept);
        ESP_LOGI(TAG, "Time zone of the place: %s, %+d s from UTC in %s%s", zone->name, (int)shift, country,
                 ret == ESP_OK ? "; kept" : "; not kept");
        return;
    }
    /* POSIX counts the other way: hours to add to the local time to get UTC */
    char tz[24];
    int  minutes = abs((int)shift) / 60;
    snprintf(tz, sizeof(tz), "OWM%c%d:%02d", shift > 0 ? '-' : '+', minutes / 60, minutes % 60);
    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGW(TAG, "No time zone %+d s from UTC in \"%s\" in the table, or the clock is not set: TZ=%s for now", (int)shift, country, tz);
}

/* A place given by coordinates has no name until the weather names it. */
static void take_city(const settings_t *settings, const char *data, int length)
{
    char city[SETTINGS_CITY_LEN] = "";
    if (settings->auto_location || settings->city[0] || !settings->has_place) return;
    if (mjson_get_string(data, length, "$.name", city, sizeof(city)) <= 0) return;
    settings_t kept;
    settings_get(&kept);
    strlcpy(kept.city, city, sizeof(kept.city));
    if (settings_update(&kept) == ESP_OK) ESP_LOGI(TAG, "The place is named %s", city);
}

static bool weather_get(const settings_t *settings, const location_t *location, weather_now_t *now)
{
    static char data[WEATHER_BODY_SIZE];
    char        url[256];
    snprintf(url, sizeof(url), "https://api.openweathermap.org/data/2.5/weather?units=metric&lat=%.4f&lon=%.4f&appid=", location->lat,
             location->lon);
    http_append_encoded(url, sizeof(url), settings->weather_key);
    int status = http_get(url, data, sizeof(data));
    if (status != 200) {
        if (status > 0) ESP_LOGW(TAG, "OpenWeatherMap answers %d: %s", status, data);
        return false;
    }
    int    length = (int)strlen(data);
    double id, temp, dt, sunrise, sunset;
    if (!mjson_get_number(data, length, "$.weather[0].id", &id) || !mjson_get_number(data, length, "$.main.temp", &temp)) {
        ESP_LOGW(TAG, "No weather in the answer: %s", data);
        return false;
    }
    take_time_zone(settings, location, data, length);
    take_city(settings, data, length);
    /* day unless the answer says the sun is down */
    bool daytime = true;
    if (mjson_get_number(data, length, "$.dt", &dt) && mjson_get_number(data, length, "$.sys.sunrise", &sunrise) &&
        mjson_get_number(data, length, "$.sys.sunset", &sunset)) {
        daytime = dt >= sunrise && dt <= sunset;
        now->sunrise = (int64_t)sunrise;
        now->sunset = (int64_t)sunset;
    }
    now->id = (int)id;
    now->temperature = (int)lround(temp);
    now->symbol = weather_symbol(now->id, daytime);
    /* the name given on the page; without it the name found out, and a place without a name has the one the weather gives it */
    strlcpy(now->city, settings->place_name[0] ? settings->place_name : location->city, sizeof(now->city));
    if (!now->city[0]) mjson_get_string(data, length, "$.name", now->city, sizeof(now->city));
    ESP_LOGI(TAG, "%.4f,%.4f: %d C, condition %d by %s, symbol U+%04lx", location->lat, location->lon, now->temperature, now->id,
             daytime ? "day" : "night", (unsigned long)now->symbol);
    return true;
}

/* What the weather waits for is told once, not every time it is looked at. */
static void tell(const char **told, const char *what)
{
    if (*told != what && what) ESP_LOGW(TAG, "No weather: %s", what);
    *told = what;
}

static void weather_task(void *arg)
{
    (void)arg;
    const char *told = NULL;
    /* what fails is asked again after a pause twice as long each time, up to the usual one: a wrong key stays wrong */
    uint32_t retry_ms = WEATHER_RETRY_MS;
    for (;;) {
        if (!net_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        settings_t settings;
        location_t location;
        settings_get(&settings);
        const char *lacks = !settings.weather_key[0]             ? "the settings have no key of OpenWeatherMap"
                            : !location_get(&settings, &location) ? "the place is not known"
                                                                  : NULL;
        tell(&told, lacks);
        if (lacks) {
            vTaskDelay(pdMS_TO_TICKS(retry_ms));
            retry_ms = retry_ms * 2 < WEATHER_PERIOD_MS ? retry_ms * 2 : WEATHER_PERIOD_MS;
            continue;
        }
        weather_now_t now = {0};
        bool          got = weather_get(&settings, &location, &now);
        if (got) listener(&now);
        vTaskDelay(pdMS_TO_TICKS(got ? WEATHER_PERIOD_MS : retry_ms));
        retry_ms = got ? WEATHER_RETRY_MS : retry_ms * 2 < WEATHER_PERIOD_MS ? retry_ms * 2 : WEATHER_PERIOD_MS;
    }
}
#endif

bool weather_start(weather_listener_t on_weather)
{
    listener = on_weather;
    /* TLS runs on the stack of the task that asks */
    return xTaskCreate(weather_task, "weather", 8192, NULL, tskIDLE_PRIORITY + 1, NULL) == pdPASS;
}
