#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "mdns.h"

#include "settings.h"
#include "tzones.h"
#include "webserver.h"

typedef esp_err_t (*web_chunk_handler_t)(httpd_req_t *req, const settings_t *settings);

typedef struct {
    const char         *const_html;
    web_chunk_handler_t handler;
} web_template_chunk_t;

static const char    *TAG = "webserver";
static httpd_handle_t server;
static volatile bool  done, clears_wifi, clears_all;

static int hex_to_int(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static void url_decode_inplace(char *text)
{
    char *read = text, *write = text;
    while (*read) {
        if (*read == '+') {
            *write++ = ' ';
            ++read;
        } else if (*read == '%' && hex_to_int(read[1]) >= 0 && hex_to_int(read[2]) >= 0) {
            *write++ = (char)(hex_to_int(read[1]) << 4 | hex_to_int(read[2]));
            read += 3;
        } else {
            *write++ = *read++;
        }
    }
    *write = 0;
}

/* The fields of the form into the settings; a checkbox not checked is not in the form at all. */
static void update_settings_from_form(settings_t *settings, char *body, bool *cancel, bool *clear_wifi, bool *clear_all)
{
    char *save;
    settings->auto_location = false;
    for (char *token = strtok_r(body, "&", &save); token; token = strtok_r(NULL, "&", &save)) {
        char *value = strchr(token, '=');
        if (!value) continue;
        *value++ = 0;
        url_decode_inplace(token);
        url_decode_inplace(value);
        if (strcmp(token, "cancel") == 0) {
            *cancel = true;
        } else if (strcmp(token, "clear_wifi") == 0) {
            *clear_wifi = strcmp(value, "on") == 0;
        } else if (strcmp(token, "clear_all") == 0) {
            *clear_all = strcmp(value, "on") == 0;
        } else if (strcmp(token, "openweather_key") == 0) {
            strlcpy(settings->weather_key, value, sizeof(settings->weather_key));
        } else if (strcmp(token, "is_auto_location") == 0) {
            settings->auto_location = strcmp(value, "on") == 0;
        } else if (strcmp(token, "ip_info_key") == 0) {
            strlcpy(settings->ipinfo_key, value, sizeof(settings->ipinfo_key));
        } else if (strcmp(token, "location") == 0) {
            strlcpy(settings->location, value, sizeof(settings->location));
        } else if (strcmp(token, "place_name") == 0) {
            strlcpy(settings->place_name, value, sizeof(settings->place_name));
        } else if (strcmp(token, "timezone") == 0) {
            strlcpy(settings->timezone, value, sizeof(settings->timezone));
        } else if (strcmp(token, "host_name") == 0) {
            strlcpy(settings->host_name, value, sizeof(settings->host_name));
        }
    }
}

/* A value of the settings as the text of the page: what HTML reads as its own goes as an entity. */
static esp_err_t send_escaped_chunk(httpd_req_t *req, const char *text)
{
    for (const char *flush = text;; ++text) {
        const char *entity = *text == '&' ? "&amp;" : *text == '<' ? "&lt;" : *text == '>' ? "&gt;" : *text == '"' ? "&quot;" : *text == '\'' ? "&#39;" : NULL;
        if (!entity && *text) continue;
        if (text > flush) {
            esp_err_t ret = httpd_resp_send_chunk(req, flush, text - flush);
            if (ret != ESP_OK) return ret;
        }
        if (!*text) return ESP_OK;
        esp_err_t ret = httpd_resp_send_chunk(req, entity, HTTPD_RESP_USE_STRLEN);
        if (ret != ESP_OK) return ret;
        flush = text + 1;
    }
}

static esp_err_t http_data_openweather_key(httpd_req_t *req, const settings_t *settings)
{
    return send_escaped_chunk(req, settings->weather_key);
}

static esp_err_t http_data_is_auto_location_checked(httpd_req_t *req, const settings_t *settings)
{
    return settings->auto_location ? httpd_resp_send_chunk(req, "checked", HTTPD_RESP_USE_STRLEN) : ESP_OK;
}

static esp_err_t http_data_ip_info_key(httpd_req_t *req, const settings_t *settings)
{
    return send_escaped_chunk(req, settings->ipinfo_key);
}

static esp_err_t http_data_location(httpd_req_t *req, const settings_t *settings)
{
    return send_escaped_chunk(req, settings->location);
}

static esp_err_t http_data_place_name(httpd_req_t *req, const settings_t *settings)
{
    return send_escaped_chunk(req, settings->place_name);
}

/* The name found out stands in the empty field of the name given: that is what the screen shows without one. */
static esp_err_t http_data_city(httpd_req_t *req, const settings_t *settings)
{
    return send_escaped_chunk(req, settings->city);
}

static esp_err_t http_data_host_name(httpd_req_t *req, const settings_t *settings)
{
    return send_escaped_chunk(req, settings->host_name);
}

/* The list of time zones is hundreds of short lines: they go to the network a kilobyte at a time, not one by one. */
static esp_err_t http_data_timezone_options(httpd_req_t *req, const settings_t *settings)
{
    static char buffer[1024];
    size_t      length = 0;
    /* no time zone chosen: it is found by the answer about the weather and kept */
    length += snprintf(buffer, sizeof(buffer), "<option value=\"\"%s>Find by the location</option>", settings->timezone[0] ? "" : " selected");
    for (size_t i = 0; i < TimeZonesNumer; ++i) {
        if (sizeof(buffer) - length < 160) {
            esp_err_t ret = httpd_resp_send_chunk(req, buffer, length);
            if (ret != ESP_OK) return ret;
            length = 0;
        }
        length += snprintf(buffer + length, sizeof(buffer) - length, "<option value=\"%s\"%s>%s</option>", TimeZones[i].name,
                           strcmp(settings->timezone, TimeZones[i].name) == 0 ? " selected" : "", TimeZones[i].name);
    }
    return httpd_resp_send_chunk(req, buffer, length);
}

#include "webserver_template.h"

static esp_err_t send_status_page(httpd_req_t *req, const char *title, const char *message)
{
    char page[256];
    int  length = snprintf(page, sizeof(page),
                           "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>%s</title></head><body><p>%s</p></body></html>", title,
                           message);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t ret = httpd_resp_send(req, page, length);
    /* the one at the page knows the outcome: the page has done its work */
    done = true;
    return ret;
}

static esp_err_t redirect_to(httpd_req_t *req, const char *path)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", path);
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    settings_t settings;
    settings_get(&settings);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    for (size_t i = 0; i < sizeof(s_index_template) / sizeof(s_index_template[0]); ++i) {
        esp_err_t ret = httpd_resp_send_chunk(req, s_index_template[i].const_html, HTTPD_RESP_USE_STRLEN);
        if (ret == ESP_OK && s_index_template[i].handler) ret = s_index_template[i].handler(req, &settings);
        if (ret != ESP_OK) {
            httpd_resp_send_chunk(req, NULL, 0);
            return ret;
        }
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t settings_save_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 2048) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form body");
    char *body = malloc(req->content_len + 1);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    for (size_t got = 0; got < req->content_len;) {
        int received = httpd_req_recv(req, body + got, req->content_len - got);
        if (received <= 0) {
            free(body);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read request body");
        }
        got += received;
    }
    body[req->content_len] = 0;

    settings_t current, updated;
    bool       cancel = false, clear_wifi = false, clear_all = false;
    settings_get(&current);
    updated = current;
    update_settings_from_form(&updated, body, &cancel, &clear_wifi, &clear_all);
    free(body);
    /* what this form asks for, not one sent before it */
    clears_all = !cancel && clear_all;
    clears_wifi = !cancel && !clear_all && clear_wifi;
    if (cancel) return redirect_to(req, "/nochanges");
    /* nothing of the form is kept then: all that is kept goes */
    if (clear_all) return redirect_to(req, "/cleared");
    /* a time zone the table has not is no time zone */
    if (updated.timezone[0] && !findTZ(updated.timezone)) updated.timezone[0] = 0;
    /* another place is found anew, and its time zone too unless one is chosen for it right now */
    if (strcmp(current.location, updated.location) != 0) {
        updated.has_place = false;
        updated.city[0] = 0;
        if (strcmp(current.timezone, updated.timezone) == 0) updated.timezone[0] = 0;
    }
    /* compared as they are kept: an emptied field has its default again */
    settings_normalize(&updated);
    if (memcmp(&current, &updated, sizeof(current)) == 0) return redirect_to(req, clear_wifi ? "/thankyou" : "/nochanges");
    esp_err_t ret = settings_update(&updated);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "The settings are not saved: %s", esp_err_to_name(ret));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save settings");
    }
    ESP_LOGI(TAG, "The settings are saved");
    return redirect_to(req, "/thankyou");
}

static esp_err_t settings_thankyou_handler(httpd_req_t *req)
{
    return send_status_page(req, "Configured", "Settings are saved. The clock restarts with them.");
}

static esp_err_t settings_cleared_handler(httpd_req_t *req)
{
    return send_status_page(req, "Cleared", "All settings are cleared. The clock restarts as a new one and waits for the Wi-Fi setup.");
}

static esp_err_t settings_nochanges_handler(httpd_req_t *req)
{
    return send_status_page(req, "No changes", "Settings are not changed.");
}

static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t web_start(void)
{
    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = settings_get_handler},
        {.uri = "/", .method = HTTP_POST, .handler = settings_save_handler},
        {.uri = "/thankyou", .method = HTTP_GET, .handler = settings_thankyou_handler},
        {.uri = "/nochanges", .method = HTTP_GET, .handler = settings_nochanges_handler},
        {.uri = "/cleared", .method = HTTP_GET, .handler = settings_cleared_handler},
        {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler},
    };
    if (server) return ESP_OK;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /* a browser keeps its connections; when all are taken the oldest gives way to a new one */
    config.lru_purge_enable = true;
    esp_err_t      ret = httpd_start(&server, &config);
    if (ret != ESP_OK) return ret;
    done = clears_wifi = clears_all = false;
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); ++i) {
        ret = httpd_register_uri_handler(server, &uris[i]);
        if (ret != ESP_OK) {
            web_stop();
            return ret;
        }
    }
    /* the page is found in the network by those who look for pages */
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    return ESP_OK;
}

void web_stop(void)
{
    if (!server) return;
    mdns_service_remove("_http", "_tcp");
    httpd_stop(server);
    server = NULL;
}

bool web_clears_wifi(void)
{
    return clears_wifi;
}

bool web_clears_all(void)
{
    return clears_all;
}

bool web_is_done(void)
{
    return done;
}
