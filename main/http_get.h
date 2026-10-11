#pragma once

#include <stddef.h>

/*
 * Asks a URL with GET over HTTPS and takes the answer into body, as much of
 * it as fits, with a zero at its end. Returns the HTTP status, or -1 when
 * there is no answer. TLS runs on the stack of the caller: 8 KB of it.
 * URLs here carry keys: they are not for the log.
 */
int http_get(const char *url, char *body, size_t size);

/* Adds a value to a URL: what a URL takes as it is, the rest as %XX. */
void http_append_encoded(char *url, size_t size, const char *value);
