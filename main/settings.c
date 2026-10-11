#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "sdkconfig.h"

#include "settings.h"

#define SETTINGS_SCHEMA_VERSION 5
/*
 * The name in the network until another is given at the page of settings:
 * this and the last three bytes of the address of the station, the same ones
 * the setup of Wi-Fi shows, so two clocks in a house have two names.
 */
#define DEFAULT_HOST_NAME "weather-clock"
/* version 4 kept the same fields up to the name found out, version 3 up to the time zone, and nothing after them */
#define SETTINGS_SCHEMA_VERSION_4 4
#define SETTINGS_SIZE_4           offsetof(settings_t, place_name)
#define SETTINGS_SCHEMA_VERSION_3 3
#define SETTINGS_SIZE_3           offsetof(settings_t, has_place)

typedef struct {
    uint32_t   schema_version;
    settings_t settings;
} settings_storage_t;

static const char       *NAMESPACE = "appcfg";
static const char       *KEY = "settings";
static const char       *KEY_BOOT = "boot";
static SemaphoreHandle_t mutex;
static settings_t        current;

void settings_normalize(settings_t *settings)
{
    settings->host_name[SETTINGS_HOST_NAME_LEN - 1] = 0;
    settings->weather_key[SETTINGS_WEATHER_KEY_LEN - 1] = 0;
    settings->ipinfo_key[SETTINGS_IPINFO_KEY_LEN - 1] = 0;
    settings->location[SETTINGS_LOCATION_LEN - 1] = 0;
    settings->timezone[SETTINGS_TIMEZONE_LEN - 1] = 0;
    settings->city[SETTINGS_CITY_LEN - 1] = 0;
    settings->place_name[SETTINGS_CITY_LEN - 1] = 0;
    /* a name in the network is letters, digits and hyphens inside it; anything else in it becomes a hyphen */
    char *end = settings->host_name;
    for (const char *c = settings->host_name; *c; ++c) {
        bool fits = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z');
        if (fits) {
            *end++ = *c;
        } else if (end > settings->host_name && end[-1] != '-') {
            *end++ = '-';
        }
    }
    while (end > settings->host_name && end[-1] == '-') --end;
    *end = 0;
    if (!settings->host_name[0]) {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(settings->host_name, SETTINGS_HOST_NAME_LEN, DEFAULT_HOST_NAME "-%02x%02x%02x", mac[3], mac[4], mac[5]);
    }
}

static esp_err_t save(const settings_t *settings)
{
    settings_storage_t storage = {.schema_version = SETTINGS_SCHEMA_VERSION, .settings = *settings};
    nvs_handle_t       handle;
    esp_err_t          ret = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_blob(handle, KEY, &storage, sizeof(storage));
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    return ret;
}

esp_err_t settings_init(void)
{
    if (mutex) return ESP_OK;
    mutex = xSemaphoreCreateMutex();
    if (!mutex) return ESP_ERR_NO_MEM;

    /* what is kept by another version of the settings is not read: the defaults take its place when something is saved */
    settings_storage_t storage = {0};
    size_t             size = sizeof(storage);
    nvs_handle_t       handle;
    memset(&current, 0, sizeof(current));
    if (nvs_open(NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_blob(handle, KEY, &storage, &size) == ESP_OK) {
            if (size == sizeof(storage) && storage.schema_version == SETTINGS_SCHEMA_VERSION) {
                current = storage.settings;
            } else if (storage.schema_version == SETTINGS_SCHEMA_VERSION_4 && size >= sizeof(uint32_t) + SETTINGS_SIZE_4) {
                memcpy(&current, &storage.settings, SETTINGS_SIZE_4);
            } else if (storage.schema_version == SETTINGS_SCHEMA_VERSION_3 && size >= sizeof(uint32_t) + SETTINGS_SIZE_3) {
                memcpy(&current, &storage.settings, SETTINGS_SIZE_3);
            }
        }
        nvs_close(handle);
    }
    settings_normalize(&current);
    return ESP_OK;
}

void settings_get(settings_t *settings)
{
    xSemaphoreTake(mutex, portMAX_DELAY);
    *settings = current;
    xSemaphoreGive(mutex);
}

esp_err_t settings_update(const settings_t *settings)
{
    settings_t normalized = *settings;
    settings_normalize(&normalized);
    xSemaphoreTake(mutex, portMAX_DELAY);
    esp_err_t ret = save(&normalized);
    if (ret == ESP_OK) current = normalized;
    xSemaphoreGive(mutex);
    return ret;
}

esp_err_t settings_erase(void)
{
    nvs_handle_t handle;
    esp_err_t    ret = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;
    xSemaphoreTake(mutex, portMAX_DELAY);
    ret = nvs_erase_all(handle);
    if (ret == ESP_OK) ret = nvs_commit(handle);
    if (ret == ESP_OK) {
        memset(&current, 0, sizeof(current));
        settings_normalize(&current);
    }
    xSemaphoreGive(mutex);
    nvs_close(handle);
    return ret;
}

esp_err_t settings_ask_boot(boot_mode_t mode)
{
    nvs_handle_t handle;
    esp_err_t    ret = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) return ret;
    ret = nvs_set_u8(handle, KEY_BOOT, (uint8_t)mode);
    if (ret == ESP_OK) ret = nvs_commit(handle);
    nvs_close(handle);
    return ret;
}

boot_mode_t settings_take_boot(void)
{
    nvs_handle_t handle;
    uint8_t      asked = BOOT_CLOCK;
    if (nvs_open(NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return BOOT_CLOCK;
    if (nvs_get_u8(handle, KEY_BOOT, &asked) == ESP_OK) {
        nvs_erase_key(handle, KEY_BOOT);
        nvs_commit(handle);
    }
    nvs_close(handle);
    return asked == BOOT_WIFI_SETUP || asked == BOOT_WEB_SETTINGS ? (boot_mode_t)asked : BOOT_CLOCK;
}
