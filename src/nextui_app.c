#define _POSIX_C_SOURCE 200809L

#include "nextui_app.h"

#include "nextui_json.h"
#include "nextui_transport.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define NX_QUEUE_CAPACITY 32
#define NX_RECONNECT_DELAY_MS 1000
#define NX_LINE_BUFFER (NEXTUI_APP_LINE_MAX * 2)
#define NX_DEFERRED_CAPACITY 16
#define NX_ASYNC_ERROR_CAPACITY 8
#define NX_DEFERRED_WAKE_MS 100

/*
 * Work the event pump must do on the request channel (sleep ack/inhibit and
 * overlay host restore after a reconnect). It runs on the worker thread, never
 * on the pump thread: a synchronous request can block on devserv, and blocking
 * the pump there is what let a full event socket deadlock the client.
 */
typedef enum
{
    NX_DEFERRED_SLEEP_ACK = 0,
    NX_DEFERRED_SLEEP_INHIBIT,
    NX_DEFERRED_HOST_OVERRIDE
} NxDeferredKind;

typedef struct
{
    NxDeferredKind kind;
    char session_id[NEXTUI_APP_SESSION_ID_SIZE];
    char sleep_id[NEXTUI_APP_SLEEP_ID_SIZE];
    char reason[NEXTUI_APP_REASON_SIZE];
    char detail[NEXTUI_APP_DETAIL_SIZE];
    int paused;
    int saved;
    int timeout_ms;
    int host;
} NxDeferred;

struct NextuiApp
{
    /*
     * Lock design (see README "Internal locking"):
     *
     *   request_lock  serializes request-channel round trips, including the
     *                 hello/subscribe handshake. Held across socket I/O.
     *   state_lock    guards info, connected, handshaking, last_error,
     *                 last_detail and host_override. Only held briefly; never
     *                 across I/O, callbacks, poll() or recv().
     *   event_lock    guards the event queue, the cached status snapshot and
     *                 worker error events. Only held briefly; never across
     *                 I/O or callbacks.
     *
     * Ordering: request_lock -> state_lock. event_lock is independent.
     * The event pump takes state_lock and event_lock briefly and never takes
     * request_lock, so draining the subscribe socket cannot depend on an
     * in-flight request.
     */
    pthread_mutex_t request_lock;
    pthread_mutex_t state_lock;
    pthread_mutex_t event_lock;

    /* Deferred request-channel work, executed on the worker thread. */
    pthread_mutex_t deferred_lock;
    pthread_cond_t deferred_cond;
    pthread_t worker;
    int worker_started;
    int worker_stop;
    NxDeferred deferred[NX_DEFERRED_CAPACITY];
    int deferred_head;
    int deferred_count;

    char socket_path[NEXTUI_APP_SOCKET_SIZE];
    char pak[NEXTUI_APP_PAK_SIZE];
    char app[NEXTUI_APP_APP_SIZE];
    char client[NEXTUI_APP_CLIENT_SIZE];
    int connect_timeout_ms;
    int io_timeout_ms;
    int auto_reconnect;
    int handshaking;
    void (*log)(void* user, int level, const char* message);
    void* log_user;

    int connected; /* hello succeeded */
    int event_fd;
    int stopped; /* session ended: no automatic reconnect */
    int host_override; /* -1 none, 0 yielded, 1 restored; re-applied on reconnect */
    unsigned long long next_reconnect_at;
    unsigned event_features;
    int heartbeat_ms;

    NextuiAppInfo info; /* guarded by state_lock; updated by connect/reconnect */
    char last_error[NEXTUI_APP_REASON_SIZE];
    char last_detail[NEXTUI_APP_DETAIL_SIZE];

    /* Event stream state, owned by the polling thread. */
    int have_ready;
    unsigned last_seq;
    char inbuf[NX_LINE_BUFFER];
    size_t inlen;
    unsigned long long last_event_at;
    NextuiStatus status; /* guarded by event_lock */
    int have_status;

    /* Event delivery. */
    NextuiAppEvent queue[NX_QUEUE_CAPACITY];
    int queue_count; /* guarded by event_lock */
    NextuiAppEvent async_errors[NX_ASYNC_ERROR_CAPACITY];
    int async_error_head;
    int async_error_count;
    NextuiAppEventCallback event_callback;
    void* event_user;
    NextuiAppSleepHandler sleep_handler;
    void* sleep_user;
};

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */

static void copy_string(char* dst, size_t size, const char* src)
{
    if (dst == NULL || size == 0) return;
    if (src == NULL) src = "";
    snprintf(dst, size, "%s", src);
}

static void set_last_error(NextuiApp* app, const char* code, const char* detail)
{
    if (app == NULL) return;
    pthread_mutex_lock(&app->state_lock);
    copy_string(app->last_error, sizeof app->last_error, code != NULL ? code : "");
    copy_string(app->last_detail, sizeof app->last_detail, detail != NULL ? detail : "");
    pthread_mutex_unlock(&app->state_lock);
}

/* Copy the session-facing state without holding state_lock across I/O. */
static void app_info_copy(NextuiApp* app, NextuiAppInfo* out)
{
    pthread_mutex_lock(&app->state_lock);
    *out = app->info;
    pthread_mutex_unlock(&app->state_lock);
}

static void log_message(NextuiApp* app, int level, const char* message)
{
    if (app != NULL && app->log != NULL) app->log(app->log_user, level, message);
}

static void info_init(NextuiAppInfo* info)
{
    memset(info, 0, sizeof *info);
    info->protocol = NEXTUI_APP_PROTOCOL;
    info->limits_sleep_ack_ms = 1500;
    info->limits_inhibit_max_ms = 300000;
    info->limits_heartbeat_ms = 15000;
    info->limits_status_min_interval_ms = 500;
}

static int parse_features(const char* line, const char* path, unsigned* out)
{
    NxJsonValue array;
    char name[32];
    size_t cursor = 0;
    unsigned bits = 0;

    if (out == NULL) return 0;
    if (!nx_json_find(line, path, &array)) return 0;
    for (;;)
    {
        int result = nx_json_array_next_string(array, &cursor, name, sizeof name);
        if (result <= 0) break;
        if (strcmp(name, "events") == 0) bits |= NEXTUI_APP_FEATURE_EVENTS;
        else if (strcmp(name, "sleep") == 0) bits |= NEXTUI_APP_FEATURE_SLEEP;
        else if (strcmp(name, "status") == 0) bits |= NEXTUI_APP_FEATURE_STATUS;
        else if (strcmp(name, "hud") == 0) bits |= NEXTUI_APP_FEATURE_HUD;
        else if (strcmp(name, "overlay") == 0) bits |= NEXTUI_APP_FEATURE_OVERLAY;
        else if (strcmp(name, "battery") == 0) bits |= NEXTUI_APP_FEATURE_BATTERY;
    }
    *out = bits;
    return 1;
}

static int json_string(const char* line, const char* path, char* out, size_t out_size)
{
    NxJsonValue value;
    if (out != NULL && out_size > 0) out[0] = '\0';
    if (!nx_json_find(line, path, &value)) return 0;
    return nx_json_get_string(value, out, out_size);
}

static int json_int(const char* line, const char* path, int* out)
{
    NxJsonValue value;
    if (!nx_json_find(line, path, &value)) return 0;
    return nx_json_get_int(value, out);
}

static int json_bool(const char* line, const char* path, int* out)
{
    NxJsonValue value;
    if (!nx_json_find(line, path, &value)) return 0;
    return nx_json_get_bool(value, out);
}

static int json_ull(const char* line, const char* path, unsigned long long* out)
{
    NxJsonValue value;
    if (!nx_json_find(line, path, &value)) return 0;
    return nx_json_get_ull(value, out);
}

static void parse_battery_fields(const char* line, const char* prefix, NextuiBattery* battery)
{
    char path[64];

    memset(battery, 0, sizeof *battery);
    snprintf(path, sizeof path, "%s.available", prefix);
    (void)json_bool(line, path, &battery->available);
    snprintf(path, sizeof path, "%s.percent", prefix);
    (void)json_int(line, path, &battery->percent);
    snprintf(path, sizeof path, "%s.charging", prefix);
    (void)json_bool(line, path, &battery->charging);
    snprintf(path, sizeof path, "%s.usb", prefix);
    (void)json_bool(line, path, &battery->usb);
}

static uint32_t parse_color(const char* text)
{
    unsigned long value;

    if (text == NULL) return 0;
    if (text[0] == '#') text++;
    value = strtoul(text, NULL, 16);
    return (uint32_t)(value & 0xFFFFFFu);
}

static void parse_status_section(const char* line, const char* section, NextuiStatus* status,
                                 unsigned* present)
{
    char path[80];

    if (strcmp(section, "battery") == 0)
    {
        parse_battery_fields(line, "battery", &status->battery);
        *present |= NEXTUI_APP_SECTION_BATTERY;
        return;
    }

    if (strcmp(section, "wifi") == 0)
    {
        NextuiWifi* wifi = &status->wifi;
        memset(wifi, 0, sizeof *wifi);
        snprintf(path, sizeof path, "wifi.available");
        (void)json_bool(line, path, &wifi->available);
        snprintf(path, sizeof path, "wifi.enabled");
        (void)json_bool(line, path, &wifi->enabled);
        snprintf(path, sizeof path, "wifi.connected");
        (void)json_bool(line, path, &wifi->connected);
        (void)json_string(line, "wifi.strength", wifi->strength, sizeof wifi->strength);
        (void)json_string(line, "wifi.ssid", wifi->ssid, sizeof wifi->ssid);
        *present |= NEXTUI_APP_SECTION_WIFI;
        return;
    }

    if (strcmp(section, "bluetooth") == 0)
    {
        NextuiBluetooth* bluetooth = &status->bluetooth;
        memset(bluetooth, 0, sizeof *bluetooth);
        snprintf(path, sizeof path, "bluetooth.available");
        (void)json_bool(line, path, &bluetooth->available);
        snprintf(path, sizeof path, "bluetooth.enabled");
        (void)json_bool(line, path, &bluetooth->enabled);
        snprintf(path, sizeof path, "bluetooth.connected");
        (void)json_bool(line, path, &bluetooth->connected);
        (void)json_string(line, "bluetooth.name", bluetooth->name, sizeof bluetooth->name);
        *present |= NEXTUI_APP_SECTION_BLUETOOTH;
        return;
    }

    if (strcmp(section, "volume") == 0)
    {
        NextuiVolume* volume = &status->volume;
        memset(volume, 0, sizeof *volume);
        (void)json_bool(line, "volume.available", &volume->available);
        (void)json_int(line, "volume.level", &volume->level);
        (void)json_int(line, "volume.min", &volume->min);
        (void)json_int(line, "volume.max", &volume->max);
        (void)json_bool(line, "volume.mute", &volume->mute);
        (void)json_bool(line, "volume.mute_supported", &volume->mute_supported);
        (void)json_bool(line, "volume.locked", &volume->locked);
        (void)json_string(line, "volume.sink", volume->sink, sizeof volume->sink);
        *present |= NEXTUI_APP_SECTION_VOLUME;
        return;
    }

    if (strcmp(section, "display") == 0)
    {
        NextuiDisplay* display = &status->display;
        memset(display, 0, sizeof *display);
        (void)json_bool(line, "display.available", &display->available);
        (void)json_int(line, "display.brightness", &display->brightness);
        (void)json_int(line, "display.brightness_min", &display->brightness_min);
        (void)json_int(line, "display.brightness_max", &display->brightness_max);
        (void)json_int(line, "display.colortemp", &display->colortemp);
        (void)json_int(line, "display.colortemp_min", &display->colortemp_min);
        (void)json_int(line, "display.colortemp_max", &display->colortemp_max);
        (void)json_bool(line, "display.brightness_locked", &display->brightness_locked);
        (void)json_bool(line, "display.colortemp_locked", &display->colortemp_locked);
        *present |= NEXTUI_APP_SECTION_DISPLAY;
        return;
    }

    if (strcmp(section, "theme") == 0)
    {
        NextuiTheme* theme = &status->theme;
        char color[16];
        memset(theme, 0, sizeof *theme);
        (void)json_bool(line, "theme.available", &theme->available);
        if (json_string(line, "theme.main", color, sizeof color)) theme->main = parse_color(color);
        if (json_string(line, "theme.accent", color, sizeof color)) theme->accent = parse_color(color);
        if (json_string(line, "theme.accent2", color, sizeof color)) theme->accent2 = parse_color(color);
        if (json_string(line, "theme.list_text", color, sizeof color))
            theme->list_text = parse_color(color);
        if (json_string(line, "theme.list_text_selected", color, sizeof color))
            theme->list_text_selected = parse_color(color);
        if (json_string(line, "theme.hint", color, sizeof color)) theme->hint = parse_color(color);
        if (json_string(line, "theme.background", color, sizeof color))
            theme->background = parse_color(color);
        *present |= NEXTUI_APP_SECTION_THEME;
        return;
    }

    if (strcmp(section, "clock") == 0)
    {
        int show = 0;
        int hour24 = 0;
        (void)json_bool(line, "clock.show", &show);
        (void)json_bool(line, "clock.hour24", &hour24);
        status->clock.show = show;
        status->clock.hour24 = hour24;
        *present |= NEXTUI_APP_SECTION_CLOCK;
        return;
    }

}

static unsigned parse_status(const char* line, NextuiStatus* status, int clear)
{
    static const char* sections[] = {"battery", "wifi", "bluetooth", "volume",
                                     "display", "theme", "clock"};
    NxJsonValue changed;
    unsigned present = 0;
    int overlay = 0;
    size_t i;

    if (clear) memset(status, 0, sizeof *status);
    if (json_bool(line, "overlay_open", &overlay)) status->overlay_open = overlay;

    if (nx_json_find(line, "changed", &changed))
    {
        char name[32];
        size_t cursor = 0;
        while (nx_json_array_next_string(changed, &cursor, name, sizeof name) > 0)
        {
            for (i = 0; i < sizeof sections / sizeof sections[0]; i++)
            {
                if (strcmp(name, sections[i]) == 0)
                {
                    parse_status_section(line, sections[i], status, &present);
                    break;
                }
            }
        }
    }
    else
    {
        for (i = 0; i < sizeof sections / sizeof sections[0]; i++)
        {
            NxJsonValue value;
            if (nx_json_find(line, sections[i], &value))
                parse_status_section(line, sections[i], status, &present);
        }
    }

    status->sections = present;
    return present;
}

static NextuiAppHudKind parse_hud_kind(const char* name)
{
    if (name == NULL) return NEXTUI_APP_HUD_VOLUME;
    if (strcmp(name, "brightness") == 0) return NEXTUI_APP_HUD_BRIGHTNESS;
    if (strcmp(name, "colortemp") == 0) return NEXTUI_APP_HUD_COLORTEMP;
    if (strcmp(name, "mute") == 0) return NEXTUI_APP_HUD_MUTE;
    return NEXTUI_APP_HUD_VOLUME;
}

static void queue_push(NextuiApp* app, const NextuiAppEvent* event)
{
    pthread_mutex_lock(&app->event_lock);
    if (app->queue_count >= NX_QUEUE_CAPACITY)
    {
        int index;
        int drop = 0;

        for (index = 0; index < app->queue_count; index++)
        {
            NextuiAppEventType type = app->queue[index].type;
            if (type == NEXTUI_APP_EVENT_STATUS_CHANGED
                || type == NEXTUI_APP_EVENT_STATUS_SNAPSHOT
                || type == NEXTUI_APP_EVENT_OVERFLOW
                || type == NEXTUI_APP_EVENT_TRANSPORT_ERROR)
            {
                drop = index;
                break;
            }
        }
        if (app->queue_count > 0)
        {
            memmove(&app->queue[drop], &app->queue[drop + 1],
                    (size_t)(app->queue_count - drop - 1) * sizeof app->queue[0]);
            app->queue_count--;
        }
    }
    app->queue[app->queue_count++] = *event;
    pthread_mutex_unlock(&app->event_lock);
}

static int deliver(NextuiApp* app, const NextuiAppEvent* event)
{
    if (app->event_callback != NULL)
    {
        /* Callbacks run on the pump thread and may issue requests. No lock is
         * held here: request_lock is only for request I/O, and event_lock is
         * only for the queue. */
        app->event_callback(app, event, app->event_user);
    }
    else
    {
        queue_push(app, event);
    }
    return 1;
}

/* Worker-side transport errors are queued for the pump thread, so callbacks
 * still run on the thread that owns poll/wait. */
static void queue_async_error(NextuiApp* app, const NxDeferred* action)
{
    NextuiAppEvent event;
    char code[NEXTUI_APP_REASON_SIZE];
    char detail[NEXTUI_APP_DETAIL_SIZE];

    pthread_mutex_lock(&app->state_lock);
    copy_string(code, sizeof code, app->last_error);
    copy_string(detail, sizeof detail, app->last_detail);
    pthread_mutex_unlock(&app->state_lock);

    memset(&event, 0, sizeof event);
    event.type = NEXTUI_APP_EVENT_TRANSPORT_ERROR;
    event.at_ms = nx_now_ms();
    copy_string(event.session_id, sizeof event.session_id, action->session_id);
    copy_string(event.data.error.code, sizeof event.data.error.code,
                code[0] != '\0' ? code : "transport");
    copy_string(event.data.error.detail, sizeof event.data.error.detail,
                detail[0] != '\0' ? detail : "deferred request failed");

    pthread_mutex_lock(&app->event_lock);
    if (app->async_error_count >= NX_ASYNC_ERROR_CAPACITY)
    {
        app->async_error_head = (app->async_error_head + 1) % NX_ASYNC_ERROR_CAPACITY;
        app->async_error_count--;
    }
    app->async_errors[(app->async_error_head + app->async_error_count) % NX_ASYNC_ERROR_CAPACITY] =
        event;
    app->async_error_count++;
    pthread_mutex_unlock(&app->event_lock);
}

static int drain_async_errors(NextuiApp* app)
{
    NextuiAppEvent events[NX_ASYNC_ERROR_CAPACITY];
    int count = 0;
    int index;
    int produced = 0;

    pthread_mutex_lock(&app->event_lock);
    while (app->async_error_count > 0 && count < NX_ASYNC_ERROR_CAPACITY)
    {
        events[count++] = app->async_errors[app->async_error_head];
        app->async_error_head = (app->async_error_head + 1) % NX_ASYNC_ERROR_CAPACITY;
        app->async_error_count--;
    }
    pthread_mutex_unlock(&app->event_lock);

    for (index = 0; index < count; index++)
    {
        (void)deliver(app, &events[index]);
        produced++;
    }
    return produced;
}

static int deferred_pending(NextuiApp* app)
{
    int pending;

    pthread_mutex_lock(&app->deferred_lock);
    pending = app->deferred_count > 0;
    pthread_mutex_unlock(&app->deferred_lock);
    if (!pending)
    {
        pthread_mutex_lock(&app->event_lock);
        pending = app->async_error_count > 0;
        pthread_mutex_unlock(&app->event_lock);
    }
    return pending;
}

static void queue_deferred(NextuiApp* app, const NxDeferred* action)
{
    pthread_mutex_lock(&app->deferred_lock);
    if (app->deferred_count >= NX_DEFERRED_CAPACITY)
    {
        app->deferred_head = (app->deferred_head + 1) % NX_DEFERRED_CAPACITY;
        app->deferred_count--;
    }
    app->deferred[(app->deferred_head + app->deferred_count) % NX_DEFERRED_CAPACITY] = *action;
    app->deferred_count++;
    pthread_cond_signal(&app->deferred_cond);
    pthread_mutex_unlock(&app->deferred_lock);
}

static void clear_deferred(NextuiApp* app)
{
    pthread_mutex_lock(&app->deferred_lock);
    app->deferred_head = 0;
    app->deferred_count = 0;
    pthread_mutex_unlock(&app->deferred_lock);

    pthread_mutex_lock(&app->event_lock);
    app->async_error_head = 0;
    app->async_error_count = 0;
    pthread_mutex_unlock(&app->event_lock);
}

/* ------------------------------------------------------------------------- */
/* Requests                                                                  */
/* ------------------------------------------------------------------------- */

static NextuiAppStatus map_server_error(const char* code, const char* detail)
{
    if (code == NULL) return NEXTUI_APP_ERR_SERVER;
    if (strcmp(code, "session_stale") == 0 || strcmp(code, "sleep_stale") == 0)
        return NEXTUI_APP_ERR_STALE;
    if (strcmp(code, "not_active") == 0) return NEXTUI_APP_ERR_NOT_ACTIVE;
    if (strcmp(code, "not_adapted") == 0) return NEXTUI_APP_ERR_NOT_ADAPTED;
    if (strcmp(code, "not_hosting") == 0) return NEXTUI_APP_ERR_NOT_HOSTING;
    if (strcmp(code, "sleep_busy") == 0) return NEXTUI_APP_ERR_BUSY;
    if (strcmp(code, "too_many_subscriptions") == 0) return NEXTUI_APP_ERR_BUSY;
    if (strcmp(code, "unknown_op") == 0 || strcmp(code, "unsupported") == 0
        || strcmp(code, "protocol_unsupported") == 0
        || strcmp(code, "unknown_capability") == 0
        || strcmp(code, "unavailable") == 0)
        return NEXTUI_APP_ERR_UNSUPPORTED;
    if (strcmp(code, "bad_request") == 0 && detail != NULL
        && strstr(detail, "unknown operation") != NULL)
        return NEXTUI_APP_ERR_UNSUPPORTED;
    return NEXTUI_APP_ERR_SERVER;
}

static NextuiAppStatus request_raw(NextuiApp* app, const char* request_line, char* response,
                                   size_t response_size)
{
    int descriptor;
    int status;
    int available;
    char line[NEXTUI_APP_LINE_MAX];
    size_t length;

    if (app == NULL || request_line == NULL || response == NULL || response_size < 2)
        return NEXTUI_APP_ERR_INVALID_ARGUMENT;
    pthread_mutex_lock(&app->state_lock);
    available = app->connected || app->handshaking;
    pthread_mutex_unlock(&app->state_lock);
    if (!available) return NEXTUI_APP_ERR_NOT_CONNECTED;
    if (strlen(request_line) + 2 > sizeof line)
    {
        set_last_error(app, "bad_request", "request line is too long");
        return NEXTUI_APP_ERR_INVALID_ARGUMENT;
    }

    length = strlen(request_line);
    memcpy(line, request_line, length);
    line[length++] = '\n';

    descriptor = nx_connect_unix(app->socket_path, app->connect_timeout_ms);
    if (descriptor < 0)
    {
        set_last_error(app, "io", "cannot connect to the devserv socket");
        return NEXTUI_APP_ERR_IO;
    }
    (void)nx_set_socket_timeout(descriptor, app->io_timeout_ms);

    if (!nx_write_all(descriptor, line, length))
    {
        close(descriptor);
        set_last_error(app, "io", "cannot send the request");
        return NEXTUI_APP_ERR_IO;
    }
    (void)shutdown(descriptor, SHUT_WR);

    status = nx_read_line(descriptor, response, response_size, app->io_timeout_ms);
    close(descriptor);
    if (status == -2)
    {
        set_last_error(app, "protocol", "response line is too long");
        return NEXTUI_APP_ERR_PROTOCOL;
    }
    if (status == -3)
    {
        set_last_error(app, "timeout", "no response from devserv");
        return NEXTUI_APP_ERR_TIMEOUT;
    }
    if (status != 1)
    {
        set_last_error(app, "io", "no response from devserv");
        return NEXTUI_APP_ERR_IO;
    }

    {
        int ok = 0;
        if (!json_bool(response, "ok", &ok))
        {
            set_last_error(app, "protocol", "response has no ok field");
            return NEXTUI_APP_ERR_PROTOCOL;
        }
        if (!ok)
        {
            char code[NEXTUI_APP_REASON_SIZE] = "";
            char detail[NEXTUI_APP_DETAIL_SIZE] = "";
            (void)json_string(response, "error", code, sizeof code);
            (void)json_string(response, "detail", detail, sizeof detail);
            set_last_error(app, code[0] != '\0' ? code : "server_error", detail);
            return map_server_error(code, detail);
        }
    }

    return NEXTUI_APP_OK;
}

NextuiAppStatus nextui_app_request(NextuiApp* app, const char* request_line, char* response,
                                   size_t response_size)
{
    NextuiAppStatus status;

    if (app == NULL) return NEXTUI_APP_ERR_INVALID_ARGUMENT;
    pthread_mutex_lock(&app->request_lock);
    status = request_raw(app, request_line, response, response_size);
    pthread_mutex_unlock(&app->request_lock);
    return status;
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

const char* nextui_app_status_string(NextuiAppStatus status)
{
    switch (status)
    {
    case NEXTUI_APP_OK: return "ok";
    case NEXTUI_APP_ERR_INVALID_ARGUMENT: return "invalid_argument";
    case NEXTUI_APP_ERR_NO_MEMORY: return "no_memory";
    case NEXTUI_APP_ERR_IO: return "io";
    case NEXTUI_APP_ERR_TIMEOUT: return "timeout";
    case NEXTUI_APP_ERR_PROTOCOL: return "protocol";
    case NEXTUI_APP_ERR_SERVER: return "server";
    case NEXTUI_APP_ERR_UNSUPPORTED: return "unsupported";
    case NEXTUI_APP_ERR_NOT_CONNECTED: return "not_connected";
    case NEXTUI_APP_ERR_STALE: return "stale";
    case NEXTUI_APP_ERR_NOT_ACTIVE: return "not_active";
    case NEXTUI_APP_ERR_NOT_ADAPTED: return "not_adapted";
    case NEXTUI_APP_ERR_BUSY: return "busy";
    case NEXTUI_APP_ERR_CLOSED: return "closed";
    case NEXTUI_APP_ERR_NO_SESSION: return "no_session";
    case NEXTUI_APP_ERR_NOT_HOSTING: return "not_hosting";
    }
    return "unknown";
}

/* Deferred request-channel actions run here; see struct NextuiApp. */
static void* worker_main(void* argument);
static void execute_deferred(NextuiApp* app, const NxDeferred* action);

NextuiApp* nextui_app_create(const NextuiAppConfig* config)
{
    NextuiApp* app = (NextuiApp*)calloc(1, sizeof(NextuiApp));
    const char* environment;
    int initialized = 0;

    if (app == NULL) return NULL;
    if (pthread_mutex_init(&app->request_lock, NULL) != 0) goto fail;
    initialized = 1;
    if (pthread_mutex_init(&app->state_lock, NULL) != 0) goto fail;
    initialized = 2;
    if (pthread_mutex_init(&app->event_lock, NULL) != 0) goto fail;
    initialized = 3;
    if (pthread_mutex_init(&app->deferred_lock, NULL) != 0) goto fail;
    initialized = 4;
    if (pthread_cond_init(&app->deferred_cond, NULL) != 0) goto fail;
    initialized = 5;
    if (pthread_create(&app->worker, NULL, worker_main, app) != 0) goto fail;
    app->worker_started = 1;

    app->event_fd = -1;
    app->host_override = -1;
    app->connect_timeout_ms = 2000;
    app->io_timeout_ms = 2000;
    app->auto_reconnect = 1;
    app->heartbeat_ms = 15000;
    info_init(&app->info);

    if (config != NULL)
    {
        if (config->connect_timeout_ms > 0) app->connect_timeout_ms = config->connect_timeout_ms;
        if (config->io_timeout_ms > 0) app->io_timeout_ms = config->io_timeout_ms;
        if (config->disable_reconnect) app->auto_reconnect = 0;
        app->log = config->log;
        app->log_user = config->log_user;
        copy_string(app->pak, sizeof app->pak, config->pak_path);
        copy_string(app->app, sizeof app->app, config->app_id);
        copy_string(app->client, sizeof app->client,
                    config->client_name != NULL ? config->client_name
                                                : "nextui-app-sdk/" NEXTUI_APP_SDK_VERSION_STRING);
    }
    if (config == NULL || config->socket_path == NULL || config->socket_path[0] == '\0')
    {
        environment = getenv("NEXTUI_DEVSERV_SOCKET");
        copy_string(app->socket_path, sizeof app->socket_path,
                    environment != NULL && environment[0] != '\0' ? environment
                                                                  : NEXTUI_APP_DEFAULT_SOCKET);
    }
    else
    {
        copy_string(app->socket_path, sizeof app->socket_path, config->socket_path);
    }
    if (config == NULL || config->pak_path == NULL || config->pak_path[0] == '\0')
    {
        environment = getenv("NEXTUI_PAK_PATH");
        copy_string(app->pak, sizeof app->pak, environment != NULL ? environment : "");
    }
    if (config == NULL || config->app_id == NULL || config->app_id[0] == '\0')
    {
        environment = getenv("NEXTUI_APP_ID");
        copy_string(app->app, sizeof app->app, environment != NULL ? environment : "");
    }
    if (app->client[0] == '\0')
        copy_string(app->client, sizeof app->client,
                    "nextui-app-sdk/" NEXTUI_APP_SDK_VERSION_STRING);

    return app;

fail:
    if (initialized >= 5) pthread_cond_destroy(&app->deferred_cond);
    if (initialized >= 4) pthread_mutex_destroy(&app->deferred_lock);
    if (initialized >= 3) pthread_mutex_destroy(&app->event_lock);
    if (initialized >= 2) pthread_mutex_destroy(&app->state_lock);
    if (initialized >= 1) pthread_mutex_destroy(&app->request_lock);
    free(app);
    return NULL;
}

static void close_event_stream(NextuiApp* app, int emit_error, const char* detail)
{
    int descriptor;

    if (app == NULL) return;
    descriptor = app->event_fd;
    app->event_fd = -1;
    app->have_ready = 0;
    if (descriptor >= 0) close(descriptor);

    if (emit_error && !app->stopped)
    {
        NextuiAppInfo info;
        NextuiAppEvent event;
        memset(&event, 0, sizeof event);
        event.type = NEXTUI_APP_EVENT_TRANSPORT_ERROR;
        event.at_ms = nx_now_ms();
        app_info_copy(app, &info);
        copy_string(event.session_id, sizeof event.session_id, info.session_id);
        copy_string(event.data.error.code, sizeof event.data.error.code, "transport");
        copy_string(event.data.error.detail, sizeof event.data.error.detail,
                    detail != NULL ? detail : "event stream closed");
        (void)deliver(app, &event);
    }
}

void nextui_app_disconnect(NextuiApp* app)
{
    if (app == NULL) return;
    close_event_stream(app, 0, NULL);
    clear_deferred(app);
    pthread_mutex_lock(&app->state_lock);
    app->connected = 0;
    pthread_mutex_unlock(&app->state_lock);
}

void nextui_app_destroy(NextuiApp* app)
{
    if (app == NULL) return;
    if (app->worker_started)
    {
        pthread_mutex_lock(&app->deferred_lock);
        app->worker_stop = 1;
        pthread_cond_signal(&app->deferred_cond);
        pthread_mutex_unlock(&app->deferred_lock);
    }
    /* Close the subscribe socket before joining: if devserv is blocked writing
     * a full event socket, closing it lets a worker request finish instead of
     * waiting for the worker's I/O timeout. */
    close_event_stream(app, 0, NULL);
    if (app->worker_started)
    {
        pthread_join(app->worker, NULL);
        app->worker_started = 0;
    }
    pthread_cond_destroy(&app->deferred_cond);
    pthread_mutex_destroy(&app->deferred_lock);
    pthread_mutex_destroy(&app->event_lock);
    pthread_mutex_destroy(&app->state_lock);
    pthread_mutex_destroy(&app->request_lock);
    free(app);
}

int nextui_app_is_connected(const NextuiApp* app)
{
    int connected;
    NextuiApp* mutable_app = (NextuiApp*)app;

    if (app == NULL) return 0;
    pthread_mutex_lock(&mutable_app->state_lock);
    connected = app->connected;
    pthread_mutex_unlock(&mutable_app->state_lock);
    return connected;
}

const NextuiAppInfo* nextui_app_info(const NextuiApp* app)
{
    return app != NULL ? &app->info : NULL;
}

const char* nextui_app_last_error(const NextuiApp* app)
{
    return app != NULL ? app->last_error : "";
}

const char* nextui_app_last_detail(const NextuiApp* app)
{
    return app != NULL ? app->last_detail : "";
}

const char* nextui_app_socket_path(const NextuiApp* app)
{
    return app != NULL ? app->socket_path : "";
}

/* Build and send hello; fill info. Called without the lock held. */
static NextuiAppStatus hello(NextuiApp* app, NextuiAppInfo* out)
{
    NxWriter writer;
    char request[NEXTUI_APP_LINE_MAX];
    char response[NEXTUI_APP_LINE_MAX];
    NextuiAppStatus status;
    NextuiAppInfo info;

    info_init(&info);
    nx_writer_init(&writer, request, sizeof request);
    nx_writer_char(&writer, '{');
    {
        int first = 1;
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "op");
        nx_writer_string(&writer, "hello");
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "client");
        nx_writer_string(&writer, app->client);
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "protocol");
        nx_writer_int(&writer, NEXTUI_APP_PROTOCOL);
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "features");
        nx_writer_raw(&writer, "[\"events\",\"sleep\",\"status\",\"hud\",\"overlay\",\"battery\"]");
        if (app->app[0] != '\0')
        {
            nx_writer_prefix(&writer, &first);
            nx_writer_key(&writer, "app");
            nx_writer_string(&writer, app->app);
        }
        if (app->pak[0] != '\0')
        {
            nx_writer_prefix(&writer, &first);
            nx_writer_key(&writer, "pak");
            nx_writer_string(&writer, app->pak);
        }
    }
    nx_writer_char(&writer, '}');
    if (!nx_writer_finish(&writer))
    {
        set_last_error(app, "invalid_argument", "hello request is too long");
        return NEXTUI_APP_ERR_INVALID_ARGUMENT;
    }

    status = request_raw(app, request, response, sizeof response);
    if (status != NEXTUI_APP_OK) return status;

    {
        int protocol = 0;
        int service_version = 0;
        int active = 0;
        int overlay_host = 0;
        int matches = 1;
        char contract[32] = "";
        unsigned features = 0;
        int value = 0;

        if (!json_int(response, "protocol", &protocol) || protocol < 1)
        {
            set_last_error(app, "protocol", "hello response has no protocol");
            return NEXTUI_APP_ERR_PROTOCOL;
        }
        info.protocol = protocol;
        (void)json_int(response, "version", &service_version);
        info.service_version = service_version;
        (void)json_string(response, "contract", contract, sizeof contract);
        copy_string(info.contract, sizeof info.contract, contract);
        (void)parse_features(response, "features", &features);
        info.features = features;

        if (json_int(response, "limits.sleep_ack_ms", &value) && value > 0)
            info.limits_sleep_ack_ms = value;
        if (json_int(response, "limits.inhibit_max_ms", &value) && value > 0)
            info.limits_inhibit_max_ms = value;
        if (json_int(response, "limits.heartbeat_ms", &value) && value > 0)
            info.limits_heartbeat_ms = value;
        if (json_int(response, "limits.status_min_interval_ms", &value) && value > 0)
            info.limits_status_min_interval_ms = value;

        if (json_bool(response, "session.active", &active) && active)
        {
            info.session_active = 1;
            (void)json_string(response, "session.session_id", info.session_id,
                              sizeof info.session_id);
            (void)json_string(response, "session.app", info.app, sizeof info.app);
            (void)json_string(response, "session.pak", info.pak, sizeof info.pak);
            (void)json_bool(response, "session.overlay_host", &overlay_host);
            info.overlay_host = overlay_host;
            info.overlay_host_declared = overlay_host;
            (void)json_bool(response, "session.overlay_host_declared",
                            &info.overlay_host_declared);
            (void)json_ull(response, "session.overlay_host_changed_at_ms",
                           &info.overlay_host_changed_at_ms);
            (void)json_ull(response, "session.started_at", &info.session_started_at);
            if (json_bool(response, "session.matches_pak", &matches)) info.session_matches = matches;
            else info.session_matches = 1;
            if (!info.session_matches)
            {
                /* Someone else's session: do not use its id. */
                info.session_active = 0;
                info.session_id[0] = '\0';
                info.overlay_host = 0;
                info.overlay_host_declared = 0;
            }
        }
        parse_battery_fields(response, "battery", &info.battery);
        (void)json_ull(response, "at_ms", &info.at_ms);
    }

    if (out != NULL) *out = info;
    return NEXTUI_APP_OK;
}

static void apply_host_override(NextuiApp* app, int defer)
{
    NextuiAppInfo info;
    int host;

    pthread_mutex_lock(&app->state_lock);
    host = app->host_override;
    pthread_mutex_unlock(&app->state_lock);
    if (host < 0) return;

    app_info_copy(app, &info);
    if (!info.session_active || !info.overlay_host_declared) return;
    if (info.overlay_host == host) return;

    if (defer)
    {
        NxDeferred action;
        memset(&action, 0, sizeof action);
        action.kind = NX_DEFERRED_HOST_OVERRIDE;
        action.host = host;
        copy_string(action.session_id, sizeof action.session_id, info.session_id);
        queue_deferred(app, &action);
        return;
    }

    {
        NextuiAppStatus status = nextui_app_overlay_set_host(app, host);
        if (status == NEXTUI_APP_ERR_UNSUPPORTED || status == NEXTUI_APP_ERR_NOT_ADAPTED)
        {
            pthread_mutex_lock(&app->state_lock);
            app->host_override = -1;
            pthread_mutex_unlock(&app->state_lock);
            log_message(app, 1, "overlay.host is not available; static routing remains");
        }
    }
}

static NextuiAppStatus open_subscription(NextuiApp* app, NextuiAppInfo* info)
{
    NxWriter writer;
    char request[NEXTUI_APP_LINE_MAX];
    char response[NEXTUI_APP_LINE_MAX];
    int descriptor;
    int families = 0;

    if ((info->features & NEXTUI_APP_FEATURE_EVENTS) == 0) return NEXTUI_APP_OK;
    if ((info->features & NEXTUI_APP_FEATURE_SLEEP) == 0
        && (info->features & NEXTUI_APP_FEATURE_STATUS) == 0
        && (info->features & NEXTUI_APP_FEATURE_HUD) == 0
        && (info->features & NEXTUI_APP_FEATURE_OVERLAY) == 0)
        return NEXTUI_APP_OK;

    nx_writer_init(&writer, request, sizeof request);
    nx_writer_char(&writer, '{');
    {
        int first = 1;
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "op");
        nx_writer_string(&writer, "subscribe");
        if (info->session_active)
        {
            nx_writer_prefix(&writer, &first);
            nx_writer_key(&writer, "session_id");
            nx_writer_string(&writer, info->session_id);
        }
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "features");
        nx_writer_char(&writer, '[');
        if (info->features & NEXTUI_APP_FEATURE_SLEEP)
        {
            nx_writer_string(&writer, "sleep");
            families++;
        }
        if (info->features & NEXTUI_APP_FEATURE_STATUS)
        {
            if (families++ > 0) nx_writer_char(&writer, ',');
            nx_writer_string(&writer, "status");
        }
        if (info->features & NEXTUI_APP_FEATURE_HUD)
        {
            if (families++ > 0) nx_writer_char(&writer, ',');
            nx_writer_string(&writer, "hud");
        }
        if (info->features & NEXTUI_APP_FEATURE_OVERLAY)
        {
            if (families++ > 0) nx_writer_char(&writer, ',');
            nx_writer_string(&writer, "overlay");
        }
        nx_writer_char(&writer, ']');

        if (info->features & NEXTUI_APP_FEATURE_STATUS)
        {
            nx_writer_prefix(&writer, &first);
            nx_writer_key(&writer, "status");
            nx_writer_raw(&writer,
                          "{\"fields\":[\"battery\",\"wifi\",\"bluetooth\",\"volume\","
                          "\"display\",\"theme\",\"clock\"],\"min_interval_ms\":");
            nx_writer_int(&writer, info->limits_status_min_interval_ms > 0
                                       ? info->limits_status_min_interval_ms
                                       : 500);
            nx_writer_char(&writer, '}');
        }
    }
    nx_writer_char(&writer, '}');
    if (!nx_writer_finish(&writer))
    {
        set_last_error(app, "invalid_argument", "subscribe request is too long");
        return NEXTUI_APP_ERR_INVALID_ARGUMENT;
    }

    descriptor = nx_connect_unix(app->socket_path, app->connect_timeout_ms);
    if (descriptor < 0)
    {
        set_last_error(app, "io", "cannot connect the event stream");
        return NEXTUI_APP_ERR_IO;
    }
    if (!nx_write_all(descriptor, request, strlen(request))
        || !nx_write_all(descriptor, "\n", 1))
    {
        close(descriptor);
        set_last_error(app, "io", "cannot send subscribe");
        return NEXTUI_APP_ERR_IO;
    }

    if (nx_read_line(descriptor, response, sizeof response, app->io_timeout_ms) != 1)
    {
        close(descriptor);
        set_last_error(app, "io", "no subscription acknowledgement");
        return NEXTUI_APP_ERR_IO;
    }

    {
        int ok = 0;
        char event[32] = "";
        if (!json_bool(response, "ok", &ok) || !ok)
        {
            char code[NEXTUI_APP_REASON_SIZE] = "";
            char detail[NEXTUI_APP_DETAIL_SIZE] = "";
            (void)json_string(response, "error", code, sizeof code);
            (void)json_string(response, "detail", detail, sizeof detail);
            close(descriptor);
            set_last_error(app, code[0] ? code : "server_error", detail);
            return map_server_error(code, detail);
        }
        (void)json_string(response, "event", event, sizeof event);
        if (strcmp(event, "subscription.ready") != 0)
        {
            close(descriptor);
            set_last_error(app, "protocol", "subscription.ready expected");
            return NEXTUI_APP_ERR_PROTOCOL;
        }
        {
            unsigned ready_features = 0;
            int heartbeat = 0;
            char session_id[NEXTUI_APP_SESSION_ID_SIZE] = "";
            int overlay_host = 0;
            int overlay_declared = 0;
            if (parse_features(response, "features", &ready_features))
                app->event_features = ready_features;
            else
                app->event_features = info->features;
            if (json_int(response, "limits.heartbeat_ms", &heartbeat) && heartbeat > 0)
                app->heartbeat_ms = heartbeat;
            if (json_string(response, "session_id", session_id, sizeof session_id)
                && session_id[0] != '\0')
            {
                copy_string(info->session_id, sizeof info->session_id, session_id);
                info->session_active = 1;
            }
            if (json_bool(response, "overlay_host", &overlay_host))
                info->overlay_host = overlay_host;
            if (json_bool(response, "overlay_host_declared", &overlay_declared))
                info->overlay_host_declared = overlay_declared;
        }
    }

    (void)nx_set_nonblocking(descriptor, 1);
    app->event_fd = descriptor;
    app->have_ready = 1;
    app->last_seq = 0;
    app->inlen = 0;
    app->last_event_at = nx_now_ms();
    app->stopped = 0;
    return NEXTUI_APP_OK;
}

NextuiAppStatus nextui_app_connect(NextuiApp* app, NextuiAppInfo* info)
{
    NextuiAppStatus status;
    NextuiAppInfo fresh;

    if (app == NULL) return NEXTUI_APP_ERR_INVALID_ARGUMENT;
    nextui_app_disconnect(app);
    info_init(&fresh);
    app->stopped = 0;
    app->next_reconnect_at = 0;

    pthread_mutex_lock(&app->event_lock);
    app->have_status = 0;
    app->queue_count = 0;
    pthread_mutex_unlock(&app->event_lock);

    pthread_mutex_lock(&app->state_lock);
    app->handshaking = 1;
    pthread_mutex_unlock(&app->state_lock);

    pthread_mutex_lock(&app->request_lock);
    status = hello(app, &fresh);
    if (status == NEXTUI_APP_OK) status = open_subscription(app, &fresh);
    pthread_mutex_unlock(&app->request_lock);

    pthread_mutex_lock(&app->state_lock);
    app->handshaking = 0;
    pthread_mutex_unlock(&app->state_lock);

    if (status == NEXTUI_APP_ERR_UNSUPPORTED && fresh.features != 0)
    {
        /* The server has events but none of the families we asked for. */
        app->event_fd = -1;
        status = NEXTUI_APP_OK;
    }
    else if (status != NEXTUI_APP_OK)
    {
        if (status == NEXTUI_APP_ERR_UNSUPPORTED)
            log_message(app, 1, "devserv does not implement the cooperation protocol");
        return status;
    }

    pthread_mutex_lock(&app->state_lock);
    app->info = fresh;
    app->connected = 1;
    app->heartbeat_ms = fresh.limits_heartbeat_ms;
    if (fresh.session_active && fresh.session_id[0] == '\0') app->info.session_active = 0;
    app->last_error[0] = '\0';
    app->last_detail[0] = '\0';
    pthread_mutex_unlock(&app->state_lock);

    /* Connect is called before the poll loop starts, so the restore may run
     * synchronously here. Reconnects defer it to the worker. */
    apply_host_override(app, 0);
    if (info != NULL) app_info_copy(app, info);
    return NEXTUI_APP_OK;
}

/* ------------------------------------------------------------------------- */
/* Event stream                                                              */
/* ------------------------------------------------------------------------- */

static void schedule_reconnect(NextuiApp* app)
{
    app->next_reconnect_at = nx_now_ms() + NX_RECONNECT_DELAY_MS;
}

static void handle_line(NextuiApp* app, char* line)
{
    char name[32] = "";
    int seq = 0;
    NextuiAppEvent event;
    unsigned long long at_ms = 0;
    char session_id[NEXTUI_APP_SESSION_ID_SIZE] = "";

    memset(&event, 0, sizeof event);
    if (!json_string(line, "event", name, sizeof name))
    {
        close_event_stream(app, 1, "malformed event line");
        schedule_reconnect(app);
        return;
    }
    (void)json_int(line, "seq", &seq);
    (void)json_ull(line, "at_ms", &at_ms);
    (void)json_string(line, "session_id", session_id, sizeof session_id);
    event.at_ms = at_ms;
    event.seq = (unsigned)seq;
    copy_string(event.session_id, sizeof event.session_id, session_id);
    app->last_event_at = nx_now_ms();

    if (app->have_ready)
    {
        if (seq != (int)app->last_seq + 1)
        {
            event.type = NEXTUI_APP_EVENT_OVERFLOW;
            event.data.overflow.dropped = 0;
            copy_string(event.data.overflow.reason, sizeof event.data.overflow.reason,
                        "sequence gap");
            (void)deliver(app, &event);
        }
        app->last_seq = (unsigned)seq;
    }
    else if (seq > 0)
    {
        app->last_seq = (unsigned)seq;
        app->have_ready = 1;
    }

    if (session_id[0] != '\0')
    {
        NextuiAppInfo info;
        app_info_copy(app, &info);
        if (info.session_active && info.session_id[0] != '\0'
            && strcmp(session_id, info.session_id) != 0)
            return; /* an event from another generation */
    }

    if (strcmp(name, "heartbeat") == 0 || strcmp(name, "subscription.ready") == 0)
    {
        return;
    }

    if (strcmp(name, "status.snapshot") == 0 || strcmp(name, "status.changed") == 0)
    {
        NextuiStatus status;
        int have_status;
        int clear = strcmp(name, "status.snapshot") == 0;
        unsigned present;

        pthread_mutex_lock(&app->event_lock);
        status = app->status;
        have_status = app->have_status;
        pthread_mutex_unlock(&app->event_lock);

        if (clear || !have_status) memset(&status, 0, sizeof status);
        present = parse_status(line, &status, clear);
        (void)present;

        pthread_mutex_lock(&app->event_lock);
        app->status = status;
        app->have_status = 1;
        pthread_mutex_unlock(&app->event_lock);

        event.type = clear ? NEXTUI_APP_EVENT_STATUS_SNAPSHOT : NEXTUI_APP_EVENT_STATUS_CHANGED;
        event.data.status = status;
        (void)deliver(app, &event);
        return;
    }

    if (strcmp(name, "feedback") == 0)
    {
        char kind[24] = "volume";
        int value = 0;
        (void)json_string(line, "kind", kind, sizeof kind);
        event.type = NEXTUI_APP_EVENT_FEEDBACK;
        event.data.feedback.kind = parse_hud_kind(kind);
        (void)json_int(line, "value", &value);
        event.data.feedback.value = value;
        (void)json_int(line, "min", &event.data.feedback.min);
        (void)json_int(line, "max", &event.data.feedback.max);
        (void)json_bool(line, "mute", &event.data.feedback.mute);
        (void)json_bool(line, "locked", &event.data.feedback.locked);
        (void)json_bool(line, "overlay_open", &event.data.feedback.overlay_open);
        (void)json_string(line, "sink", event.data.feedback.sink,
                          sizeof event.data.feedback.sink);
        (void)json_string(line, "source", event.data.feedback.source,
                          sizeof event.data.feedback.source);
        (void)deliver(app, &event);
        return;
    }

    if (strcmp(name, "sleep.prepare") == 0)
    {
        NextuiSleepPrepare prepare;
        memset(&prepare, 0, sizeof prepare);
        (void)json_string(line, "sleep_id", prepare.sleep_id, sizeof prepare.sleep_id);
        (void)json_string(line, "reason", prepare.reason, sizeof prepare.reason);
        (void)json_int(line, "deadline_ms", &prepare.deadline_ms);
        (void)json_ull(line, "deadline_at_ms", &prepare.deadline_at_ms);
        parse_battery_fields(line, "battery", &prepare.battery);
        if (prepare.sleep_id[0] == '\0')
        {
            close_event_stream(app, 1, "sleep.prepare without sleep_id");
            schedule_reconnect(app);
            return;
        }
        event.type = NEXTUI_APP_EVENT_SLEEP_PREPARE;
        event.data.sleep_prepare = prepare;

        {
            NextuiAppInfo info;
            app_info_copy(app, &info);

            if (app->sleep_handler != NULL)
            {
                NextuiAppSleepDecision decision;
                memset(&decision, 0, sizeof decision);
                app->sleep_handler(app, &prepare, &decision, app->sleep_user);
                if (decision.action == NEXTUI_APP_SLEEP_PENDING) return;

                {
                    NxDeferred action;
                    memset(&action, 0, sizeof action);
                    copy_string(action.session_id, sizeof action.session_id,
                                info.session_active ? info.session_id : "");
                    copy_string(action.sleep_id, sizeof action.sleep_id, prepare.sleep_id);
                    if (decision.action == NEXTUI_APP_SLEEP_INHIBIT)
                    {
                        action.kind = NX_DEFERRED_SLEEP_INHIBIT;
                        copy_string(action.reason, sizeof action.reason,
                                    decision.reason != NULL && decision.reason[0] != '\0'
                                        ? decision.reason
                                        : "app");
                        action.timeout_ms = decision.timeout_ms > 0 ? decision.timeout_ms : 60000;
                    }
                    else
                    {
                        action.kind = NX_DEFERRED_SLEEP_ACK;
                        action.paused = decision.paused;
                        action.saved = decision.saved;
                    }
                    copy_string(action.detail, sizeof action.detail, decision.detail);
                    /* The worker sends the ack/inhibit; the pump must never
                     * wait on request_lock while the subscribe socket has
                     * readable data. */
                    queue_deferred(app, &action);
                }

                /* The handler consumed the prepare; do not notify a second time. */
                return;
            }

            {
                NxDeferred action;
                memset(&action, 0, sizeof action);
                action.kind = NX_DEFERRED_SLEEP_ACK;
                copy_string(action.session_id, sizeof action.session_id,
                            info.session_active ? info.session_id : "");
                copy_string(action.sleep_id, sizeof action.sleep_id, prepare.sleep_id);
                copy_string(action.detail, sizeof action.detail, "auto-ack");
                queue_deferred(app, &action);
            }
        }
        (void)deliver(app, &event);
        return;
    }

    if (strcmp(name, "wake.done") == 0)
    {
        NextuiSleepDone done;
        memset(&done, 0, sizeof done);
        (void)json_string(line, "sleep_id", done.sleep_id, sizeof done.sleep_id);
        (void)json_ull(line, "slept_ms", &done.slept_ms);
        (void)json_string(line, "reason", done.reason, sizeof done.reason);
        (void)json_string(line, "woke_by", done.woke_by, sizeof done.woke_by);
        (void)json_string(line, "result", done.result, sizeof done.result);
        parse_battery_fields(line, "battery", &done.battery);
        event.type = NEXTUI_APP_EVENT_WAKE_DONE;
        event.data.wake_done = done;
        (void)deliver(app, &event);
        return;
    }

    if (strcmp(name, "overlay.home") == 0)
    {
        (void)json_string(line, "action", event.data.overlay_home.action,
                          sizeof event.data.overlay_home.action);
        event.type = NEXTUI_APP_EVENT_OVERLAY_HOME;
        (void)deliver(app, &event);
        return;
    }

    if (strcmp(name, "session.ended") == 0)
    {
        char reason[32] = "";
        (void)json_string(line, "reason", reason, sizeof reason);
        event.type = NEXTUI_APP_EVENT_SESSION_ENDED;
        copy_string(event.data.session_ended.reason, sizeof event.data.session_ended.reason,
                    reason);
        app->stopped = 1;
        pthread_mutex_lock(&app->state_lock);
        app->info.session_active = 0;
        app->info.session_id[0] = '\0';
        app->info.overlay_host = 0;
        app->info.overlay_host_declared = 0;
        pthread_mutex_unlock(&app->state_lock);
        (void)deliver(app, &event);
        close_event_stream(app, 0, NULL);
        return;
    }

    if (strcmp(name, "overflow") == 0)
    {
        event.type = NEXTUI_APP_EVENT_OVERFLOW;
        (void)json_int(line, "dropped", &event.data.overflow.dropped);
        copy_string(event.data.overflow.reason, sizeof event.data.overflow.reason, "overflow");
        (void)deliver(app, &event);
        close_event_stream(app, 0, NULL);
        schedule_reconnect(app);
        return;
    }

    /* Unknown events are ignored, as required by the versioning rules. */
}

static int reconnect_now(NextuiApp* app, int* produced)
{
    NextuiAppInfo fresh;
    NextuiAppInfo previous_info;
    NextuiAppStatus status;
    char previous[NEXTUI_APP_SESSION_ID_SIZE];
    int was_active;

    info_init(&fresh);
    app_info_copy(app, &previous_info);
    copy_string(previous, sizeof previous, previous_info.session_id);
    was_active = previous_info.session_active;

    pthread_mutex_lock(&app->state_lock);
    app->handshaking = 1;
    pthread_mutex_unlock(&app->state_lock);

    pthread_mutex_lock(&app->request_lock);
    status = hello(app, &fresh);
    if (status == NEXTUI_APP_OK) status = open_subscription(app, &fresh);
    pthread_mutex_unlock(&app->request_lock);

    pthread_mutex_lock(&app->state_lock);
    app->handshaking = 0;
    pthread_mutex_unlock(&app->state_lock);

    if (status == NEXTUI_APP_OK)
    {
        if (app->event_fd < 0) status = NEXTUI_APP_ERR_UNSUPPORTED;
        if (app->event_fd >= 0)
        {
            int changed = 0;

            pthread_mutex_lock(&app->state_lock);
            if (was_active
                && (!fresh.session_active || strcmp(previous, fresh.session_id) != 0))
                changed = 1;
            app->info = fresh;
            app->heartbeat_ms = fresh.limits_heartbeat_ms;
            pthread_mutex_unlock(&app->state_lock);

            app->last_event_at = nx_now_ms();
            app->inlen = 0;
            app->last_seq = 0;
            app->next_reconnect_at = 0;
            /* A reconnect is driven from the pump. Queue the host restore so
             * the pump keeps draining this new subscription while devserv
             * answers the request. */
            apply_host_override(app, 1);

            if (changed)
            {
                NextuiAppEvent event;
                memset(&event, 0, sizeof event);
                event.type = NEXTUI_APP_EVENT_SESSION_ENDED;
                event.at_ms = nx_now_ms();
                copy_string(event.session_id, sizeof event.session_id, previous);
                copy_string(event.data.session_ended.reason,
                            sizeof event.data.session_ended.reason, "replaced");
                if (produced != NULL) *produced += deliver(app, &event);
                app->stopped = 1;
                close_event_stream(app, 0, NULL);
            }
            return 1;
        }
    }

    schedule_reconnect(app);
    if (status == NEXTUI_APP_ERR_UNSUPPORTED) return -1;
    return 0;
}

static int pump(NextuiApp* app, int timeout_ms)
{
    int produced = 0;
    unsigned long long start = nx_now_ms();
    int remaining = timeout_ms;
    int connected;
    unsigned features;

    if (app == NULL) return NEXTUI_APP_ERR_INVALID_ARGUMENT;
    pthread_mutex_lock(&app->state_lock);
    connected = app->connected;
    features = app->info.features;
    pthread_mutex_unlock(&app->state_lock);
    if (!connected) return NEXTUI_APP_ERR_NOT_CONNECTED;
    if ((features & NEXTUI_APP_FEATURE_EVENTS) == 0)
        return NEXTUI_APP_ERR_UNSUPPORTED;

    produced += drain_async_errors(app);

    if (app->event_fd < 0)
    {
        if ((features
             & (NEXTUI_APP_FEATURE_SLEEP | NEXTUI_APP_FEATURE_STATUS
                | NEXTUI_APP_FEATURE_HUD | NEXTUI_APP_FEATURE_OVERLAY)) == 0)
            return NEXTUI_APP_ERR_UNSUPPORTED;
        if (!app->auto_reconnect || app->stopped) return 0;
        if (app->next_reconnect_at == 0) app->next_reconnect_at = nx_now_ms();

        if (nx_now_ms() < app->next_reconnect_at)
        {
            unsigned long long delta;
            int sleep_ms;

            if (timeout_ms <= 0) return 0;
            delta = app->next_reconnect_at - nx_now_ms();
            sleep_ms = delta > (unsigned long long)remaining ? remaining : (int)delta;
            if (sleep_ms > 0) (void)poll(NULL, 0, sleep_ms);
            remaining = timeout_ms - (int)(nx_now_ms() - start);
            if (remaining < 0) remaining = 0;
            if (nx_now_ms() < app->next_reconnect_at) return 0;
        }

        {
            int reconnect = reconnect_now(app, &produced);
            if (reconnect < 0) return reconnect;
            if (app->event_fd < 0) return produced;
        }
    }

    for (;;)
    {
        struct pollfd wait;
        int ready;
        int wait_ms = 0;

        if (app->event_fd < 0) return produced;
        if (timeout_ms == 0)
        {
            wait_ms = 0;
        }
        else
        {
            unsigned long long elapsed = nx_now_ms() - start;
            if (elapsed >= (unsigned long long)timeout_ms) return produced;
            wait_ms = timeout_ms - (int)elapsed;
        }

        /* Worker actions and their transport errors are delivered on the
         * pump thread, so wake up periodically while they are outstanding. */
        if (wait_ms > NX_DEFERRED_WAKE_MS && deferred_pending(app))
            wait_ms = NX_DEFERRED_WAKE_MS;

        wait.fd = app->event_fd;
        wait.events = POLLIN;
        wait.revents = 0;
        ready = poll(&wait, 1, wait_ms);
        if (ready < 0)
        {
            if (errno == EINTR) continue;
            close_event_stream(app, 1, "poll failed");
            schedule_reconnect(app);
            return produced;
        }
        if (ready == 0)
        {
            produced += drain_async_errors(app);
            if (timeout_ms == 0) return produced;
            if (nx_now_ms() - start >= (unsigned long long)timeout_ms) return produced;
            continue;
        }

        if (wait.revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            close_event_stream(app, 1, "event stream closed");
            schedule_reconnect(app);
            return produced;
        }

        {
            char buffer[4096];
            ssize_t count = recv(app->event_fd, buffer, sizeof buffer, 0);
            size_t offset = 0;

            if (count == 0)
            {
                close_event_stream(app, 1, "event stream closed");
                schedule_reconnect(app);
                return produced;
            }
            if (count < 0)
            {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
                close_event_stream(app, 1, "event stream read failed");
                schedule_reconnect(app);
                return produced;
            }

            if (app->inlen + (size_t)count >= sizeof app->inbuf)
            {
                app->inlen = 0;
                close_event_stream(app, 1, "event line too long");
                schedule_reconnect(app);
                return produced;
            }
            memcpy(app->inbuf + app->inlen, buffer, (size_t)count);
            app->inlen += (size_t)count;

            while (offset < app->inlen)
            {
                char* newline = (char*)memchr(app->inbuf + offset, '\n', app->inlen - offset);
                size_t line_length;
                if (newline == NULL) break;
                line_length = (size_t)(newline - (app->inbuf + offset));
                while (line_length > 0 && app->inbuf[offset + line_length - 1] == '\r')
                    line_length--;
                app->inbuf[offset + line_length] = '\0';
                handle_line(app, app->inbuf + offset);
                produced++;
                offset += (size_t)(newline - (app->inbuf + offset)) + 1;
            }
            if (offset > 0)
            {
                memmove(app->inbuf, app->inbuf + offset, app->inlen - offset);
                app->inlen -= offset;
            }
        }

        produced += drain_async_errors(app);
        if (app->event_fd < 0) return produced;
        if (app->heartbeat_ms > 0
            && nx_now_ms() - app->last_event_at > (unsigned long long)app->heartbeat_ms * 3)
        {
            close_event_stream(app, 1, "event stream timed out");
            schedule_reconnect(app);
            return produced;
        }
        if (timeout_ms == 0) return produced;
    }
}

int nextui_app_poll(NextuiApp* app)
{
    return pump(app, 0);
}

int nextui_app_wait(NextuiApp* app, int timeout_ms)
{
    if (timeout_ms < 0) timeout_ms = 0;
    return pump(app, timeout_ms);
}

NextuiAppStatus nextui_app_next_event(NextuiApp* app, NextuiAppEvent* out, int timeout_ms)
{
    unsigned long long start;

    if (app == NULL || out == NULL) return NEXTUI_APP_ERR_INVALID_ARGUMENT;
    if (app->event_callback != NULL) return NEXTUI_APP_ERR_INVALID_ARGUMENT;
    if (timeout_ms < 0) timeout_ms = 0;
    start = nx_now_ms();

    for (;;)
    {
        int result;
        int queued;

        pthread_mutex_lock(&app->event_lock);
        queued = app->queue_count;
        if (queued > 0)
        {
            *out = app->queue[0];
            memmove(&app->queue[0], &app->queue[1],
                    (size_t)(app->queue_count - 1) * sizeof app->queue[0]);
            app->queue_count--;
        }
        pthread_mutex_unlock(&app->event_lock);
        if (queued > 0) return NEXTUI_APP_OK;

        {
            unsigned long long elapsed = nx_now_ms() - start;
            int remaining;
            if (elapsed > (unsigned long long)timeout_ms) return NEXTUI_APP_ERR_TIMEOUT;
            remaining = timeout_ms - (int)elapsed;
            result = pump(app, remaining);
        }
        if (result < 0 && result != NEXTUI_APP_ERR_TIMEOUT) return (NextuiAppStatus)result;
        if (app->queue_count > 0) continue;
        if (timeout_ms == 0) return NEXTUI_APP_ERR_TIMEOUT;
        if (nx_now_ms() - start >= (unsigned long long)timeout_ms)
            return NEXTUI_APP_ERR_TIMEOUT;
    }
}

void nextui_app_set_event_callback(NextuiApp* app, NextuiAppEventCallback callback, void* user)
{
    if (app == NULL) return;
    app->event_callback = callback;
    app->event_user = user;
}

void nextui_app_set_sleep_handler(NextuiApp* app, NextuiAppSleepHandler handler, void* user)
{
    if (app == NULL) return;
    app->sleep_handler = handler;
    app->sleep_user = user;
}

/* ------------------------------------------------------------------------- */
/* Typed reads                                                               */
/* ------------------------------------------------------------------------- */

NextuiAppStatus nextui_app_battery_get(NextuiApp* app, NextuiBattery* out)
{
    char response[NEXTUI_APP_LINE_MAX];
    NextuiAppStatus status;

    if (app == NULL || out == NULL) return NEXTUI_APP_ERR_INVALID_ARGUMENT;
    status = nextui_app_request(app, "{\"op\":\"battery.status\"}", response, sizeof response);
    if (status != NEXTUI_APP_OK) return status;
    memset(out, 0, sizeof *out);
    {
        NxJsonValue battery;
        if (!nx_json_find(response, "battery", &battery))
        {
            set_last_error(app, "protocol", "battery.status response has no battery object");
            return NEXTUI_APP_ERR_PROTOCOL;
        }
    }
    parse_battery_fields(response, "battery", out);
    if (out->percent < 0 || out->percent > 100)
    {
        set_last_error(app, "backend_error", "battery percent is out of range");
        memset(out, 0, sizeof *out);
        return NEXTUI_APP_ERR_SERVER;
    }
    return NEXTUI_APP_OK;
}

static void write_fields_array(NxWriter* writer, unsigned sections)
{
    static const struct
    {
        unsigned bit;
        const char* name;
    } names[] = {
        {NEXTUI_APP_SECTION_BATTERY, "battery"},
        {NEXTUI_APP_SECTION_WIFI, "wifi"},
        {NEXTUI_APP_SECTION_BLUETOOTH, "bluetooth"},
        {NEXTUI_APP_SECTION_VOLUME, "volume"},
        {NEXTUI_APP_SECTION_DISPLAY, "display"},
        {NEXTUI_APP_SECTION_THEME, "theme"},
        {NEXTUI_APP_SECTION_CLOCK, "clock"},
    };
    int first = 1;
    size_t i;

    nx_writer_char(writer, '[');
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
    {
        if ((sections & names[i].bit) == 0) continue;
        if (!first) nx_writer_char(writer, ',');
        nx_writer_string(writer, names[i].name);
        first = 0;
    }
    nx_writer_char(writer, ']');
}

NextuiAppStatus nextui_app_status_get(NextuiApp* app, unsigned sections, NextuiStatus* out)
{
    NxWriter writer;
    char request[NEXTUI_APP_LINE_MAX];
    char response[NEXTUI_APP_LINE_MAX];
    NextuiAppStatus status;

    if (app == NULL || out == NULL) return NEXTUI_APP_ERR_INVALID_ARGUMENT;
    if (sections == 0) sections = NEXTUI_APP_SECTION_ALL;

    nx_writer_init(&writer, request, sizeof request);
    nx_writer_char(&writer, '{');
    {
        int first = 1;
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "op");
        nx_writer_string(&writer, "status.get");
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "fields");
        write_fields_array(&writer, sections);
    }
    nx_writer_char(&writer, '}');
    if (!nx_writer_finish(&writer)) return NEXTUI_APP_ERR_INVALID_ARGUMENT;

    status = nextui_app_request(app, request, response, sizeof response);
    if (status != NEXTUI_APP_OK) return status;

    memset(out, 0, sizeof *out);
    (void)parse_status(response, out, 1);
    return NEXTUI_APP_OK;
}

NextuiAppStatus nextui_app_sleep_status(NextuiApp* app, NextuiSleepStatus* out)
{
    char response[NEXTUI_APP_LINE_MAX];
    NextuiAppStatus status;

    if (app == NULL || out == NULL) return NEXTUI_APP_ERR_INVALID_ARGUMENT;
    status = nextui_app_request(app, "{\"op\":\"sleep.status\"}", response, sizeof response);
    if (status != NEXTUI_APP_OK) return status;

    memset(out, 0, sizeof *out);
    (void)json_string(response, "phase", out->phase, sizeof out->phase);
    (void)json_ull(response, "at_ms", &out->at_ms);
    {
        NxJsonValue pending;
        if (nx_json_find(response, "pending", &pending) && !nx_json_is_null(pending))
        {
            out->pending = 1;
            (void)json_string(response, "pending.sleep_id", out->pending_sleep_id,
                              sizeof out->pending_sleep_id);
            (void)json_string(response, "pending.reason", out->pending_reason,
                              sizeof out->pending_reason);
            (void)json_int(response, "pending.deadline_ms", &out->pending_deadline_ms);
            (void)json_ull(response, "pending.deadline_at_ms", &out->pending_deadline_at_ms);
            (void)json_int(response, "pending.subscribers", &out->pending_subscribers);
            (void)json_int(response, "pending.acks", &out->pending_acks);
        }
    }
    {
        NxJsonValue inhibit;
        if (nx_json_find(response, "inhibit", &inhibit) && !nx_json_is_null(inhibit))
        {
            unsigned long long expires_at = 0;
            out->inhibit_active = 1;
            (void)json_string(response, "inhibit.scope", out->inhibit_scope,
                              sizeof out->inhibit_scope);
            (void)json_string(response, "inhibit.reason", out->inhibit_reason,
                              sizeof out->inhibit_reason);
            if (json_ull(response, "inhibit.expires_at_ms", &expires_at)
                && expires_at > out->at_ms)
                out->inhibit_expires_in_ms = (int)(expires_at - out->at_ms);
        }
    }
    {
        NxJsonValue last;
        if (nx_json_find(response, "last", &last) && !nx_json_is_null(last))
        {
            out->last_valid = 1;
            (void)json_string(response, "last.sleep_id", out->last_sleep_id,
                              sizeof out->last_sleep_id);
            (void)json_string(response, "last.reason", out->last_reason,
                              sizeof out->last_reason);
            (void)json_string(response, "last.result", out->last_result,
                              sizeof out->last_result);
            (void)json_string(response, "last.woke_by", out->last_woke_by,
                              sizeof out->last_woke_by);
            (void)json_ull(response, "last.slept_ms", &out->last_slept_ms);
        }
    }
    return NEXTUI_APP_OK;
}

/* ------------------------------------------------------------------------- */
/* Sleep handshake                                                           */
/* ------------------------------------------------------------------------- */

NextuiAppStatus nextui_app_sleep_ack(NextuiApp* app, const char* sleep_id, int paused, int saved,
                                     const char* detail)
{
    NxWriter writer;
    NextuiAppInfo info;
    char request[NEXTUI_APP_LINE_MAX];
    char response[NEXTUI_APP_LINE_MAX];

    if (app == NULL || sleep_id == NULL || sleep_id[0] == '\0')
        return NEXTUI_APP_ERR_INVALID_ARGUMENT;
    app_info_copy(app, &info);

    nx_writer_init(&writer, request, sizeof request);
    nx_writer_char(&writer, '{');
    {
        int first = 1;
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "op");
        nx_writer_string(&writer, "sleep.ack");
        if (info.session_active)
        {
            nx_writer_prefix(&writer, &first);
            nx_writer_key(&writer, "session_id");
            nx_writer_string(&writer, info.session_id);
        }
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "sleep_id");
        nx_writer_string(&writer, sleep_id);
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "paused");
        nx_writer_bool(&writer, paused);
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "saved");
        nx_writer_bool(&writer, saved);
        if (detail != NULL && detail[0] != '\0')
        {
            nx_writer_prefix(&writer, &first);
            nx_writer_key(&writer, "detail");
            nx_writer_string(&writer, detail);
        }
    }
    nx_writer_char(&writer, '}');
    if (!nx_writer_finish(&writer)) return NEXTUI_APP_ERR_INVALID_ARGUMENT;

    return nextui_app_request(app, request, response, sizeof response);
}

NextuiAppStatus nextui_app_sleep_inhibit(NextuiApp* app, const char* reason, int timeout_ms)
{
    NextuiAppInfo info;

    if (app == NULL || reason == NULL || reason[0] == '\0')
        return NEXTUI_APP_ERR_INVALID_ARGUMENT;
    app_info_copy(app, &info);
    if (!info.session_active) return NEXTUI_APP_ERR_NOT_ACTIVE;
    return nextui_app_sleep_inhibit_for(app, NULL, reason, timeout_ms, NULL);
}

NextuiAppStatus nextui_app_sleep_inhibit_for(NextuiApp* app, const char* sleep_id,
                                             const char* reason, int timeout_ms,
                                             const char* detail)
{
    NxWriter writer;
    NextuiAppInfo info;
    char request[NEXTUI_APP_LINE_MAX];
    char response[NEXTUI_APP_LINE_MAX];
    NextuiAppStatus status;

    if (app == NULL || reason == NULL || reason[0] == '\0') return NEXTUI_APP_ERR_INVALID_ARGUMENT;
    if (timeout_ms < 1000) return NEXTUI_APP_ERR_INVALID_ARGUMENT;
    app_info_copy(app, &info);
    if (!info.session_active) return NEXTUI_APP_ERR_NOT_ACTIVE;

    nx_writer_init(&writer, request, sizeof request);
    nx_writer_char(&writer, '{');
    {
        int first = 1;
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "op");
        nx_writer_string(&writer, "sleep.inhibit");
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "session_id");
        nx_writer_string(&writer, info.session_id);
        if (sleep_id != NULL && sleep_id[0] != '\0')
        {
            nx_writer_prefix(&writer, &first);
            nx_writer_key(&writer, "sleep_id");
            nx_writer_string(&writer, sleep_id);
        }
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "reason");
        nx_writer_string(&writer, reason);
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "timeout_ms");
        nx_writer_int(&writer, timeout_ms);
        if (detail != NULL && detail[0] != '\0')
        {
            nx_writer_prefix(&writer, &first);
            nx_writer_key(&writer, "detail");
            nx_writer_string(&writer, detail);
        }
    }
    nx_writer_char(&writer, '}');
    if (!nx_writer_finish(&writer)) return NEXTUI_APP_ERR_INVALID_ARGUMENT;

    status = nextui_app_request(app, request, response, sizeof response);
    return status;
}

NextuiAppStatus nextui_app_sleep_release(NextuiApp* app)
{
    return nextui_app_sleep_release_for(app, NULL);
}

NextuiAppStatus nextui_app_sleep_release_for(NextuiApp* app, const char* sleep_id)
{
    NxWriter writer;
    NextuiAppInfo info;
    char request[NEXTUI_APP_LINE_MAX];
    char response[NEXTUI_APP_LINE_MAX];
    NextuiAppStatus status;

    if (app == NULL) return NEXTUI_APP_ERR_INVALID_ARGUMENT;
    app_info_copy(app, &info);
    if (!info.session_active) return NEXTUI_APP_ERR_NOT_ACTIVE;

    nx_writer_init(&writer, request, sizeof request);
    nx_writer_char(&writer, '{');
    {
        int first = 1;
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "op");
        nx_writer_string(&writer, "sleep.release");
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "session_id");
        nx_writer_string(&writer, info.session_id);
        if (sleep_id != NULL && sleep_id[0] != '\0')
        {
            nx_writer_prefix(&writer, &first);
            nx_writer_key(&writer, "sleep_id");
            nx_writer_string(&writer, sleep_id);
        }
    }
    nx_writer_char(&writer, '}');
    if (!nx_writer_finish(&writer)) return NEXTUI_APP_ERR_INVALID_ARGUMENT;

    status = nextui_app_request(app, request, response, sizeof response);
    return status;
}

/* ------------------------------------------------------------------------- */
/* HUD and overlay                                                           */
/* ------------------------------------------------------------------------- */

NextuiAppStatus nextui_app_hud_get_policy(NextuiApp* app, unsigned* kinds_app)
{
    NextuiAppInfo info;
    char response[NEXTUI_APP_LINE_MAX];
    NextuiAppStatus status;
    unsigned bits = 0;

    if (app == NULL || kinds_app == NULL) return NEXTUI_APP_ERR_INVALID_ARGUMENT;
    app_info_copy(app, &info);
    if (!info.session_active) return NEXTUI_APP_ERR_NOT_ACTIVE;

    {
        NxWriter writer;
        char request[NEXTUI_APP_LINE_MAX];
        nx_writer_init(&writer, request, sizeof request);
        nx_writer_raw(&writer, "{\"op\":\"hud.policy\",\"session_id\":");
        nx_writer_string(&writer, info.session_id);
        nx_writer_char(&writer, '}');
        if (!nx_writer_finish(&writer)) return NEXTUI_APP_ERR_INVALID_ARGUMENT;
        status = nextui_app_request(app, request, response, sizeof response);
    }
    if (status != NEXTUI_APP_OK) return status;

    {
        static const struct
        {
            const char* name;
            unsigned bit;
        } kinds[] = {
            {"volume", NEXTUI_APP_HUD_VOLUME},
            {"brightness", NEXTUI_APP_HUD_BRIGHTNESS},
            {"colortemp", NEXTUI_APP_HUD_COLORTEMP},
            {"mute", NEXTUI_APP_HUD_MUTE},
        };
        size_t index;
        char mode[16];
        char path[64];

        for (index = 0; index < sizeof kinds / sizeof kinds[0]; index++)
        {
            int app = 0;
            snprintf(path, sizeof path, "effective.%s", kinds[index].name);
            if (json_string(response, path, mode, sizeof mode))
                app = strcmp(mode, "app") == 0;
            else
            {
                snprintf(path, sizeof path, "policy.%s", kinds[index].name);
                if (json_string(response, path, mode, sizeof mode))
                    app = strcmp(mode, "app") == 0;
            }
            if (app) bits |= kinds[index].bit;
        }
    }
    *kinds_app = bits;
    return NEXTUI_APP_OK;
}

NextuiAppStatus nextui_app_hud_set_policy(NextuiApp* app, unsigned kinds, NextuiAppHudMode mode)
{
    static const struct
    {
        unsigned bit;
        const char* name;
    } names[] = {
        {NEXTUI_APP_HUD_VOLUME, "volume"},
        {NEXTUI_APP_HUD_BRIGHTNESS, "brightness"},
        {NEXTUI_APP_HUD_COLORTEMP, "colortemp"},
        {NEXTUI_APP_HUD_MUTE, "mute"},
    };
    NxWriter writer;
    NextuiAppInfo info;
    char request[NEXTUI_APP_LINE_MAX];
    char response[NEXTUI_APP_LINE_MAX];
    NextuiAppStatus status;
    int first = 1;
    int first_kind = 1;
    size_t i;

    if (app == NULL) return NEXTUI_APP_ERR_INVALID_ARGUMENT;
    app_info_copy(app, &info);
    if (!info.session_active) return NEXTUI_APP_ERR_NOT_ACTIVE;
    if (kinds == 0) kinds = NEXTUI_APP_HUD_ALL;
    if (kinds & ~(unsigned)NEXTUI_APP_HUD_ALL) return NEXTUI_APP_ERR_INVALID_ARGUMENT;

    nx_writer_init(&writer, request, sizeof request);
    nx_writer_char(&writer, '{');
    nx_writer_prefix(&writer, &first);
    nx_writer_key(&writer, "op");
    nx_writer_string(&writer, "hud.policy");
    nx_writer_prefix(&writer, &first);
    nx_writer_key(&writer, "session_id");
    nx_writer_string(&writer, info.session_id);
    nx_writer_prefix(&writer, &first);
    nx_writer_key(&writer, "mode");
    nx_writer_string(&writer, mode == NEXTUI_APP_HUD_APP ? "app" : "system");
    nx_writer_prefix(&writer, &first);
    nx_writer_key(&writer, "kinds");
    nx_writer_char(&writer, '[');
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
    {
        if ((kinds & names[i].bit) == 0) continue;
        if (!first_kind) nx_writer_char(&writer, ',');
        nx_writer_string(&writer, names[i].name);
        first_kind = 0;
    }
    nx_writer_char(&writer, ']');
    nx_writer_char(&writer, '}');
    if (!nx_writer_finish(&writer)) return NEXTUI_APP_ERR_INVALID_ARGUMENT;

    status = nextui_app_request(app, request, response, sizeof response);
    return status;
}

int nextui_app_overlay_host(const NextuiApp* app)
{
    int value;
    NextuiApp* mutable_app = (NextuiApp*)app;

    if (app == NULL) return 0;
    pthread_mutex_lock(&mutable_app->state_lock);
    value = app->info.overlay_host;
    pthread_mutex_unlock(&mutable_app->state_lock);
    return value;
}

int nextui_app_overlay_host_declared(const NextuiApp* app)
{
    int value;
    NextuiApp* mutable_app = (NextuiApp*)app;

    if (app == NULL) return 0;
    pthread_mutex_lock(&mutable_app->state_lock);
    value = app->info.overlay_host_declared;
    pthread_mutex_unlock(&mutable_app->state_lock);
    return value;
}

NextuiAppStatus nextui_app_overlay_set_host(NextuiApp* app, int host)
{
    NxWriter writer;
    NextuiAppInfo info;
    char request[NEXTUI_APP_LINE_MAX];
    char response[NEXTUI_APP_LINE_MAX];
    NextuiAppStatus status;
    int value = 0;

    if (app == NULL) return NEXTUI_APP_ERR_INVALID_ARGUMENT;
    app_info_copy(app, &info);
    if (!info.session_active) return NEXTUI_APP_ERR_NOT_ACTIVE;
    if (host && !info.overlay_host_declared) return NEXTUI_APP_ERR_NOT_ADAPTED;

    nx_writer_init(&writer, request, sizeof request);
    nx_writer_char(&writer, '{');
    {
        int first = 1;
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "op");
        nx_writer_string(&writer, "overlay.host");
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "session_id");
        nx_writer_string(&writer, info.session_id);
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "host");
        nx_writer_bool(&writer, host != 0);
    }
    nx_writer_char(&writer, '}');
    if (!nx_writer_finish(&writer)) return NEXTUI_APP_ERR_INVALID_ARGUMENT;

    status = nextui_app_request(app, request, response, sizeof response);
    if (status != NEXTUI_APP_OK) return status;

    pthread_mutex_lock(&app->state_lock);
    if (json_bool(response, "overlay_host", &value)) app->info.overlay_host = value;
    if (json_bool(response, "overlay_host_declared", &value))
        app->info.overlay_host_declared = value;
    (void)json_ull(response, "host_changed_at_ms", &app->info.overlay_host_changed_at_ms);
    app->host_override = host ? 1 : 0;
    pthread_mutex_unlock(&app->state_lock);
    return NEXTUI_APP_OK;
}

NextuiAppStatus nextui_app_overlay_set_open(NextuiApp* app, int open)
{
    NxWriter writer;
    NextuiAppInfo info;
    char request[NEXTUI_APP_LINE_MAX];
    char response[NEXTUI_APP_LINE_MAX];
    NextuiAppStatus status;

    if (app == NULL) return NEXTUI_APP_ERR_INVALID_ARGUMENT;
    app_info_copy(app, &info);
    if (!info.session_active) return NEXTUI_APP_ERR_NOT_ACTIVE;
    if (!info.overlay_host)
        return info.overlay_host_declared ? NEXTUI_APP_ERR_NOT_HOSTING
                                          : NEXTUI_APP_ERR_NOT_ADAPTED;

    nx_writer_init(&writer, request, sizeof request);
    nx_writer_char(&writer, '{');
    {
        int first = 1;
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "op");
        nx_writer_string(&writer, "overlay.state");
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "session_id");
        nx_writer_string(&writer, info.session_id);
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "open");
        nx_writer_bool(&writer, open != 0);
    }
    nx_writer_char(&writer, '}');
    if (!nx_writer_finish(&writer)) return NEXTUI_APP_ERR_INVALID_ARGUMENT;

    status = nextui_app_request(app, request, response, sizeof response);
    return status;
}

NextuiAppStatus nextui_app_call(NextuiApp* app, const char* capability, const char* action,
                                const char* args, char* value_out, size_t value_size)
{
    NxWriter writer;
    char request[NEXTUI_APP_LINE_MAX];
    char response[NEXTUI_APP_LINE_MAX];
    NextuiAppStatus status;

    if (app == NULL || capability == NULL || capability[0] == '\0')
        return NEXTUI_APP_ERR_INVALID_ARGUMENT;
    if (value_out != NULL && value_size > 0) value_out[0] = '\0';
    if (app->pak[0] == '\0') return NEXTUI_APP_ERR_INVALID_ARGUMENT;

    nx_writer_init(&writer, request, sizeof request);
    nx_writer_char(&writer, '{');
    {
        int first = 1;
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "op");
        nx_writer_string(&writer, "call");
        if (app->app[0] != '\0')
        {
            nx_writer_prefix(&writer, &first);
            nx_writer_key(&writer, "app");
            nx_writer_string(&writer, app->app);
        }
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "pak");
        nx_writer_string(&writer, app->pak);
        nx_writer_prefix(&writer, &first);
        nx_writer_key(&writer, "capability");
        nx_writer_string(&writer, capability);
        if (action != NULL && action[0] != '\0')
        {
            nx_writer_prefix(&writer, &first);
            nx_writer_key(&writer, "action");
            nx_writer_string(&writer, action);
        }
        if (args != NULL && args[0] != '\0')
        {
            nx_writer_prefix(&writer, &first);
            nx_writer_key(&writer, "args");
            nx_writer_raw(&writer, args);
        }
    }
    nx_writer_char(&writer, '}');
    if (!nx_writer_finish(&writer)) return NEXTUI_APP_ERR_INVALID_ARGUMENT;

    status = nextui_app_request(app, request, response, sizeof response);
    if (status != NEXTUI_APP_OK) return status;

    if (value_out != NULL && value_size > 0)
    {
        NxJsonValue value;
        if (nx_json_find(response, "value", &value))
        {
            size_t length = (size_t)(value.end - value.begin);
            if (length >= value_size) return NEXTUI_APP_ERR_PROTOCOL;
            memcpy(value_out, value.begin, length);
            value_out[length] = '\0';
        }
        else
        {
            copy_string(value_out, value_size, "null");
        }
    }
    return NEXTUI_APP_OK;
}

/* ------------------------------------------------------------------------- */
/* Misc                                                                      */
/* ------------------------------------------------------------------------- */

int nextui_app_json_escape(const char* input, char* out, size_t out_size)
{
    return nx_json_escape(input, out, out_size);
}

/* ------------------------------------------------------------------------- */
/* Deferred request worker                                                   */
/* ------------------------------------------------------------------------- */

/*
 * The event pump never performs a synchronous request. sleep.prepare replies,
 * sleep.inhibit and the post-reconnect overlay restore are queued here and
 * executed on this thread, so a request that blocks on devserv (which may
 * itself be blocked writing a full subscribe socket) cannot stop the pump from
 * draining that socket.
 */
static void execute_deferred(NextuiApp* app, const NxDeferred* action)
{
    NextuiAppStatus status;

    switch (action->kind)
    {
    case NX_DEFERRED_SLEEP_ACK:
        status = nextui_app_sleep_ack(app, action->sleep_id, action->paused, action->saved,
                                      action->detail);
        if (status != NEXTUI_APP_OK) queue_async_error(app, action);
        return;

    case NX_DEFERRED_SLEEP_INHIBIT:
        status = nextui_app_sleep_inhibit_for(app, action->sleep_id, action->reason,
                                              action->timeout_ms, action->detail);
        if (status != NEXTUI_APP_OK) queue_async_error(app, action);
        return;

    case NX_DEFERRED_HOST_OVERRIDE:
        status = nextui_app_overlay_set_host(app, action->host);
        if (status == NEXTUI_APP_ERR_UNSUPPORTED || status == NEXTUI_APP_ERR_NOT_ADAPTED)
        {
            pthread_mutex_lock(&app->state_lock);
            app->host_override = -1;
            pthread_mutex_unlock(&app->state_lock);
            log_message(app, 1, "overlay.host is not available; static routing remains");
        }
        return;
    }
}

static void* worker_main(void* argument)
{
    NextuiApp* app = (NextuiApp*)argument;

    for (;;)
    {
        NxDeferred action;

        pthread_mutex_lock(&app->deferred_lock);
        while (app->deferred_count == 0 && !app->worker_stop)
            pthread_cond_wait(&app->deferred_cond, &app->deferred_lock);
        if (app->worker_stop)
        {
            pthread_mutex_unlock(&app->deferred_lock);
            break;
        }
        action = app->deferred[app->deferred_head];
        app->deferred_head = (app->deferred_head + 1) % NX_DEFERRED_CAPACITY;
        app->deferred_count--;
        pthread_mutex_unlock(&app->deferred_lock);

        execute_deferred(app, &action);
    }
    return NULL;
}
