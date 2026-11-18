#ifndef NEXTUI_JSON_INTERNAL_H
#define NEXTUI_JSON_INTERNAL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Small, dependency-free JSON helpers for the client. This is not a general
 * DOM: values are spans into the original line, and lookups walk object keys by
 * path ("session.active", "battery.percent"). Unknown fields are skipped, which
 * is what the contract requires for forward compatibility.
 */

typedef struct
{
    const char* begin;
    const char* end;
} NxJsonValue;

/* Path lookup: "a", "a.b", "a.b.c". Returns 1 when found. */
int nx_json_find(const char* text, const char* path, NxJsonValue* out);

/* Object lookup on an existing value. Returns 1 when found. */
int nx_json_object_get(NxJsonValue object, const char* key, NxJsonValue* out);

int nx_json_is_null(NxJsonValue value);
int nx_json_get_string(NxJsonValue value, char* out, size_t out_size);
int nx_json_get_long(NxJsonValue value, long* out);
int nx_json_get_int(NxJsonValue value, int* out);
int nx_json_get_bool(NxJsonValue value, int* out);
int nx_json_get_ull(NxJsonValue value, unsigned long long* out);
/* 1 when the array contains the exact string. 0 when not, -1 when not an array. */
int nx_json_array_contains_string(NxJsonValue value, const char* needle);

/* Iterate an array of strings. Start with *cursor = 0; returns 1 and fills
 * `out` for each element, 0 at the end, -1 on a malformed array. */
int nx_json_array_next_string(NxJsonValue value, size_t* cursor, char* out, size_t out_size);

/* Writer for request lines. */
typedef struct
{
    char* out;
    size_t size;
    size_t length;
    int ok;
} NxWriter;

void nx_writer_init(NxWriter* writer, char* out, size_t out_size);
void nx_writer_raw(NxWriter* writer, const char* text);
void nx_writer_char(NxWriter* writer, char ch);
void nx_writer_string(NxWriter* writer, const char* text);
void nx_writer_key(NxWriter* writer, const char* key);
void nx_writer_int(NxWriter* writer, int value);
void nx_writer_long(NxWriter* writer, long value);
void nx_writer_ull(NxWriter* writer, unsigned long long value);
void nx_writer_bool(NxWriter* writer, int value);
void nx_writer_prefix(NxWriter* writer, int* first);
int nx_writer_finish(NxWriter* writer);

/* Escape `input` into `out` (no surrounding quotes). Returns 1 on success. */
int nx_json_escape(const char* input, char* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* NEXTUI_JSON_INTERNAL_H */
