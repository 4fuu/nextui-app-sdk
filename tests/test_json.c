/* Host tests for the internal JSON helpers. */
#define _POSIX_C_SOURCE 200809L

#include "../src/nextui_json.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                                     \
    do                                                                                       \
    {                                                                                        \
        if (!(condition))                                                                    \
        {                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);             \
            failures++;                                                                      \
        }                                                                                    \
    } while (0)

static void test_lookup(void)
{
    const char* line =
        "{\"ok\":true,\"service\":\"devserv\",\"session\":{\"active\":true,"
        "\"session_id\":\"0123456789abcdef\",\"overlay_host\":false},"
        "\"battery\":{\"available\":true,\"percent\":87,\"charging\":false,\"usb\":0},"
        "\"features\":[\"events\",\"sleep\",\"status\"],\"pending\":null}";
    NxJsonValue value;

    CHECK(nx_json_find(line, "ok", &value) == 1);
    {
        int parsed = 0;
        CHECK(nx_json_get_bool(value, &parsed) == 1 && parsed == 1);
    }

    CHECK(nx_json_find(line, "session.active", &value) == 1);
    {
        int parsed = 0;
        CHECK(nx_json_get_bool(value, &parsed) == 1);
        CHECK(parsed == 1);
    }

    CHECK(nx_json_find(line, "session.session_id", &value) == 1);
    {
        char text[32];
        CHECK(nx_json_get_string(value, text, sizeof text) == 1);
        CHECK(strcmp(text, "0123456789abcdef") == 0);
    }

    CHECK(nx_json_find(line, "battery.percent", &value) == 1);
    {
        int parsed = 0;
        CHECK(nx_json_get_int(value, &parsed) == 1 && parsed == 87);
    }

    CHECK(nx_json_find(line, "battery.charging", &value) == 1);
    {
        int parsed = 1;
        CHECK(nx_json_get_bool(value, &parsed) == 1 && parsed == 0);
    }

    CHECK(nx_json_find(line, "features", &value) == 1);
    CHECK(nx_json_array_contains_string(value, "sleep") == 1);
    CHECK(nx_json_array_contains_string(value, "hud") == 0);

    CHECK(nx_json_find(line, "pending", &value) == 1);
    CHECK(nx_json_is_null(value) == 1);

    CHECK(nx_json_find(line, "missing", &value) == 0);
    CHECK(nx_json_find(line, "session.missing", &value) == 0);
    CHECK(nx_json_find(line, "battery.missing", &value) == 0);
}

static void test_escapes(void)
{
    const char* line =
        "{\"ssid\":\"Caf\\u00e9 \\\"Guest\\\"\",\"emoji\":\"\\ud83d\\ude00\","
        "\"control\":\"a\\tb\\nc\",\"slash\":\"a\\/b\"}";
    NxJsonValue value;
    char text[64];

    CHECK(nx_json_find(line, "ssid", &value) == 1);
    CHECK(nx_json_get_string(value, text, sizeof text) == 1);
    CHECK(strcmp(text, "Caf\xc3\xa9 \"Guest\"") == 0);

    CHECK(nx_json_find(line, "emoji", &value) == 1);
    CHECK(nx_json_get_string(value, text, sizeof text) == 1);
    CHECK(strcmp(text, "\xf0\x9f\x98\x80") == 0);

    CHECK(nx_json_find(line, "control", &value) == 1);
    CHECK(nx_json_get_string(value, text, sizeof text) == 1);
    CHECK(strcmp(text, "a\tb\nc") == 0);

    CHECK(nx_json_find(line, "slash", &value) == 1);
    CHECK(nx_json_get_string(value, text, sizeof text) == 1);
    CHECK(strcmp(text, "a/b") == 0);
}

static void test_numbers(void)
{
    const char* line = "{\"negative\":-42,\"zero\":0,\"big\":9007199254740993,\"float\":1.5}";
    NxJsonValue value;
    int number;
    unsigned long long large;

    CHECK(nx_json_find(line, "negative", &value) == 1);
    CHECK(nx_json_get_int(value, &number) == 1 && number == -42);
    CHECK(nx_json_find(line, "zero", &value) == 1);
    CHECK(nx_json_get_int(value, &number) == 1 && number == 0);
    CHECK(nx_json_find(line, "big", &value) == 1);
    CHECK(nx_json_get_ull(value, &large) == 1 && large == 9007199254740993ULL);
    CHECK(nx_json_find(line, "float", &value) == 1);
    CHECK(nx_json_get_int(value, &number) == 0); /* not an integer */
}

static void test_unknown_nested(void)
{
    const char* line =
        "{\"before\":1,\"unknown\":{\"a\":[1,2,{\"b\":\"c\"}],\"d\":true},"
        "\"after\":\"value\",\"array\":[{\"x\":1},2,3]}";
    NxJsonValue value;
    char text[16];

    CHECK(nx_json_find(line, "before", &value) == 1);
    CHECK(nx_json_find(line, "after", &value) == 1);
    CHECK(nx_json_get_string(value, text, sizeof text) == 1);
    CHECK(strcmp(text, "value") == 0);
    CHECK(nx_json_find(line, "unknown.d", &value) == 1);
    CHECK(nx_json_find(line, "array", &value) == 1);
}

static void test_malformed(void)
{
    NxJsonValue value;

    CHECK(nx_json_find("", "a", &value) == 0);
    CHECK(nx_json_find("{", "a", &value) == 0);
    CHECK(nx_json_find("{\"a\":}", "a", &value) == 0);
    CHECK(nx_json_find("{\"a\":1", "a", &value) == 0);
    CHECK(nx_json_find("{\"a\":\"unterminated}", "a", &value) == 0);
    CHECK(nx_json_find("not json", "a", &value) == 0);
    CHECK(nx_json_find("[1,2]", "a", &value) == 0);
}

static void test_array_iteration(void)
{
    const char* line = "{\"items\":[\"one\",\"two\",\"three\"]}";
    NxJsonValue value;
    size_t cursor = 0;
    char item[16];
    int count = 0;

    CHECK(nx_json_find(line, "items", &value) == 1);
    while (nx_json_array_next_string(value, &cursor, item, sizeof item) == 1)
    {
        static const char* expected[] = {"one", "two", "three"};
        CHECK(count < 3);
        if (count < 3) CHECK(strcmp(item, expected[count]) == 0);
        count++;
    }
    CHECK(count == 3);
}

static void test_writer(void)
{
    char out[256];

    CHECK(nx_json_escape("a\"b\\c\nd\te", out, sizeof out) == 1);
    CHECK(strcmp(out, "a\\\"b\\\\c\\nd\\te") == 0);

    {
        NxWriter writer;
        int first = 1;
        nx_writer_init(&writer, out, sizeof out);
        nx_writer_char(&writer, '{');
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "op");
        nx_writer_string(&writer, "hello");
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "session_id");
        nx_writer_string(&writer, "0123456789abcdef");
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "count");
        nx_writer_int(&writer, 7);
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "ok");
        nx_writer_bool(&writer, 1);
        nx_writer_char(&writer, '}');
        CHECK(nx_writer_finish(&writer) == 1);
        CHECK(strcmp(out,
                     "{\"op\":\"hello\",\"session_id\":\"0123456789abcdef\","
                     "\"count\":7,\"ok\":true}") == 0);
    }

    {
        NxWriter writer;
        nx_writer_init(&writer, out, 4);
        nx_writer_raw(&writer, "abcdef");
        CHECK(nx_writer_finish(&writer) == 0);
    }
}

int main(void)
{
    test_lookup();
    test_escapes();
    test_numbers();
    test_unknown_nested();
    test_malformed();
    test_array_iteration();
    test_writer();

    if (failures != 0)
    {
        fprintf(stderr, "%d JSON test(s) failed\n", failures);
        return 1;
    }
    printf("test_json: all checks passed\n");
    return 0;
}
