#pragma once
// Text input from outside (MQTT payloads, web forms): pure C, host-tested in tests/host/test_textparse.c.
#include <stdbool.h>
#include <stddef.h>

// A whole number text as a float in [min, max]: surrounding spaces are allowed, anything else after the number
// ("21.5xyz"), an empty text, nan and inf are refused. *out is changed only on success.
bool tp_parse_float(const char *s, float min, float max, float *out);

// application/x-www-form-urlencoded: '+' -> space, %XX -> byte (a '%' without two hex digits stays as it is)
void tp_url_decode(char *s);
void tp_trim(char *s);

typedef enum { TP_OK, TP_MISSING, TP_TOO_LONG } tp_res_t;

// Value of key in a form body (key=value&key=value), decoded and trimmed. TP_TOO_LONG when the result does not fit
// in size bytes (out is then empty): a value is never cut.
tp_res_t tp_form_get(const char *body, const char *key, char *out, size_t size);

// Copies src into a fixed text field of dst_size bytes only if it fits whole; false (dst unchanged) otherwise
bool tp_copy_fits(char *dst, size_t dst_size, const char *src);
