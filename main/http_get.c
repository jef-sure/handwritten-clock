#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "http_get.h"

static const char *TAG = "http_get";

typedef struct {
    char  *data;
    size_t size, length;
} body_t;

static esp_err_t on_http_event(esp_http_client_event_t *event)
{
    body_t *body = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA) {
        size_t room = body->size - 1 - body->length;
        size_t take = (size_t)event->data_len < room ? (size_t)event->data_len : room;
        memcpy(body->data + body->length, event->data, take);
        body->length += take;
    }
    return ESP_OK;
}

int http_get(const char *url, char *data, size_t size)
{
    body_t                   body = {.data = data, .size = size};
    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 7000,
        .event_handler = on_http_event,
        .user_data = &body,
    };
    data[0] = 0;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return -1;
    esp_err_t ret = esp_http_client_perform(client);
    int       status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    data[body.length] = 0;
    if (ret != ESP_OK) {
        /* a handshake that has no memory fails in the middle of checking the certificate: the heap tells it apart from a bad network */
        ESP_LOGW(TAG, "No answer: %s; %u bytes of the heap are free, the largest piece is %u, the least there was is %u", esp_err_to_name(ret),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
        return -1;
    }
    return status;
}

void http_append_encoded(char *url, size_t size, const char *value)
{
    size_t length = strlen(url);
    for (; *value && length + 4 < size; ++value) {
        unsigned char c = (unsigned char)*value;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.' || c == '~') {
            url[length++] = (char)c;
        } else {
            length += snprintf(url + length, size - length, "%%%02X", c);
        }
    }
    url[length] = 0;
}
