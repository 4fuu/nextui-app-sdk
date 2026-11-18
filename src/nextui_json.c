#define _POSIX_C_SOURCE 200809L

#include "nextui_json.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NX_JSON_MAX_DEPTH 32
#define NX_JSON_ESCAPE_MAX 8192

static const char* skip_space(const char* p, const char* end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

/* Decode one string starting at p (which must point at '"'). Returns the
 * pointer just past the closing quote, or NULL. The decoded UTF-8 is written to
 * `out` (NUL terminated) when it is non-NULL; `out_length` always receives the
 * full decoded byte length, even when the output was truncated. */
static const char* decode_string(const char* p, const char* end, char* out, size_t out_size,
                                 size_t* out_length)
{
    size_t total = 0;
    size_t written = 0;

    if (p >= end || *p != '"') return NULL;
    p++;

    while (p < end)
    {
        unsigned char ch = (unsigned char)*p;
        unsigned char bytes[4];
        size_t count = 1;
        size_t i;

        if (ch == '"')
        {
            if (out != NULL && out_size > 0)
            {
                size_t index = written < out_size ? written : out_size - 1;
                out[index] = '\0';
            }
            else if (out != NULL)
            {
                return NULL;
            }
            if (out_length != NULL) *out_length = total;
            return p + 1;
        }

        if (ch < 0x20) return NULL; /* raw control character */

        if (ch == '\\')
        {
            uint32_t codepoint;

            p++;
            if (p >= end) return NULL;
            switch (*p)
            {
            case '"': codepoint = '"'; p++; break;
            case '\\': codepoint = '\\'; p++; break;
            case '/': codepoint = '/'; p++; break;
            case 'b': codepoint = '\b'; p++; break;
            case 'f': codepoint = '\f'; p++; break;
            case 'n': codepoint = '\n'; p++; break;
            case 'r': codepoint = '\r'; p++; break;
            case 't': codepoint = '\t'; p++; break;
            case 'u':
            {
                unsigned value = 0;
                int digits;
                p++;
                for (digits = 0; digits < 4; digits++)
                {
                    unsigned char digit;
                    if (p >= end) return NULL;
                    digit = (unsigned char)*p++;
                    if (digit >= '0' && digit <= '9') value = value * 16u + (digit - '0');
                    else if (digit >= 'a' && digit <= 'f') value = value * 16u + (digit - 'a' + 10u);
                    else if (digit >= 'A' && digit <= 'F') value = value * 16u + (digit - 'A' + 10u);
                    else return NULL;
                }
                codepoint = value;
                if (codepoint >= 0xD800 && codepoint <= 0xDBFF)
                {
                    unsigned low = 0;
                    if (p + 1 >= end || p[0] != '\\' || p[1] != 'u') return NULL;
                    p += 2;
                    for (digits = 0; digits < 4; digits++)
                    {
                        unsigned char digit;
                        if (p >= end) return NULL;
                        digit = (unsigned char)*p++;
                        if (digit >= '0' && digit <= '9') low = low * 16u + (digit - '0');
                        else if (digit >= 'a' && digit <= 'f') low = low * 16u + (digit - 'a' + 10u);
                        else if (digit >= 'A' && digit <= 'F') low = low * 16u + (digit - 'A' + 10u);
                        else return NULL;
                    }
                    if (low < 0xDC00 || low > 0xDFFF) return NULL;
                    codepoint = 0x10000u + ((codepoint - 0xD800u) << 10) + (low - 0xDC00u);
                }
                else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF)
                {
                    return NULL; /* lone low surrogate */
                }
                break;
            }
            default:
                return NULL;
            }

            if (codepoint < 0x80)
            {
                bytes[0] = (unsigned char)codepoint;
                count = 1;
            }
            else if (codepoint < 0x800)
            {
                bytes[0] = (unsigned char)(0xC0 | (codepoint >> 6));
                bytes[1] = (unsigned char)(0x80 | (codepoint & 0x3F));
                count = 2;
            }
            else if (codepoint < 0x10000)
            {
                bytes[0] = (unsigned char)(0xE0 | (codepoint >> 12));
                bytes[1] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
                bytes[2] = (unsigned char)(0x80 | (codepoint & 0x3F));
                count = 3;
            }
            else
            {
                bytes[0] = (unsigned char)(0xF0 | (codepoint >> 18));
                bytes[1] = (unsigned char)(0x80 | ((codepoint >> 12) & 0x3F));
                bytes[2] = (unsigned char)(0x80 | ((codepoint >> 6) & 0x3F));
                bytes[3] = (unsigned char)(0x80 | (codepoint & 0x3F));
                count = 4;
            }
        }
        else
        {
            bytes[0] = ch;
            count = 1;
            p++;
        }

        for (i = 0; i < count; i++)
        {
            if (out != NULL && written + 1 < out_size) out[written++] = (char)bytes[i];
            total++;
        }
    }

    return NULL; /* unterminated */
}

static const char* skip_value_depth(const char* p, const char* end, int depth);

static const char* skip_container(const char* p, const char* end, int depth, char open, char close)
{
    p++;
    p = skip_space(p, end);
    if (p < end && *p == close) return p + 1;

    for (;;)
    {
        if (open == '{')
        {
            size_t key_length = 0;
            p = skip_space(p, end);
            p = decode_string(p, end, NULL, 0, &key_length);
            if (p == NULL) return NULL;
            p = skip_space(p, end);
            if (p >= end || *p != ':') return NULL;
            p++;
        }
        p = skip_value_depth(p, end, depth + 1);
        if (p == NULL) return NULL;
        p = skip_space(p, end);
        if (p >= end) return NULL;
        if (*p == close) return p + 1;
        if (*p != ',') return NULL;
        p++;
    }
}

static const char* skip_value_depth(const char* p, const char* end, int depth)
{
    if (depth > NX_JSON_MAX_DEPTH) return NULL;
    p = skip_space(p, end);
    if (p >= end) return NULL;

    switch (*p)
    {
    case '"':
        return decode_string(p, end, NULL, 0, NULL);
    case '{':
        return skip_container(p, end, depth, '{', '}');
    case '[':
        return skip_container(p, end, depth, '[', ']');
    case 't':
        if (end - p >= 4 && strncmp(p, "true", 4) == 0) return p + 4;
        return NULL;
    case 'f':
        if (end - p >= 5 && strncmp(p, "false", 5) == 0) return p + 5;
        return NULL;
    case 'n':
        if (end - p >= 4 && strncmp(p, "null", 4) == 0) return p + 4;
        return NULL;
    default:
    {
        const char* start = p;
        if (*p == '-') p++;
        if (p >= end || *p < '0' || *p > '9') return NULL;
        while (p < end && *p >= '0' && *p <= '9') p++;
        if (p < end && *p == '.')
        {
            p++;
            if (p >= end || *p < '0' || *p > '9') return NULL;
            while (p < end && *p >= '0' && *p <= '9') p++;
        }
        if (p < end && (*p == 'e' || *p == 'E'))
        {
            p++;
            if (p < end && (*p == '+' || *p == '-')) p++;
            if (p >= end || *p < '0' || *p > '9') return NULL;
            while (p < end && *p >= '0' && *p <= '9') p++;
        }
        return p > start ? p : NULL;
    }
    }
}

static int is_whitespace(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static int text_bounds(const char* text, const char** out_begin, const char** out_end)
{
    size_t length;

    if (text == NULL) return 0;
    length = strlen(text);
    if (length == 0) return 0;
    *out_begin = text;
    *out_end = text + length;
    return 1;
}

int nx_json_object_get(NxJsonValue object, const char* key, NxJsonValue* out)
{
    const char* p;
    size_t key_length;

    if (out == NULL || key == NULL) return 0;
    if (object.begin == NULL || object.end == NULL) return 0;
    if (skip_value_depth(object.begin, object.end, 0) == NULL) return 0;
    p = skip_space(object.begin, object.end);
    if (p >= object.end || *p != '{') return 0;
    p++;
    key_length = strlen(key);
    p = skip_space(p, object.end);
    if (p < object.end && *p == '}') return 0;

    for (;;)
    {
        char name[64];
        size_t length = 0;
        const char* after;

        p = skip_space(p, object.end);
        p = decode_string(p, object.end, name, sizeof name, &length);
        if (p == NULL) return 0;
        p = skip_space(p, object.end);
        if (p >= object.end || *p != ':') return 0;
        p++;
        p = skip_space(p, object.end);

        after = skip_value_depth(p, object.end, 0);
        if (after == NULL) return 0;
        if (length == key_length && strncmp(name, key, key_length) == 0)
        {
            out->begin = p;
            out->end = after;
            return 1;
        }
        p = after;

        p = skip_space(p, object.end);
        if (p >= object.end) return 0;
        if (*p == '}') return 0;
        if (*p != ',') return 0;
        p++;
    }
}

int nx_json_find(const char* text, const char* path, NxJsonValue* out)
{
    const char* begin;
    const char* end;
    NxJsonValue current;
    const char* segment;
    char name[64];

    if (out == NULL || path == NULL || path[0] == '\0') return 0;
    if (!text_bounds(text, &begin, &end)) return 0;

    current.begin = begin;
    current.end = end;
    segment = path;

    for (;;)
    {
        const char* dot = strchr(segment, '.');
        size_t length = dot != NULL ? (size_t)(dot - segment) : strlen(segment);

        if (length == 0 || length >= sizeof name) return 0;
        memcpy(name, segment, length);
        name[length] = '\0';

        {
            NxJsonValue next;
            if (!nx_json_object_get(current, name, &next)) return 0;
            current = next;
        }

        if (dot == NULL) break;
        segment = dot + 1;
    }

    *out = current;
    return 1;
}

int nx_json_is_null(NxJsonValue value)
{
    const char* p;

    if (value.begin == NULL) return 1;
    p = skip_space(value.begin, value.end);
    return (size_t)(value.end - p) >= 4 && strncmp(p, "null", 4) == 0;
}

int nx_json_get_string(NxJsonValue value, char* out, size_t out_size)
{
    const char* p;
    size_t length = 0;

    if (out == NULL || out_size == 0) return 0;
    out[0] = '\0';
    if (value.begin == NULL || value.end == NULL) return 0;
    p = skip_space(value.begin, value.end);
    if (p >= value.end || *p != '"') return 0;
    if (decode_string(p, value.end, out, out_size, &length) == NULL) return 0;
    return 1;
}

int nx_json_get_long(NxJsonValue value, long* out)
{
    char buffer[40];
    size_t length;
    char* endptr = NULL;
    long parsed;

    if (out == NULL || value.begin == NULL) return 0;
    length = (size_t)(value.end - value.begin);
    if (length == 0 || length >= sizeof buffer) return 0;
    memcpy(buffer, value.begin, length);
    buffer[length] = '\0';

    errno = 0;
    parsed = strtol(buffer, &endptr, 10);
    if (errno != 0 || endptr == buffer) return 0;
    while (endptr < buffer + length && is_whitespace(*endptr)) endptr++;
    if (endptr != buffer + length) return 0;
    *out = parsed;
    return 1;
}

int nx_json_get_int(NxJsonValue value, int* out)
{
    long parsed;

    if (out == NULL) return 0;
    if (!nx_json_get_long(value, &parsed)) return 0;
    if (parsed < -2147483647L - 1L || parsed > 2147483647L) return 0;
    *out = (int)parsed;
    return 1;
}

int nx_json_get_ull(NxJsonValue value, unsigned long long* out)
{
    char buffer[40];
    size_t length;
    char* endptr = NULL;
    unsigned long long parsed;

    if (out == NULL || value.begin == NULL) return 0;
    length = (size_t)(value.end - value.begin);
    if (length == 0 || length >= sizeof buffer) return 0;
    memcpy(buffer, value.begin, length);
    buffer[length] = '\0';
    if (buffer[0] == '-') return 0;

    errno = 0;
    parsed = strtoull(buffer, &endptr, 10);
    if (errno != 0 || endptr == buffer) return 0;
    while (endptr < buffer + length && is_whitespace(*endptr)) endptr++;
    if (endptr != buffer + length) return 0;
    *out = parsed;
    return 1;
}

int nx_json_get_bool(NxJsonValue value, int* out)
{
    const char* p;

    if (out == NULL || value.begin == NULL) return 0;
    p = skip_space(value.begin, value.end);
    if ((size_t)(value.end - p) >= 4 && strncmp(p, "true", 4) == 0)
    {
        *out = 1;
        return 1;
    }
    if ((size_t)(value.end - p) >= 5 && strncmp(p, "false", 5) == 0)
    {
        *out = 0;
        return 1;
    }
    return 0;
}


int nx_json_array_contains_string(NxJsonValue value, const char* needle)
{
    size_t cursor = 0;
    char item[128];

    if (value.begin == NULL || needle == NULL) return 0;
    for (;;)
    {
        int result = nx_json_array_next_string(value, &cursor, item, sizeof item);
        if (result <= 0) return result < 0 ? -1 : 0;
        if (strcmp(item, needle) == 0) return 1;
    }
}

int nx_json_array_next_string(NxJsonValue value, size_t* cursor, char* out, size_t out_size)
{
    const char* p;
    size_t index = 0;
    size_t want;

    if (value.begin == NULL || cursor == NULL || out == NULL || out_size == 0) return -1;
    p = skip_space(value.begin, value.end);
    if (p >= value.end || *p != '[') return -1;
    p++;
    want = *cursor;

    for (;;)
    {
        size_t length = 0;

        p = skip_space(p, value.end);
        if (p >= value.end) return -1;
        if (*p == ']') return 0;

        if (*p == '"')
        {
            const char* after = decode_string(p, value.end, index == want ? out : NULL,
                                              index == want ? out_size : 0, &length);
            if (after == NULL) return -1;
            if (index == want)
            {
                *cursor = want + 1;
                return 1;
            }
            p = after;
        }
        else
        {
            const char* after = skip_value_depth(p, value.end, 0);
            if (after == NULL) return -1;
            p = after;
        }

        p = skip_space(p, value.end);
        if (p >= value.end) return -1;
        if (*p == ',')
        {
            p++;
            index++;
            continue;
        }
        if (*p == ']') return 0;
        return -1;
    }
}

int nx_json_escape(const char* input, char* out, size_t out_size)
{
    size_t used = 0;
    const unsigned char* p = (const unsigned char*)(input != NULL ? input : "");

    if (out == NULL || out_size == 0) return 0;

    while (*p != '\0')
    {
        unsigned char ch = *p++;
        const char* replacement = NULL;
        char unicode[7];

        switch (ch)
        {
        case '"': replacement = "\\\""; break;
        case '\\': replacement = "\\\\"; break;
        case '\b': replacement = "\\b"; break;
        case '\f': replacement = "\\f"; break;
        case '\n': replacement = "\\n"; break;
        case '\r': replacement = "\\r"; break;
        case '\t': replacement = "\\t"; break;
        default:
            if (ch < 0x20)
            {
                snprintf(unicode, sizeof unicode, "\\u%04x", (unsigned)ch);
                replacement = unicode;
            }
            break;
        }

        if (replacement != NULL)
        {
            size_t length = strlen(replacement);
            if (used + length + 1 > out_size) return 0;
            memcpy(out + used, replacement, length);
            used += length;
        }
        else
        {
            if (used + 2 > out_size) return 0;
            out[used++] = (char)ch;
        }
    }

    out[used] = '\0';
    return 1;
}

void nx_writer_init(NxWriter* writer, char* out, size_t out_size)
{
    if (writer == NULL) return;
    writer->out = out;
    writer->size = out_size;
    writer->length = 0;
    writer->ok = out != NULL && out_size > 0;
    if (writer->ok) out[0] = '\0';
}

void nx_writer_raw(NxWriter* writer, const char* text)
{
    size_t length;

    if (writer == NULL || !writer->ok || text == NULL) return;
    length = strlen(text);
    if (writer->length + length >= writer->size)
    {
        writer->ok = 0;
        return;
    }
    memcpy(writer->out + writer->length, text, length);
    writer->length += length;
    writer->out[writer->length] = '\0';
}

void nx_writer_char(NxWriter* writer, char ch)
{
    if (writer == NULL || !writer->ok) return;
    if (writer->length + 1 >= writer->size)
    {
        writer->ok = 0;
        return;
    }
    writer->out[writer->length++] = ch;
    writer->out[writer->length] = '\0';
}

void nx_writer_string(NxWriter* writer, const char* text)
{
    char escaped[NX_JSON_ESCAPE_MAX];

    if (writer == NULL || !writer->ok) return;
    if (text == NULL) text = "";
    if (strlen(text) >= sizeof escaped || !nx_json_escape(text, escaped, sizeof escaped))
    {
        writer->ok = 0;
        return;
    }
    nx_writer_char(writer, '"');
    nx_writer_raw(writer, escaped);
    nx_writer_char(writer, '"');
}

void nx_writer_key(NxWriter* writer, const char* key)
{
    nx_writer_string(writer, key);
    nx_writer_char(writer, ':');
}

void nx_writer_int(NxWriter* writer, int value)
{
    char text[32];
    snprintf(text, sizeof text, "%d", value);
    nx_writer_raw(writer, text);
}

void nx_writer_long(NxWriter* writer, long value)
{
    char text[32];
    snprintf(text, sizeof text, "%ld", value);
    nx_writer_raw(writer, text);
}

void nx_writer_ull(NxWriter* writer, unsigned long long value)
{
    char text[32];
    snprintf(text, sizeof text, "%llu", value);
    nx_writer_raw(writer, text);
}

void nx_writer_bool(NxWriter* writer, int value)
{
    nx_writer_raw(writer, value ? "true" : "false");
}

void nx_writer_prefix(NxWriter* writer, int* first)
{
    if (writer == NULL || first == NULL) return;
    if (!*first) nx_writer_char(writer, ',');
    *first = 0;
}

int nx_writer_finish(NxWriter* writer)
{
    return writer != NULL && writer->ok;
}
