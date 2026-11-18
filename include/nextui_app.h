/*
 * nextui_app.h — framework-agnostic client for the NextUI app ↔ system
 * cooperation protocol (docs/CONTRACT.md, protocol 1).
 *
 * Pure C99, libc + POSIX only. No UI framework, no SDL, no msettings.
 *
 * The library speaks newline-delimited JSON over the devserv Unix socket:
 *   - short-lived request connections (one line in, one line out), and
 *   - one persistent event connection opened with `subscribe`.
 *
 * Typical use:
 *
 *   NextuiAppConfig config = {0};
 *   config.client_name = "myapp/1.0";
 *   NextuiApp* app = nextui_app_create(&config);
 *   NextuiAppInfo info;
 *   if (nextui_app_connect(app, &info) != NEXTUI_APP_OK) {
 *       // run without cooperation
 *   }
 *   nextui_app_set_sleep_handler(app, on_sleep, NULL);
 *   for (;;) {
 *       nextui_app_wait(app, 100);       // dispatch events / callbacks
 *       if (quit) break;
 *   }
 *   nextui_app_destroy(app);
 *
 * Thread-safety:
 *   - Request functions (battery/status/sleep/hud/overlay) may be called from
 *     any thread; they are serialized internally by a mutex.
 *   - nextui_app_poll / nextui_app_wait / nextui_app_next_event must be called
 *     from exactly one thread at a time, and not concurrently with each other.
 *   - Callbacks (event and sleep) run on the thread that calls poll/wait/
 *     next_event. They may call request functions, but must not call
 *     poll/wait/next_event recursively.
 *   - nextui_app_destroy must not race with any other call; the caller owns
 *     shutdown ordering.
 *
 * Event data lifetime:
 *   Event structs are owned by the caller (next_event copies into it) or are
 *   valid only for the duration of the callback. No pointer inside an event
 *   points into library memory.
 */

#ifndef NEXTUI_APP_H
#define NEXTUI_APP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NEXTUI_APP_SDK_VERSION_MAJOR 1
#define NEXTUI_APP_SDK_VERSION_MINOR 0
#define NEXTUI_APP_SDK_VERSION_PATCH 0
#define NEXTUI_APP_SDK_VERSION_STRING "1.0.0"

#define NEXTUI_APP_PROTOCOL 1
#define NEXTUI_APP_CONTRACT "nextui-app/1"

#define NEXTUI_APP_SESSION_ID_SIZE 17
#define NEXTUI_APP_SLEEP_ID_SIZE 17
#define NEXTUI_APP_SSID_SIZE 33
#define NEXTUI_APP_BT_NAME_SIZE 64
#define NEXTUI_APP_SINK_SIZE 16
#define NEXTUI_APP_STRENGTH_SIZE 16
#define NEXTUI_APP_REASON_SIZE 24
#define NEXTUI_APP_PHASE_SIZE 16
#define NEXTUI_APP_DETAIL_SIZE 160
#define NEXTUI_APP_APP_SIZE 128
#define NEXTUI_APP_PAK_SIZE 512
#define NEXTUI_APP_SOCKET_SIZE 108
#define NEXTUI_APP_CLIENT_SIZE 64
#define NEXTUI_APP_LINE_MAX 8192
#define NEXTUI_APP_DEFAULT_SOCKET "/tmp/nextui-devserv.sock"

/* Feature bits reported by hello/ping and stored in NextuiAppInfo. */
typedef enum
{
    NEXTUI_APP_FEATURE_EVENTS = 1u << 0,
    NEXTUI_APP_FEATURE_SLEEP = 1u << 1,
    NEXTUI_APP_FEATURE_STATUS = 1u << 2,
    NEXTUI_APP_FEATURE_HUD = 1u << 3,
    NEXTUI_APP_FEATURE_OVERLAY = 1u << 4,
    NEXTUI_APP_FEATURE_BATTERY = 1u << 5
} NextuiAppFeature;

/* HUD kinds and policy modes (contract §7.5). */
typedef enum
{
    NEXTUI_APP_HUD_VOLUME = 1u << 0,
    NEXTUI_APP_HUD_BRIGHTNESS = 1u << 1,
    NEXTUI_APP_HUD_COLORTEMP = 1u << 2,
    NEXTUI_APP_HUD_MUTE = 1u << 3,
    NEXTUI_APP_HUD_ALL = 0xFu
} NextuiAppHudKind;

typedef enum
{
    NEXTUI_APP_HUD_SYSTEM = 0,
    NEXTUI_APP_HUD_APP = 1
} NextuiAppHudMode;

/* Status sections (contract §8). */
typedef enum
{
    NEXTUI_APP_SECTION_BATTERY = 1u << 0,
    NEXTUI_APP_SECTION_WIFI = 1u << 1,
    NEXTUI_APP_SECTION_BLUETOOTH = 1u << 2,
    NEXTUI_APP_SECTION_VOLUME = 1u << 3,
    NEXTUI_APP_SECTION_DISPLAY = 1u << 4,
    NEXTUI_APP_SECTION_THEME = 1u << 5,
    NEXTUI_APP_SECTION_CLOCK = 1u << 6,
    NEXTUI_APP_SECTION_ALL = 0x7Fu
} NextuiAppSection;

/*
 * Return codes. Server-side failures keep the stable server error string in
 * nextui_app_last_error()/nextui_app_last_detail(); NEXTUI_APP_ERR_SERVER is
 * the generic case, and stale/unsupported/not-active/not-adapted have their
 * own code so callers can branch without parsing strings.
 */
typedef enum
{
    NEXTUI_APP_OK = 0,
    NEXTUI_APP_ERR_INVALID_ARGUMENT = -1,
    NEXTUI_APP_ERR_NO_MEMORY = -2,
    NEXTUI_APP_ERR_IO = -3,
    NEXTUI_APP_ERR_TIMEOUT = -4,
    NEXTUI_APP_ERR_PROTOCOL = -5,
    NEXTUI_APP_ERR_SERVER = -6,
    NEXTUI_APP_ERR_UNSUPPORTED = -7,
    NEXTUI_APP_ERR_NOT_CONNECTED = -8,
    NEXTUI_APP_ERR_STALE = -9,
    NEXTUI_APP_ERR_NOT_ACTIVE = -10,
    NEXTUI_APP_ERR_NOT_ADAPTED = -11,
    NEXTUI_APP_ERR_BUSY = -12,
    NEXTUI_APP_ERR_CLOSED = -13,
    NEXTUI_APP_ERR_NO_SESSION = -14,
    NEXTUI_APP_ERR_NOT_HOSTING = -15
} NextuiAppStatus;

const char* nextui_app_status_string(NextuiAppStatus status);

typedef struct
{
    int available;
    int percent;  /* 0..100 when available */
    int charging; /* platform is_charging */
    int usb;      /* USB data (UDC enumerated), not "charger present" */
} NextuiBattery;

typedef struct
{
    int available;
    int enabled;
    int connected;
    char strength[NEXTUI_APP_STRENGTH_SIZE]; /* off|disconnected|low|med|high */
    char ssid[NEXTUI_APP_SSID_SIZE];
} NextuiWifi;

typedef struct
{
    int available;
    int enabled;
    int connected;
    char name[NEXTUI_APP_BT_NAME_SIZE];
} NextuiBluetooth;

typedef struct
{
    int available;
    int level;
    int min;
    int max;
    int mute;
    int mute_supported;
    int locked;
    char sink[NEXTUI_APP_SINK_SIZE];
} NextuiVolume;

typedef struct
{
    int available;
    int brightness;
    int brightness_min;
    int brightness_max;
    int brightness_locked;
    int colortemp;
    int colortemp_min;
    int colortemp_max;
    int colortemp_locked;
} NextuiDisplay;

typedef struct
{
    int available;
    uint32_t main;
    uint32_t accent;
    uint32_t accent2;
    uint32_t list_text;
    uint32_t list_text_selected;
    uint32_t hint;
    uint32_t background;
} NextuiTheme;

typedef struct
{
    int show;
    int hour24;
} NextuiClock;

/*
 * A status snapshot or patch. `sections` says which sections were present in
 * the message that produced this value. For a patch, sections that are absent
 * are left untouched by the library; callers should keep the previous struct
 * and apply only the bits in `sections`. `nextui_app_status_get()` always
 * returns a full snapshot.
 */
typedef struct
{
    unsigned sections;
    int overlay_open;
    NextuiBattery battery;
    NextuiWifi wifi;
    NextuiBluetooth bluetooth;
    NextuiVolume volume;
    NextuiDisplay display;
    NextuiTheme theme;
    NextuiClock clock;
} NextuiStatus;

typedef struct
{
    char phase[NEXTUI_APP_PHASE_SIZE]; /* awake|preparing|sleeping */
    int pending;
    char pending_sleep_id[NEXTUI_APP_SLEEP_ID_SIZE];
    char pending_reason[NEXTUI_APP_REASON_SIZE];
    int pending_deadline_ms;
    unsigned long long pending_deadline_at_ms;
    int pending_subscribers;
    int pending_acks;
    int inhibit_active;
    char inhibit_scope[NEXTUI_APP_PHASE_SIZE]; /* prepare|automatic */
    char inhibit_reason[NEXTUI_APP_REASON_SIZE + 8];
    int inhibit_expires_in_ms;
    int last_valid;
    char last_sleep_id[NEXTUI_APP_SLEEP_ID_SIZE];
    char last_reason[NEXTUI_APP_REASON_SIZE];
    char last_result[NEXTUI_APP_PHASE_SIZE];
    char last_woke_by[NEXTUI_APP_PHASE_SIZE];
    unsigned long long last_slept_ms;
    unsigned long long at_ms;
} NextuiSleepStatus;

typedef struct
{
    char sleep_id[NEXTUI_APP_SLEEP_ID_SIZE];
    char reason[NEXTUI_APP_REASON_SIZE];
    int deadline_ms;
    unsigned long long deadline_at_ms;
    NextuiBattery battery;
} NextuiSleepPrepare;

typedef struct
{
    char sleep_id[NEXTUI_APP_SLEEP_ID_SIZE];
    unsigned long long slept_ms;
    char reason[NEXTUI_APP_REASON_SIZE];
    char woke_by[NEXTUI_APP_PHASE_SIZE];
    char result[NEXTUI_APP_PHASE_SIZE];
    NextuiBattery battery;
} NextuiSleepDone;

typedef struct
{
    NextuiAppHudKind kind;
    int value;
    int min;
    int max;
    int mute;
    int locked;
    int overlay_open;
    char sink[NEXTUI_APP_SINK_SIZE];
    char source[NEXTUI_APP_PHASE_SIZE]; /* key|change */
} NextuiFeedback;

typedef enum
{
    NEXTUI_APP_EVENT_NONE = 0,
    NEXTUI_APP_EVENT_SLEEP_PREPARE,
    NEXTUI_APP_EVENT_WAKE_DONE,
    NEXTUI_APP_EVENT_STATUS_SNAPSHOT,
    NEXTUI_APP_EVENT_STATUS_CHANGED,
    NEXTUI_APP_EVENT_FEEDBACK,
    NEXTUI_APP_EVENT_OVERLAY_HOME,
    NEXTUI_APP_EVENT_SESSION_ENDED,
    NEXTUI_APP_EVENT_OVERFLOW,
    NEXTUI_APP_EVENT_TRANSPORT_ERROR
} NextuiAppEventType;

typedef struct
{
    NextuiAppEventType type;
    unsigned seq;
    unsigned long long at_ms;
    char session_id[NEXTUI_APP_SESSION_ID_SIZE];
    union
    {
        NextuiSleepPrepare sleep_prepare;
        NextuiSleepDone wake_done;
        NextuiStatus status; /* snapshot or patch */
        NextuiFeedback feedback;
        struct
        {
            char action[NEXTUI_APP_PHASE_SIZE];
        } overlay_home;
        struct
        {
            char reason[NEXTUI_APP_PHASE_SIZE];
            int dropped;
        } overflow;
        struct
        {
            char reason[NEXTUI_APP_PHASE_SIZE]; /* stopped|replaced|server_shutdown */
        } session_ended;
        struct
        {
            char code[NEXTUI_APP_REASON_SIZE];
            char detail[NEXTUI_APP_DETAIL_SIZE];
        } error;
    } data;
} NextuiAppEvent;

/* What the sleep handler tells the library to do with a sleep.prepare. */
typedef enum
{
    /* Library acks immediately with paused=0, saved=0. */
    NEXTUI_APP_SLEEP_DEFAULT = 0,
    /* Handler paused/saved synchronously; library acks with the given flags. */
    NEXTUI_APP_SLEEP_ACK,
    /* Library sends sleep.inhibit with decision.reason/timeout_ms. */
    NEXTUI_APP_SLEEP_INHIBIT,
    /* Handler will call nextui_app_sleep_ack() later (before the deadline). */
    NEXTUI_APP_SLEEP_PENDING
} NextuiAppSleepAction;

typedef struct
{
    NextuiAppSleepAction action;
    const char* reason; /* INHIBIT; NULL means "app" */
    int timeout_ms;     /* INHIBIT; 0 means 60000 */
    int paused;         /* ACK; informational */
    int saved;          /* ACK; informational */
    const char* detail; /* optional, diagnostic */
} NextuiAppSleepDecision;

typedef struct NextuiApp NextuiApp;

typedef struct
{
    const char* socket_path; /* NULL: $NEXTUI_DEVSERV_SOCKET or the default */
    const char* pak_path;    /* NULL: $NEXTUI_PAK_PATH */
    const char* app_id;      /* NULL: $NEXTUI_APP_ID */
    const char* client_name; /* optional, diagnostics only */
    int connect_timeout_ms;  /* default 2000 */
    int io_timeout_ms;       /* default 2000 */
    int disable_reconnect;   /* 0: reconnect the event stream automatically */
    void (*log)(void* user, int level, const char* message);
    void* log_user;
} NextuiAppConfig;

typedef struct
{
    int protocol;
    int service_version;
    char contract[32];
    unsigned features; /* NextuiAppFeature bits */
    int limits_sleep_ack_ms;
    int limits_inhibit_max_ms;
    int limits_heartbeat_ms;
    int limits_status_min_interval_ms;
    int session_active;
    int session_matches;
    char session_id[NEXTUI_APP_SESSION_ID_SIZE];
    char app[NEXTUI_APP_APP_SIZE];
    char pak[NEXTUI_APP_PAK_SIZE];
    int overlay_host;          /* effective host for this session */
    int overlay_host_declared; /* manifest permission */
    unsigned long long overlay_host_changed_at_ms;
    unsigned long long session_started_at;
    NextuiBattery battery;
    unsigned long long at_ms;
} NextuiAppInfo;

typedef void (*NextuiAppEventCallback)(NextuiApp* app, const NextuiAppEvent* event, void* user);

typedef void (*NextuiAppSleepHandler)(NextuiApp* app, const NextuiSleepPrepare* prepare,
                                      NextuiAppSleepDecision* decision, void* user);

/* Creation and connection. */
NextuiApp* nextui_app_create(const NextuiAppConfig* config);
void nextui_app_destroy(NextuiApp* app);

/*
 * Connects the event channel, sends hello, subscribes to the negotiated
 * families, and fills `info` when non-NULL. Returns NEXTUI_APP_ERR_UNSUPPORTED
 * when the server predates protocol 1; the app should then run without the
 * SDK. Safe to call again after a disconnect.
 */
NextuiAppStatus nextui_app_connect(NextuiApp* app, NextuiAppInfo* info);
void nextui_app_disconnect(NextuiApp* app);
int nextui_app_is_connected(const NextuiApp* app);

/*
 * The pointer returned by nextui_app_info() is owned by the library and is
 * valid until the next nextui_app_connect()/reconnect. It is not synchronized
 * against a poll thread that is reconnecting; use it from the thread that owns
 * polling, or copy what you need after connect.
 */
const NextuiAppInfo* nextui_app_info(const NextuiApp* app);
/* Updated by request calls; read them on the thread that made the failing
 * call. The strings are owned by the library. */
const char* nextui_app_last_error(const NextuiApp* app);        /* stable code or "" */
const char* nextui_app_last_detail(const NextuiApp* app);       /* diagnostic or "" */
const char* nextui_app_socket_path(const NextuiApp* app);

/* Reads. */
NextuiAppStatus nextui_app_battery_get(NextuiApp* app, NextuiBattery* out);
NextuiAppStatus nextui_app_status_get(NextuiApp* app, unsigned sections, NextuiStatus* out);
NextuiAppStatus nextui_app_sleep_status(NextuiApp* app, NextuiSleepStatus* out);

/* Sleep handshake. All of these require a connected event channel. */
NextuiAppStatus nextui_app_sleep_ack(NextuiApp* app, const char* sleep_id, int paused, int saved,
                                     const char* detail);
NextuiAppStatus nextui_app_sleep_inhibit(NextuiApp* app, const char* reason, int timeout_ms);
NextuiAppStatus nextui_app_sleep_inhibit_for(NextuiApp* app, const char* sleep_id,
                                             const char* reason, int timeout_ms,
                                             const char* detail);
NextuiAppStatus nextui_app_sleep_release(NextuiApp* app);
NextuiAppStatus nextui_app_sleep_release_for(NextuiApp* app, const char* sleep_id);

/* HUD ownership (contract §7.5). hud_get_policy returns the *effective* mask
 * for this session (the dynamic overlay host and overlay_open are applied),
 * not just the stored policy. */
NextuiAppStatus nextui_app_hud_get_policy(NextuiApp* app, unsigned* kinds_app);
NextuiAppStatus nextui_app_hud_set_policy(NextuiApp* app, unsigned kinds, NextuiAppHudMode mode);

/* Overlay hosting helpers (contract §7, including dynamic ownership §7.9).
 * nextui_app_overlay_host() is the effective value; declared is the static
 * pak.json permission. Nextui_app_overlay_set_host() yields/restores hosting
 * within that permission and is re-applied after an automatic reconnect. */
int nextui_app_overlay_host(const NextuiApp* app);
int nextui_app_overlay_host_declared(const NextuiApp* app);
NextuiAppStatus nextui_app_overlay_set_host(NextuiApp* app, int host);
NextuiAppStatus nextui_app_overlay_set_open(NextuiApp* app, int open);

/*
 * Generic capability call, for app-specific C1 use (e.g. net.wifi once a
 * backend exists). `args` is a JSON value string or NULL. `value_out` receives
 * the raw `value` JSON (may be "null"); returns NEXTUI_APP_ERR_UNSUPPORTED when
 * the capability is unknown or unavailable.
 */
NextuiAppStatus nextui_app_call(NextuiApp* app, const char* capability, const char* action,
                                const char* args, char* value_out, size_t value_size);

/* Event delivery. */
void nextui_app_set_event_callback(NextuiApp* app, NextuiAppEventCallback callback, void* user);

/*
 * The sleep handler consumes sleep.prepare: when one is set, the event is
 * delivered to the handler (inside poll/wait/next_event) and not to the event
 * callback or queue. The handler must return quickly; use PENDING and call
 * nextui_app_sleep_ack() later if a save cannot finish inside the callback.
 * With no handler the library acks immediately with paused=0 and then delivers
 * the event for observation.
 */
void nextui_app_set_sleep_handler(NextuiApp* app, NextuiAppSleepHandler handler, void* user);

/*
 * Non-blocking pump: reads available event lines, handles reconnects, invokes
 * callbacks. Returns the number of events handled (>= 0) or a negative
 * NextuiAppStatus when there is no usable event channel (NOT_CONNECTED before
 * connect, UNSUPPORTED when the server has no events feature).
 */
int nextui_app_poll(NextuiApp* app);

/* poll() with a bounded wait (0 = non-blocking). Returns events dispatched. */
int nextui_app_wait(NextuiApp* app, int timeout_ms);

/*
 * Dequeue one event, waiting up to timeout_ms. Returns NEXTUI_APP_OK and fills
 * `out`, NEXTUI_APP_ERR_TIMEOUT when none arrived, or another status. Not
 * usable together with an event callback (returns INVALID_ARGUMENT).
 */
NextuiAppStatus nextui_app_next_event(NextuiApp* app, NextuiAppEvent* out, int timeout_ms);

/*
 * For tests and apps that already have a socket: the raw request helper used
 * by every typed call. Sends one line and returns the raw response line.
 * Most callers should use the typed functions.
 */
NextuiAppStatus nextui_app_request(NextuiApp* app, const char* request_line, char* response,
                                   size_t response_size);

/* JSON escape helper (used for request builders; exposed for tests). */
int nextui_app_json_escape(const char* input, char* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* NEXTUI_APP_H */
