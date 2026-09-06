#pragma once
#include <stddef.h>
/* HTML-escapes a value for safe placement inside an HTML attribute/body. */
void nf_html_escape(const char *in, char *out, size_t cap);
/* Percent/plus-decodes n bytes of an x-www-form-urlencoded value. */
void nf_url_decode(const char *in, size_t n, char *out, size_t cap);
/* Extracts and decodes one field's value from an x-www-form-urlencoded body. */
void nf_form_field(const char *body, const char *key, char *out, size_t cap);
/* Escapes a value for safe placement inside a JSON string literal. Without
 * this, a value containing a literal '"' could inject additional JSON keys
 * (e.g. wifi_ssid) into a hand-built config line - nf_url_valid does not
 * reject '"', so this must not be skipped for URL fields. */
void nf_json_escape(const char *in, char *out, size_t cap);
