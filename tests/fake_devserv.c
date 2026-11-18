/*
 * Fake devserv for host tests. It implements the protocol-1 subset the SDK
 * uses, on a real Unix socket, with the same framing as devserv:
 *   - request connections: one line in, one line out, close;
 *   - event connections: subscribe -> subscription.ready, then events;
 *   - control operations (test.*) that let a test emit events on demand.
 *
 * It is deliberately independent of the client's JSON parser: test inputs are
 * canonical JSON written by the SDK, and the fixture scans for exact keys.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 16
#define CLIENT_BUFFER 32768
#define LINE_BUFFER 16384


typedef struct
{
    int fd;
    int is_event;
    unsigned next_seq;
    char buffer[CLIENT_BUFFER];
    size_t length;
} Client;

static Client clients[MAX_CLIENTS];
static const char* scenario = "adapted";
static FILE* log_file = NULL;
static int running = 1;
static int adapted = 1;
static int overlay_declared = 1;
static int overlay_host = 1;
static int session_active = 1;
static int battery_available = 1;
static int battery_percent = 87;
static int battery_charging = 1;
static int battery_usb = 0;
static int volume_level = 8;
static int overlay_open = 0;
static int policy_volume_app = 1;
static int policy_brightness_app = 1;
static int policy_colortemp_app = 1;
static int policy_mute_app = 1;
static char session_id[17] = "0123456789abcdef";

static void close_client(Client* client);

static unsigned long long now_ms(void)
{
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) return 0;
    return (unsigned long long)time.tv_sec * 1000ULL + (unsigned long long)time.tv_nsec / 1000000ULL;
}

static void log_line(const char* prefix, const char* line)
{
    if (log_file == NULL) return;
    fprintf(log_file, "%s %s\n", prefix, line);
    fflush(log_file);
}

static int write_all(int fd, const char* data, size_t length)
{
    size_t written = 0;
    while (written < length)
    {
        ssize_t count = send(fd, data + written, length - written, 0);
        if (count < 0)
        {
            if (errno == EINTR) continue;
            return 0;
        }
        written += (size_t)count;
    }
    return 1;
}

static void send_line(int fd, const char* line)
{
    if (!write_all(fd, line, strlen(line)) || !write_all(fd, "\n", 1)) return;
}

static void send_json(int fd, const char* format, ...)
{
    char line[LINE_BUFFER];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    send_line(fd, line);
}

static int field_string(const char* line, const char* key, char* out, size_t out_size)
{
    char needle[64];
    const char* cursor;
    size_t used = 0;

    if (out == NULL || out_size == 0) return 0;
    out[0] = '\0';
    snprintf(needle, sizeof needle, "\"%s\":\"", key);
    cursor = strstr(line, needle);
    if (cursor == NULL) return 0;
    cursor += strlen(needle);
    while (cursor[used] != '\0' && cursor[used] != '"' && used + 1 < out_size)
    {
        out[used] = cursor[used];
        used++;
    }
    out[used] = '\0';
    return 1;
}

static int field_int(const char* line, const char* key, int* out)
{
    char needle[64];
    const char* cursor;
    if (out == NULL) return 0;
    snprintf(needle, sizeof needle, "\"%s\":", key);
    cursor = strstr(line, needle);
    if (cursor == NULL) return 0;
    cursor += strlen(needle);
    *out = atoi(cursor);
    return 1;
}

static int field_bool(const char* line, const char* key, int* out)
{
    char needle[64];
    const char* cursor;
    if (out == NULL) return 0;
    snprintf(needle, sizeof needle, "\"%s\":", key);
    cursor = strstr(line, needle);
    if (cursor == NULL) return 0;
    cursor += strlen(needle);
    if (strncmp(cursor, "true", 4) == 0) { *out = 1; return 1; }
    if (strncmp(cursor, "false", 5) == 0) { *out = 0; return 1; }
    return 0;
}

static int is_op(const char* line, const char* op)
{
    char value[64];
    if (!field_string(line, "op", value, sizeof value)) return 0;
    return strcmp(value, op) == 0;
}

static const char* policy_name(int app) { return app ? "app" : "system"; }

static int session_matches_request(const char* line)
{
    char supplied[32];
    if (!field_string(line, "session_id", supplied, sizeof supplied)) return 1;
    return strcmp(supplied, session_id) == 0;
}

static void battery_json(char* out, size_t size)
{
    if (!battery_available)
        snprintf(out, size, "{\"available\":false,\"detail\":\"desktop battery stub\"}");
    else
        snprintf(out, size,
                 "{\"available\":true,\"percent\":%d,\"charging\":%s,\"usb\":%s}",
                 battery_percent, battery_charging ? "true" : "false",
                 battery_usb ? "true" : "false");
}

static void status_fields(char* out, size_t size)
{
    char battery[160];
    battery_json(battery, sizeof battery);
    snprintf(out, size,
             "\"battery\":%s,"
             "\"wifi\":{\"available\":true,\"enabled\":true,\"connected\":true,"
             "\"strength\":\"high\",\"ssid\":\"TestNet\"},"
             "\"bluetooth\":{\"available\":true,\"enabled\":false,\"connected\":false},"
             "\"volume\":{\"available\":true,\"level\":%d,\"min\":0,\"max\":20,"
             "\"mute\":false,\"mute_supported\":true,\"sink\":\"default\",\"locked\":false},"
             "\"display\":{\"available\":true,\"brightness\":5,\"brightness_min\":0,"
             "\"brightness_max\":10,\"brightness_locked\":false,\"colortemp\":20,"
             "\"colortemp_min\":0,\"colortemp_max\":40,\"colortemp_locked\":false},"
             "\"theme\":{\"available\":true,\"main\":\"#F5F5FA\",\"accent\":\"#2257FF\","
             "\"accent2\":\"#3A3A44\",\"list_text\":\"#F5F5FA\","
             "\"list_text_selected\":\"#FFFFFF\",\"hint\":\"#9E9EAA\","
             "\"background\":\"#141419\"},"
             "\"clock\":{\"show\":true,\"hour24\":true}",
             battery, volume_level);
}

static void session_field(char* out, size_t size)
{
    if (!session_active)
    {
        snprintf(out, size, "\"session\":{\"active\":false}");
        return;
    }
    snprintf(out, size,
             "\"session\":{\"active\":true,\"session_id\":\"%s\",\"app\":\"com.example.demo\","
             "\"pak\":\"/tmp/Demo.pak\",\"overlay_host\":%s,"
             "\"overlay_host_declared\":%s,\"started_at\":1760000000,"
             "\"matches_pak\":true}",
             session_id, overlay_host ? "true" : "false",
             overlay_declared ? "true" : "false");
}

static void hello_response(int fd)
{
    char session[512];
    char battery[160];
    session_field(session, sizeof session);
    battery_json(battery, sizeof battery);
    send_json(fd,
              "{\"ok\":true,\"service\":\"devserv\",\"version\":1,\"protocol\":1,"
              "\"contract\":\"nextui-app/1\","
              "\"features\":[\"events\",\"sleep\",\"status\",\"hud\",\"overlay\",\"battery\"],"
              "\"limits\":{\"line_max\":8192,\"event_queue\":64,\"subscribe_max\":8,"
              "\"sleep_ack_ms\":1500,\"inhibit_max_ms\":300000,\"sleep_cooldown_ms\":1000,"
              "\"heartbeat_ms\":15000,\"status_min_interval_ms\":500,"
              "\"status_resync_ms\":30000,\"feedback_min_interval_ms\":100},"
              "%s,\"battery\":%s,\"at_ms\":%llu}",
              session, battery, now_ms());
}

static void emit_event(const char* name, const char* payload)
{
    int index;

    for (index = 0; index < MAX_CLIENTS; index++)
    {
        Client* client = &clients[index];
        char line[LINE_BUFFER];
        char session[64] = "";

        if (client->fd < 0 || !client->is_event) continue;
        if (session_active) snprintf(session, sizeof session, ",\"session_id\":\"%s\"", session_id);
        snprintf(line, sizeof line, "{\"event\":\"%s\",\"seq\":%u,\"at_ms\":%llu%s%s}",
                 name, client->next_seq++, now_ms(), session,
                 payload != NULL ? payload : "");
        send_line(client->fd, line);
    }
}

static void send_snapshot(Client* client)
{
    char fields[2048];
    char session[64] = "";
    char line[LINE_BUFFER];

    status_fields(fields, sizeof fields);
    if (session_active) snprintf(session, sizeof session, ",\"session_id\":\"%s\"", session_id);
    snprintf(line, sizeof line,
             "{\"event\":\"status.snapshot\",\"seq\":%u,\"at_ms\":%llu%s,"
             "\"overlay_open\":%s,%s}",
             client->next_seq++, now_ms(), session,
             overlay_open ? "true" : "false", fields);
    send_line(client->fd, line);
    log_line("EVT", line);
}

static void handle_subscribe(Client* client, const char* line)
{
    char session[64] = "";

    (void)line;
    client->is_event = 1;
    client->next_seq = 0;
    if (session_active) snprintf(session, sizeof session, ",\"session_id\":\"%s\"", session_id);
    send_json(client->fd,
              "{\"ok\":true,\"event\":\"subscription.ready\",\"seq\":0,\"at_ms\":%llu,"
              "\"service\":\"devserv\",\"protocol\":1,"
              "\"features\":[\"events\",\"sleep\",\"status\",\"hud\",\"overlay\"],"
              "\"overlay_host\":%s,\"overlay_host_declared\":%s%s,"
              "\"limits\":{\"sleep_ack_ms\":1500,\"heartbeat_ms\":15000}}",
              now_ms(), overlay_host ? "true" : "false",
              overlay_declared ? "true" : "false", session);
    log_line("EVT", "subscription.ready");
    client->next_seq = 1;
    send_snapshot(client);
}

static void handle_control(int fd, const char* line)
{
    if (is_op(line, "test.sleep_prepare"))
    {
        char sleep_id[64] = "9f3c1a2b4d5e6f70";
        char reason[32] = "user";
        char payload[512];
        int deadline = 1500;
        (void)field_string(line, "sleep_id", sleep_id, sizeof sleep_id);
        (void)field_string(line, "reason", reason, sizeof reason);
        (void)field_int(line, "deadline_ms", &deadline);
        snprintf(payload, sizeof payload,
                 ",\"sleep_id\":\"%s\",\"reason\":\"%s\",\"deadline_ms\":%d,"
                 "\"deadline_at_ms\":%llu,\"battery\":{\"available\":true,\"percent\":87,"
                 "\"charging\":false,\"usb\":false}",
                 sleep_id, reason, deadline, now_ms() + (unsigned long long)deadline);
        emit_event("sleep.prepare", payload);
        send_json(fd, "{\"ok\":true}");
        return;
    }
    if (is_op(line, "test.feedback"))
    {
        char kind[32] = "volume";
        char payload[256];
        int value = 7;
        (void)field_string(line, "kind", kind, sizeof kind);
        (void)field_int(line, "value", &value);
        snprintf(payload, sizeof payload,
                 ",\"kind\":\"%s\",\"value\":%d,\"min\":0,\"max\":20,\"mute\":false,"
                 "\"sink\":\"default\",\"locked\":false,\"overlay_open\":%s,"
                 "\"source\":\"key\"",
                 kind, value, overlay_open ? "true" : "false");
        emit_event("feedback", payload);
        send_json(fd, "{\"ok\":true}");
        return;
    }
    if (is_op(line, "test.wake"))
    {
        char sleep_id[64] = "9f3c1a2b4d5e6f70";
        char payload[384];
        int slept = 61000;
        (void)field_string(line, "sleep_id", sleep_id, sizeof sleep_id);
        (void)field_int(line, "slept_ms", &slept);
        snprintf(payload, sizeof payload,
                 ",\"sleep_id\":\"%s\",\"slept_ms\":%d,\"reason\":\"user\","
                 "\"woke_by\":\"power\",\"result\":\"ok\","
                 "\"battery\":{\"available\":true,\"percent\":86,\"charging\":false,"
                 "\"usb\":false}",
                 sleep_id, slept);
        emit_event("wake.done", payload);
        send_json(fd, "{\"ok\":true}");
        return;
    }
    if (is_op(line, "test.status"))
    {
        char payload[512];
        (void)field_int(line, "percent", &battery_percent);
        (void)field_int(line, "level", &volume_level);
        snprintf(payload, sizeof payload,
                 ",\"changed\":[\"battery\",\"volume\"],"
                 "\"battery\":{\"available\":true,\"percent\":%d,\"charging\":false,"
                 "\"usb\":false},"
                 "\"volume\":{\"available\":true,\"level\":%d,\"min\":0,\"max\":20,"
                 "\"mute\":false,\"mute_supported\":true,\"sink\":\"default\","
                 "\"locked\":false}",
                 battery_percent, volume_level);
        emit_event("status.changed", payload);
        send_json(fd, "{\"ok\":true}");
        return;
    }
    if (is_op(line, "test.session_ended"))
    {
        char reason[32] = "stopped";
        char payload[128];
        (void)field_string(line, "reason", reason, sizeof reason);
        snprintf(payload, sizeof payload, ",\"reason\":\"%s\"", reason);
        emit_event("session.ended", payload);
        session_active = 0;
        send_json(fd, "{\"ok\":true}");
        return;
    }
    if (is_op(line, "test.overflow"))
    {
        emit_event("overflow", ",\"dropped\":3");
        send_json(fd, "{\"ok\":true}");
        return;
    }
    if (is_op(line, "test.overlay_home"))
    {
        emit_event("overlay.home", ",\"action\":\"home\"");
        send_json(fd, "{\"ok\":true}");
        return;
    }
    if (is_op(line, "test.rotate_session"))
    {
        snprintf(session_id, sizeof session_id, "fedcba9876543210");
        send_json(fd, "{\"ok\":true}");
        return;
    }
    if (is_op(line, "test.reset_host"))
    {
        overlay_host = overlay_declared;
        overlay_open = 0;
        send_json(fd, "{\"ok\":true}");
        return;
    }
    if (is_op(line, "test.close_events"))
    {
        int index;
        for (index = 0; index < MAX_CLIENTS; index++)
        {
            if (clients[index].fd >= 0 && clients[index].is_event)
                close_client(&clients[index]);
        }
        send_json(fd, "{\"ok\":true}");
        return;
    }
    send_json(fd, "{\"ok\":false,\"error\":\"bad_request\",\"detail\":\"unknown test op\"}");
}

static void handle_request(Client* client, const char* line)
{
    int fd = client->fd;

    log_line("REQ", line);

    if (strcmp(scenario, "garbage") == 0)
    {
        send_line(fd, "this is not json");
        return;
    }

    if (is_op(line, "hello"))
    {
        if (strcmp(scenario, "legacy") == 0)
        {
            send_json(fd, "{\"ok\":false,\"error\":\"bad_request\","
                          "\"detail\":\"unknown operation\"}");
            return;
        }
        hello_response(fd);
        return;
    }
    if (is_op(line, "ping"))
    {
        send_json(fd, "{\"ok\":true,\"service\":\"devserv\",\"version\":1}");
        return;
    }
    if (is_op(line, "subscribe"))
    {
        handle_subscribe(client, line);
        return;
    }
    if (is_op(line, "battery.status"))
    {
        char battery[160];
        battery_json(battery, sizeof battery);
        send_json(fd, "{\"ok\":true,\"service\":\"devserv\",\"at_ms\":%llu,\"battery\":%s}",
                  now_ms(), battery);
        return;
    }
    if (is_op(line, "status.get"))
    {
        char fields[2048];
        status_fields(fields, sizeof fields);
        send_json(fd,
                  "{\"ok\":true,\"service\":\"devserv\",\"at_ms\":%llu,\"overlay_open\":%s,%s}",
                  now_ms(), overlay_open ? "true" : "false", fields);
        return;
    }
    if (is_op(line, "sleep.status"))
    {
        if (strcmp(scenario, "sleep_state") == 0)
        {
            unsigned long long now = now_ms();
            send_json(fd,
                      "{\"ok\":true,\"service\":\"devserv\",\"phase\":\"preparing\","
                      "\"at_ms\":%llu,"
                      "\"pending\":{\"sleep_id\":\"aaaabbbbccccdddd\","
                      "\"reason\":\"user\",\"deadline_ms\":800,"
                      "\"deadline_at_ms\":%llu,\"subscribers\":1,\"acks\":0},"
                      "\"inhibit\":{\"scope\":\"automatic\",\"reason\":\"playing_video\","
                      "\"expires_at_ms\":%llu},"
                      "\"last\":{\"sleep_id\":\"1111222233334444\",\"reason\":\"idle\","
                      "\"result\":\"ok\",\"woke_by\":\"power\",\"slept_ms\":61000}}",
                      now, now + 800, now + 60000);
            return;
        }
        send_json(fd, "{\"ok\":true,\"service\":\"devserv\",\"phase\":\"awake\","
                      "\"at_ms\":%llu,\"pending\":null,\"inhibit\":null,\"last\":null}",
                  now_ms());
        return;
    }
    if (is_op(line, "sleep.ack") || is_op(line, "sleep.inhibit") || is_op(line, "sleep.release"))
    {
        if (!session_matches_request(line))
        {
            send_json(fd, "{\"ok\":false,\"error\":\"session_stale\","
                          "\"detail\":\"session id does not match\"}");
            return;
        }
        if (strcmp(scenario, "stale_sleep") == 0 && !is_op(line, "sleep.release"))
        {
            send_json(fd, "{\"ok\":false,\"error\":\"sleep_stale\","
                          "\"detail\":\"the sleep attempt already resolved\"}");
            return;
        }
        if (is_op(line, "sleep.inhibit"))
        {
            char reason[64] = "app";
            (void)field_string(line, "reason", reason, sizeof reason);
            send_json(fd, "{\"ok\":true,\"scope\":\"prepare\",\"decision\":\"defer\","
                          "\"inhibit_active\":true,\"expires_at_ms\":%llu,\"reason\":\"%s\"}",
                      now_ms() + 60000, reason);
            return;
        }
        if (is_op(line, "sleep.release"))
        {
            send_json(fd, "{\"ok\":true,\"released\":true,\"scope\":\"automatic\"}");
            return;
        }
        send_json(fd, "{\"ok\":true,\"result\":\"accepted\",\"decision\":\"proceed\"}");
        return;
    }
    if (is_op(line, "hud.policy"))
    {
        char mode[16];
        if (!session_matches_request(line))
        {
            send_json(fd, "{\"ok\":false,\"error\":\"session_stale\","
                          "\"detail\":\"session id does not match\"}");
            return;
        }
        if (field_string(line, "mode", mode, sizeof mode))
        {
            int app = strcmp(mode, "app") == 0;
            policy_volume_app = app;
            policy_brightness_app = app;
            policy_colortemp_app = app;
            policy_mute_app = app;
        }
        send_json(fd,
                  "{\"ok\":true,\"service\":\"devserv\",\"session_id\":\"%s\","
                  "\"overlay_open\":%s,\"overlay_host\":%s,"
                  "\"policy\":{\"volume\":\"%s\",\"brightness\":\"%s\","
                  "\"colortemp\":\"%s\",\"mute\":\"%s\"},"
                  "\"effective\":{\"volume\":\"%s\",\"brightness\":\"%s\","
                  "\"colortemp\":\"%s\",\"mute\":\"%s\"}}",
                  session_id, overlay_open ? "true" : "false",
                  overlay_host ? "true" : "false",
                  policy_name(policy_volume_app), policy_name(policy_brightness_app),
                  policy_name(policy_colortemp_app), policy_name(policy_mute_app),
                  policy_name(overlay_host && policy_volume_app),
                  policy_name(overlay_host && policy_brightness_app),
                  policy_name(overlay_host && policy_colortemp_app),
                  policy_name(overlay_host && policy_mute_app));
        return;
    }
    if (is_op(line, "overlay.host"))
    {
        int host = 0;
        if (!session_matches_request(line))
        {
            send_json(fd, "{\"ok\":false,\"error\":\"session_stale\","
                          "\"detail\":\"session id does not match\"}");
            return;
        }
        if (!session_active)
        {
            send_json(fd, "{\"ok\":false,\"error\":\"not_active\","
                          "\"detail\":\"no application session is active\"}");
            return;
        }
        if (!field_bool(line, "host", &host))
        {
            send_json(fd, "{\"ok\":false,\"error\":\"bad_request\","
                          "\"detail\":\"host is required\"}");
            return;
        }
        if (host && !overlay_declared)
        {
            send_json(fd, "{\"ok\":false,\"error\":\"not_adapted\","
                          "\"detail\":\"the session does not declare the overlay\"}");
            return;
        }
        overlay_host = host;
        if (!overlay_host) overlay_open = 0;
        send_json(fd, "{\"ok\":true,\"service\":\"devserv\",\"session_id\":\"%s\","
                      "\"overlay_host\":%s,\"overlay_host_declared\":%s,"
                      "\"host_changed_at_ms\":%llu,\"overlay_open\":%s,"
                      "\"effective_hud\":\"%s\"}",
                  session_id, overlay_host ? "true" : "false",
                  overlay_declared ? "true" : "false", now_ms(),
                  overlay_open ? "true" : "false",
                  overlay_host ? "app" : "system");
        return;
    }
    if (is_op(line, "overlay.state"))
    {
        int open = 0;
        if (!session_matches_request(line))
        {
            send_json(fd, "{\"ok\":false,\"error\":\"session_stale\","
                          "\"detail\":\"session id does not match\"}");
            return;
        }
        (void)field_bool(line, "open", &open);
        if (!session_active)
        {
            send_json(fd, "{\"ok\":false,\"error\":\"not_active\","
                          "\"detail\":\"no application session is active\"}");
            return;
        }
        if (!overlay_declared)
        {
            send_json(fd, "{\"ok\":false,\"error\":\"not_adapted\","
                          "\"detail\":\"the session does not declare the overlay\"}");
            return;
        }
        if (!overlay_host)
        {
            send_json(fd, "{\"ok\":false,\"error\":\"not_hosting\","
                          "\"detail\":\"the application currently yields the overlay\"}");
            return;
        }
        overlay_open = open;
        send_json(fd, "{\"ok\":true,\"service\":\"devserv\",\"session_id\":\"%s\","
                      "\"overlay_host\":true,\"overlay_open\":%s}",
                  session_id, overlay_open ? "true" : "false");
        return;
    }
    if (is_op(line, "app-action"))
    {
        char action[32] = "";
        int app = 0;
        int system = 0;
        (void)field_string(line, "action", action, sizeof action);
        if (strcmp(action, "overlay") == 0 && session_active)
        {
            app = overlay_host;
            system = !overlay_host;
        }
        send_json(fd,
                  "{\"ok\":true,\"service\":\"devserv\",\"session\":\"%s\","
                  "\"session_id\":\"%s\",\"app\":\"com.example.demo\","
                  "\"action\":\"%s\",\"handled_by_app\":%s,"
                  "\"handled_by_system\":%s,\"exit_requested\":false,"
                  "\"result\":\"ok\"}",
                  session_active ? "active" : "inactive", session_id, action,
                  app ? "true" : "false", system ? "true" : "false");
        return;
    }
    if (is_op(line, "system-feedback"))
    {
        int app = overlay_host && policy_volume_app;
        send_json(fd, "{\"ok\":true,\"service\":\"devserv\",\"session\":\"%s\","
                      "\"session_id\":\"%s\",\"handled_by_app\":%s,"
                      "\"handled_by_system\":%s,\"result\":\"ok\"}",
                  session_active ? "active" : "inactive", session_id,
                  app ? "true" : "false", app ? "false" : "true");
        return;
    }
    if (is_op(line, "call"))
    {
        if (strcmp(scenario, "unavailable_call") == 0)
        {
            send_json(fd, "{\"ok\":false,\"error\":\"unavailable\","
                          "\"capability\":\"net.wifi\",\"available\":false,"
                          "\"authorized\":true,\"detail\":\"no backend\"}");
            return;
        }
        send_json(fd, "{\"ok\":true,\"capability\":\"display.get\",\"available\":true,"
                      "\"authorized\":true,\"value\":{\"level\":5}}");
        return;
    }
    if (strncmp(line, "{\"op\":\"test.", 11) == 0)
    {
        handle_control(fd, line);
        return;
    }

    send_json(fd, "{\"ok\":false,\"error\":\"bad_request\",\"detail\":\"unknown operation\"}");
}

static void close_client(Client* client)
{
    if (client->fd >= 0) close(client->fd);
    client->fd = -1;
    client->is_event = 0;
    client->length = 0;
}

static Client* add_client(int fd)
{
    int index;
    for (index = 0; index < MAX_CLIENTS; index++)
    {
        if (clients[index].fd < 0)
        {
            memset(&clients[index], 0, sizeof clients[index]);
            clients[index].fd = fd;
            clients[index].next_seq = 1;
            return &clients[index];
        }
    }
    close(fd);
    return NULL;
}

static void handle_read(Client* client)
{
    char buffer[4096];
    ssize_t count = recv(client->fd, buffer, sizeof buffer, 0);

    if (count == 0)
    {
        close_client(client);
        return;
    }
    if (count < 0)
    {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return;
        close_client(client);
        return;
    }
    if (client->length + (size_t)count >= sizeof client->buffer)
    {
        close_client(client);
        return;
    }
    memcpy(client->buffer + client->length, buffer, (size_t)count);
    client->length += (size_t)count;

    for (;;)
    {
        char* newline = memchr(client->buffer, '\n', client->length);
        size_t length;
        char line[LINE_BUFFER];

        if (newline == NULL) return;
        length = (size_t)(newline - client->buffer);
        if (length >= sizeof line) length = sizeof line - 1;
        memcpy(line, client->buffer, length);
        line[length] = '\0';
        while (length > 0 && line[length - 1] == '\r') line[--length] = '\0';
        memmove(client->buffer, newline + 1, client->length - (size_t)(newline - client->buffer) - 1);
        client->length -= (size_t)(newline - client->buffer) + 1;

        if (client->is_event)
        {
            send_json(client->fd, "{\"ok\":false,\"error\":\"bad_request\","
                                  "\"detail\":\"the event connection is write-only\"}");
            close_client(client);
            return;
        }

        handle_request(client, line);
        if (!client->is_event)
        {
            close_client(client);
            return;
        }
    }
}

static void handle_signal(int number)
{
    (void)number;
    running = 0;
}

int main(int argc, char** argv)
{
    char socket_path[108] = "";
    struct sockaddr_un address;
    struct sigaction action;
    int listener;
    int index;

    for (index = 1; index < argc; index++)
    {
        if (strcmp(argv[index], "--socket") == 0 && index + 1 < argc)
            snprintf(socket_path, sizeof socket_path, "%s", argv[++index]);
        else if (strcmp(argv[index], "--scenario") == 0 && index + 1 < argc)
            scenario = argv[++index];
        else if (strcmp(argv[index], "--log") == 0 && index + 1 < argc)
        {
            log_file = fopen(argv[++index], "a");
            if (log_file == NULL)
            {
                perror("fake-devserv: log");
                return 1;
            }
        }
        else
        {
            fprintf(stderr, "usage: fake_devserv --socket PATH [--scenario NAME] [--log PATH]\n");
            return 2;
        }
    }
    if (socket_path[0] == '\0')
    {
        fprintf(stderr, "fake_devserv: --socket is required\n");
        return 2;
    }
    if (strcmp(scenario, "unadapted") == 0) adapted = 0;
    if (strcmp(scenario, "no_session") == 0) { adapted = 0; session_active = 0; }
    if (strcmp(scenario, "desktop") == 0) battery_available = 0;
    overlay_declared = adapted;
    overlay_host = adapted;

    if (adapted) policy_volume_app = policy_brightness_app = 1;
    else policy_volume_app = policy_brightness_app = 0;
    policy_colortemp_app = policy_volume_app;
    policy_mute_app = policy_volume_app;

    for (index = 0; index < MAX_CLIENTS; index++) clients[index].fd = -1;

    signal(SIGPIPE, SIG_IGN);
    memset(&action, 0, sizeof action);
    action.sa_handler = handle_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    unlink(socket_path);
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0)
    {
        perror("fake-devserv: socket");
        return 1;
    }
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof address.sun_path, "%s", socket_path);
    if (bind(listener, (struct sockaddr*)&address, sizeof address) != 0)
    {
        perror("fake-devserv: bind");
        return 1;
    }
    chmod(socket_path, 0600);
    if (listen(listener, 8) != 0)
    {
        perror("fake-devserv: listen");
        return 1;
    }

    while (running)
    {
        struct pollfd waits[MAX_CLIENTS + 1];
        int mapping[MAX_CLIENTS + 1];
        int count = 1;
        int ready;

        waits[0].fd = listener;
        waits[0].events = POLLIN;
        waits[0].revents = 0;
        mapping[0] = -1;

        for (index = 0; index < MAX_CLIENTS; index++)
        {
            if (clients[index].fd < 0) continue;
            waits[count].fd = clients[index].fd;
            waits[count].events = POLLIN;
            waits[count].revents = 0;
            mapping[count] = index;
            count++;
        }

        ready = poll(waits, (nfds_t)count, 500);
        if (ready < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;

        if (waits[0].revents & POLLIN)
        {
            int connection = accept(listener, NULL, NULL);
            if (connection >= 0)
            {
                (void)fcntl(connection, F_SETFD, FD_CLOEXEC);
                (void)add_client(connection);
            }
        }
        for (index = 1; index < count; index++)
        {
            if (waits[index].revents & (POLLIN | POLLHUP | POLLERR))
            {
                Client* client = &clients[mapping[index]];
                if (client->fd >= 0) handle_read(client);
            }
        }
    }

    for (index = 0; index < MAX_CLIENTS; index++)
        if (clients[index].fd >= 0) close(clients[index].fd);
    close(listener);
    unlink(socket_path);
    if (log_file != NULL) fclose(log_file);
    return 0;
}
