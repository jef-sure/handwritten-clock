#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mjson.h"

#include "http_get.h"
#include "location.h"
#include "tzones.h"

static const char *TAG = "location";

/* The place found by the address is asked for again after so long: the device may have moved. */
#define LOCATION_PERIOD_US (600 * 1000000LL)

static location_t found;
static int64_t    found_us;
static bool       has_found;
/* an answer of the geocoder names the place in dozens of languages before it comes to the coordinates */
static char body[8192];

/* "52.5200,13.4050" and nothing else */
static bool parse_coordinates(const char *text, float *lat, float *lon)
{
    char *end;
    *lat = strtof(text, &end);
    if (end == text || *end != ',') return false;
    text = end + 1;
    *lon = strtof(text, &end);
    while (*end == ' ') ++end;
    return end != text && !*end && *lat >= -90 && *lat <= 90 && *lon >= -180 && *lon <= 180;
}

/*
 * {"ip": "...", "city": "Berlin", "region": "Berlin", "country": "DE",
 *  "loc": "52.5200,13.4050", "postal": "10115", "timezone": "Europe/Berlin", ...}
 */
static bool ask_ipinfo(const char *token, location_t *location)
{
    char url[96] = "https://ipinfo.io?token=";
    http_append_encoded(url, sizeof(url), token);
    int status = http_get(url, body, sizeof(body));
    if (status != 200) {
        if (status > 0) ESP_LOGW(TAG, "ipinfo.io answers %d: %s", status, body);
        return false;
    }
    int  length = (int)strlen(body);
    char loc[32] = "", timezone[SETTINGS_TIMEZONE_LEN] = "";
    memset(location, 0, sizeof(*location));
    mjson_get_string(body, length, "$.city", location->city, sizeof(location->city));
    mjson_get_string(body, length, "$.loc", loc, sizeof(loc));
    mjson_get_string(body, length, "$.timezone", timezone, sizeof(timezone));
    if (!parse_coordinates(loc, &location->lat, &location->lon)) {
        ESP_LOGW(TAG, "No place in the answer of ipinfo.io: %s", body);
        return false;
    }
    if (timezone[0] && !setupTZ(timezone)) ESP_LOGW(TAG, "No time zone \"%s\"", timezone);
    ESP_LOGI(TAG, "By the address: %s, %.4f,%.4f, time zone %s", location->city, location->lat, location->lon, timezone);
    return true;
}

/* One request to the geocoder of OpenWeatherMap; prefix is "$" or "$[0]": where the place is in its answer. */
static bool ask_geocoder(const char *request, const char *value, const char *key, const char *prefix, location_t *location)
{
    char url[384], path[24]; /* a place of 63 bytes, every one of them as %XX, and the key after it */
    snprintf(url, sizeof(url), "https://api.openweathermap.org/geo/1.0/%s", request);
    http_append_encoded(url, sizeof(url), value);
    strlcat(url, "&appid=", sizeof(url));
    http_append_encoded(url, sizeof(url), key);
    int status = http_get(url, body, sizeof(body));
    if (status != 200) {
        if (status > 0) ESP_LOGW(TAG, "The geocoder answers %d to \"%s\": %.200s", status, value, body);
        return false;
    }
    int    length = (int)strlen(body);
    double lat, lon;
    snprintf(path, sizeof(path), "%s.lat", prefix);
    if (!mjson_get_number(body, length, path, &lat)) return false;
    snprintf(path, sizeof(path), "%s.lon", prefix);
    if (!mjson_get_number(body, length, path, &lon)) return false;
    memset(location, 0, sizeof(*location));
    location->lat = (float)lat;
    location->lon = (float)lon;
    snprintf(path, sizeof(path), "%s.name", prefix);
    mjson_get_string(body, length, path, location->city, sizeof(location->city));
    return true;
}

/* The place of the settings into coordinates. */
static bool find_manual(const settings_t *settings, location_t *location)
{
    const char *text = settings->location;
    memset(location, 0, sizeof(*location));
    if (parse_coordinates(text, &location->lat, &location->lon)) return true;
    /* a name, with a country or without: "Berlin,DE" */
    if (ask_geocoder("direct?limit=1&q=", text, settings->weather_key, "$[0]", location)) return true;
    /* a postal code and a country, whatever stands between them: "10115 Berlin, DE" is asked as "10115,DE" */
    const char *comma = strrchr(text, ',');
    size_t      digits = strspn(text, "0123456789");
    if (digits && comma) {
        char zip[32];
        while (*++comma == ' ') {}
        snprintf(zip, sizeof(zip), "%.*s,%s", (int)(digits < 12 ? digits : 12), text, comma);
        if (ask_geocoder("zip?zip=", zip, settings->weather_key, "$", location)) return true;
    }
    ESP_LOGW(TAG, "The geocoder of OpenWeatherMap knows no \"%s\"", text);
    return false;
}

/* The place found for the settings is kept with them: it is not asked for again until the settings name another. */
static void keep_place(const location_t *location)
{
    settings_t settings;
    settings_get(&settings);
    settings.has_place = true;
    settings.lat = location->lat;
    settings.lon = location->lon;
    strlcpy(settings.city, location->city, sizeof(settings.city));
    esp_err_t ret = settings_update(&settings);
    if (ret != ESP_OK) ESP_LOGW(TAG, "The place is not kept: %s", esp_err_to_name(ret));
}

bool location_get(const settings_t *settings, location_t *location)
{
    if (!settings->auto_location) {
        if (!settings->location[0]) return false;
        if (!settings->has_place) {
            location_t now;
            if (!find_manual(settings, &now)) return false;
            ESP_LOGI(TAG, "\"%s\" is %s, %.4f,%.4f", settings->location, now.city, now.lat, now.lon);
            keep_place(&now);
            *location = now;
            return true;
        }
        memset(location, 0, sizeof(*location));
        location->lat = settings->lat;
        location->lon = settings->lon;
        strlcpy(location->city, settings->city, sizeof(location->city));
        return true;
    }
    if (!settings->ipinfo_key[0]) return false;
    if (!has_found || esp_timer_get_time() - found_us > LOCATION_PERIOD_US) {
        location_t now;
        if (ask_ipinfo(settings->ipinfo_key, &now)) {
            found = now;
            found_us = esp_timer_get_time();
            has_found = true;
        }
    }
    /* the place found before serves while ipinfo.io does not answer */
    if (has_found) *location = found;
    return has_found;
}
