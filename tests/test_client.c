/* Host tests for the nextui_app client, driven against tests/fake_devserv. */
#define _POSIX_C_SOURCE 200809L

#include "nextui_app.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
static pid_t fake_pid = -1;
static char fake_root[256];
static char fake_socket[300];
static char fake_log[300];

#define CHECK(condition)                                                                     \
    do                                                                                       \
    {                                                                                        \
        if (!(condition))                                                                    \
        {                                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);             \
            failures++;                                                                      \
        }                                                                                    \
    } while (0)

static void sleep_ms(int milliseconds)
{
    struct timespec request;
    request.tv_sec = milliseconds / 1000;
    request.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    nanosleep(&request, NULL);
}

static int connect_socket(const char* path)
{
    struct sockaddr_un address;
    int descriptor;

    if (strlen(path) >= sizeof address.sun_path) return -1;
    descriptor = socket(AF_UNIX, SOCK_STREAM, 0);
    if (descriptor < 0) return -1;
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);
    if (connect(descriptor, (struct sockaddr*)&address, sizeof address) != 0)
    {
        close(descriptor);
        return -1;
    }
    return descriptor;
}

static int fake_start(const char* scenario)
{
    const char* executable = getenv("NEXTUI_FAKE_DEVSERV");
    const char* tmp = getenv("TMPDIR");
    char pattern[256];
    int attempt;

    if (executable == NULL || executable[0] == '\0')
    {
        fprintf(stderr, "NEXTUI_FAKE_DEVSERV is not set\n");
        return 0;
    }
    if (tmp == NULL || tmp[0] == '\0') tmp = "/tmp";
    snprintf(pattern, sizeof pattern, "%s/nextui-sdk-XXXXXX", tmp);
    if (mkdtemp(pattern) == NULL)
    {
        perror("mkdtemp");
        return 0;
    }
    snprintf(fake_root, sizeof fake_root, "%s", pattern);
    snprintf(fake_socket, sizeof fake_socket, "%s/devserv.sock", fake_root);
    snprintf(fake_log, sizeof fake_log, "%s/requests.log", fake_root);

    fake_pid = fork();
    if (fake_pid == 0)
    {
        execl(executable, executable, "--socket", fake_socket, "--scenario", scenario, "--log",
              fake_log, (char*)NULL);
        _exit(127);
    }
    if (fake_pid < 0)
    {
        perror("fork");
        return 0;
    }

    for (attempt = 0; attempt < 300; attempt++)
    {
        int descriptor = connect_socket(fake_socket);
        if (descriptor >= 0)
        {
            close(descriptor);
            return 1;
        }
        sleep_ms(10);
    }
    fprintf(stderr, "fake devserv did not start\n");
    return 0;
}

static void fake_stop(void)
{
    if (fake_pid > 0)
    {
        kill(fake_pid, SIGTERM);
        waitpid(fake_pid, NULL, 0);
        fake_pid = -1;
    }
    if (fake_socket[0] != '\0') unlink(fake_socket);
    if (fake_log[0] != '\0') unlink(fake_log);
    if (fake_root[0] != '\0') rmdir(fake_root);
    fake_socket[0] = '\0';
    fake_log[0] = '\0';
    fake_root[0] = '\0';
}

static int control(const char* request, char* response, size_t response_size)
{
    int descriptor = connect_socket(fake_socket);
    size_t used = 0;

    if (descriptor < 0) return 0;
    if (write(descriptor, request, strlen(request)) < 0 || write(descriptor, "\n", 1) < 0)
    {
        close(descriptor);
        return 0;
    }
    while (used + 1 < response_size)
    {
        ssize_t count = read(descriptor, response + used, response_size - used - 1);
        if (count <= 0) break;
        used += (size_t)count;
        if (memchr(response, '\n', used) != NULL) break;
    }
    response[used] = '\0';
    close(descriptor);
    return used > 0;
}

static int log_contains(const char* needle)
{
    FILE* file = fopen(fake_log, "rb");
    char buffer[131072];
    size_t length;
    int found = 0;

    if (file == NULL) return 0;
    length = fread(buffer, 1, sizeof buffer - 1, file);
    buffer[length] = '\0';
    fclose(file);
    found = strstr(buffer, needle) != NULL;
    return found;
}

static NextuiApp* app_connect(NextuiAppInfo* info)
{
    NextuiAppConfig config;
    NextuiApp* app;
    NextuiAppStatus status;

    memset(&config, 0, sizeof config);
    config.socket_path = fake_socket;
    config.client_name = "test/1.0";
    config.pak_path = "/tmp/Demo.pak";
    config.app_id = "com.example.demo";
    app = nextui_app_create(&config);
    if (app == NULL)
    {
        fprintf(stderr, "nextui_app_create failed\n");
        return NULL;
    }
    status = nextui_app_connect(app, info);
    if (status != NEXTUI_APP_OK)
    {
        fprintf(stderr, "connect failed: %s (%s: %s)\n", nextui_app_status_string(status),
                nextui_app_last_error(app), nextui_app_last_detail(app));
        nextui_app_destroy(app);
        return NULL;
    }
    return app;
}

static int next_of_type(NextuiApp* app, NextuiAppEventType type, int timeout_ms,
                        NextuiAppEvent* out)
{
    for (;;)
    {
        NextuiAppStatus status = nextui_app_next_event(app, out, timeout_ms);
        if (status == NEXTUI_APP_ERR_TIMEOUT) return 0;
        if (status != NEXTUI_APP_OK) return 0;
        if (out->type == type) return 1;
        timeout_ms = 100;
    }
}

/* ------------------------------------------------------------------------- */

static void test_connect_and_reads(void)
{
    NextuiAppInfo info;
    NextuiApp* app;
    NextuiBattery battery;
    NextuiStatus status;
    unsigned kinds = 0;

    if (!fake_start("adapted")) return;
    app = app_connect(&info);
    if (app == NULL)
    {
        fake_stop();
        return;
    }

    CHECK(info.protocol == 1);
    CHECK(strcmp(info.contract, "nextui-app/1") == 0);
    CHECK(info.session_active == 1);
    CHECK(strcmp(info.session_id, "0123456789abcdef") == 0);
    CHECK(info.overlay_host == 1);
    CHECK((info.features & NEXTUI_APP_FEATURE_SLEEP) != 0);
    CHECK((info.features & NEXTUI_APP_FEATURE_STATUS) != 0);
    CHECK(nextui_app_overlay_host(app) == 1);
    CHECK(nextui_app_is_connected(app) == 1);

    CHECK(nextui_app_battery_get(app, &battery) == NEXTUI_APP_OK);
    CHECK(battery.available == 1);
    CHECK(battery.percent == 87);
    CHECK(battery.charging == 1);
    CHECK(battery.usb == 0);

    CHECK(nextui_app_status_get(app, NEXTUI_APP_SECTION_ALL, &status) == NEXTUI_APP_OK);
    CHECK((status.sections & NEXTUI_APP_SECTION_BATTERY) != 0);
    CHECK(status.battery.percent == 87);
    CHECK((status.sections & NEXTUI_APP_SECTION_WIFI) != 0);
    CHECK(status.wifi.connected == 1);
    CHECK(strcmp(status.wifi.ssid, "TestNet") == 0);
    CHECK(strcmp(status.wifi.strength, "high") == 0);
    CHECK((status.sections & NEXTUI_APP_SECTION_VOLUME) != 0);
    CHECK(status.volume.level == 8);
    CHECK(status.volume.max == 20);
    CHECK((status.sections & NEXTUI_APP_SECTION_DISPLAY) != 0);
    CHECK(status.display.brightness == 5);
    CHECK(status.display.colortemp == 20);
    CHECK((status.sections & NEXTUI_APP_SECTION_THEME) != 0);
    CHECK(status.theme.accent == 0x2257FF);
    CHECK(status.clock.show == 1 && status.clock.hour24 == 1);

    CHECK(nextui_app_hud_get_policy(app, &kinds) == NEXTUI_APP_OK);
    CHECK(kinds == NEXTUI_APP_HUD_ALL);
    CHECK(nextui_app_overlay_set_open(app, 1) == NEXTUI_APP_OK);
    CHECK(nextui_app_overlay_set_open(app, 0) == NEXTUI_APP_OK);

    nextui_app_destroy(app);
    fake_stop();
}

static void test_events(void)
{
    NextuiAppInfo info;
    NextuiApp* app;
    NextuiAppEvent event;
    char response[256];

    if (!fake_start("adapted")) return;
    app = app_connect(&info);
    if (app == NULL)
    {
        fake_stop();
        return;
    }

    CHECK(next_of_type(app, NEXTUI_APP_EVENT_STATUS_SNAPSHOT, 1000, &event) == 1);
    CHECK((event.data.status.sections & NEXTUI_APP_SECTION_BATTERY) != 0);
    CHECK(event.data.status.battery.percent == 87);

    CHECK(control("{\"op\":\"test.status\",\"percent\":86,\"level\":7}", response,
                  sizeof response) == 1);
    CHECK(next_of_type(app, NEXTUI_APP_EVENT_STATUS_CHANGED, 1000, &event) == 1);
    CHECK((event.data.status.sections & NEXTUI_APP_SECTION_BATTERY) != 0);
    CHECK((event.data.status.sections & NEXTUI_APP_SECTION_VOLUME) != 0);
    CHECK(event.data.status.battery.percent == 86);
    CHECK(event.data.status.volume.level == 7);

    CHECK(control("{\"op\":\"test.feedback\",\"kind\":\"volume\",\"value\":7}", response,
                  sizeof response) == 1);
    CHECK(next_of_type(app, NEXTUI_APP_EVENT_FEEDBACK, 1000, &event) == 1);
    CHECK(event.data.feedback.kind == NEXTUI_APP_HUD_VOLUME);
    CHECK(event.data.feedback.value == 7);
    CHECK(strcmp(event.data.feedback.source, "key") == 0);

    nextui_app_destroy(app);
    fake_stop();
}

static int sleep_calls;
static char sleep_seen[NEXTUI_APP_SLEEP_ID_SIZE];
static int sleep_action;
static int sleep_paused;

static void sleep_handler(NextuiApp* app, const NextuiSleepPrepare* prepare,
                          NextuiAppSleepDecision* decision, void* user)
{
    (void)app;
    (void)user;
    sleep_calls++;
    snprintf(sleep_seen, sizeof sleep_seen, "%s", prepare->sleep_id);
    decision->action = (NextuiAppSleepAction)sleep_action;
    decision->paused = sleep_paused;
    decision->saved = 1;
    if (decision->action == NEXTUI_APP_SLEEP_INHIBIT)
    {
        decision->reason = "playing_video";
        decision->timeout_ms = 60000;
    }
}

static void test_sleep_ack_and_wake(void)
{
    NextuiAppInfo info;
    NextuiApp* app;
    NextuiAppEvent event;
    char response[256];

    if (!fake_start("adapted")) return;
    app = app_connect(&info);
    if (app == NULL)
    {
        fake_stop();
        return;
    }
    sleep_calls = 0;
    sleep_action = NEXTUI_APP_SLEEP_ACK;
    sleep_paused = 1;
    nextui_app_set_sleep_handler(app, sleep_handler, NULL);

    CHECK(control("{\"op\":\"test.sleep_prepare\",\"sleep_id\":\"aaaabbbbccccdddd\","
                  "\"reason\":\"user\",\"deadline_ms\":1500}",
                  response, sizeof response) == 1);
    CHECK(nextui_app_wait(app, 1000) >= 0);
    CHECK(sleep_calls == 1);
    CHECK(strcmp(sleep_seen, "aaaabbbbccccdddd") == 0);
    CHECK(log_contains("\"op\":\"sleep.ack\""));
    CHECK(log_contains("\"sleep_id\":\"aaaabbbbccccdddd\""));
    CHECK(log_contains("\"paused\":true"));

    CHECK(control("{\"op\":\"test.wake\",\"sleep_id\":\"aaaabbbbccccdddd\",\"slept_ms\":61000}",
                  response, sizeof response) == 1);
    CHECK(next_of_type(app, NEXTUI_APP_EVENT_WAKE_DONE, 1000, &event) == 1);
    CHECK(strcmp(event.data.wake_done.sleep_id, "aaaabbbbccccdddd") == 0);
    CHECK(event.data.wake_done.slept_ms == 61000);
    CHECK(strcmp(event.data.wake_done.woke_by, "power") == 0);

    nextui_app_destroy(app);
    fake_stop();
}

static void test_sleep_inhibit(void)
{
    NextuiAppInfo info;
    NextuiApp* app;
    char response[256];

    if (!fake_start("adapted")) return;
    app = app_connect(&info);
    if (app == NULL)
    {
        fake_stop();
        return;
    }
    sleep_calls = 0;
    sleep_action = NEXTUI_APP_SLEEP_INHIBIT;
    sleep_paused = 1;
    nextui_app_set_sleep_handler(app, sleep_handler, NULL);

    CHECK(control("{\"op\":\"test.sleep_prepare\",\"sleep_id\":\"1111222233334444\","
                  "\"reason\":\"idle\",\"deadline_ms\":1500}",
                  response, sizeof response) == 1);
    CHECK(nextui_app_wait(app, 1000) >= 0);
    CHECK(sleep_calls == 1);
    CHECK(log_contains("\"op\":\"sleep.inhibit\""));
    CHECK(log_contains("\"reason\":\"playing_video\""));
    CHECK(log_contains("\"timeout_ms\":60000"));

    nextui_app_destroy(app);
    fake_stop();
}

static void test_stale_sleep(void)
{
    NextuiAppInfo info;
    NextuiApp* app;
    NextuiAppEvent event;
    char response[256];

    if (!fake_start("stale_sleep")) return;
    app = app_connect(&info);
    if (app == NULL)
    {
        fake_stop();
        return;
    }
    sleep_calls = 0;
    sleep_action = NEXTUI_APP_SLEEP_ACK;
    sleep_paused = 1;
    nextui_app_set_sleep_handler(app, sleep_handler, NULL);

    CHECK(control("{\"op\":\"test.sleep_prepare\",\"sleep_id\":\"9999888877776666\"}",
                  response, sizeof response) == 1);
    CHECK(next_of_type(app, NEXTUI_APP_EVENT_TRANSPORT_ERROR, 1000, &event) == 1);
    CHECK(strcmp(event.data.error.code, "sleep_stale") == 0);

    nextui_app_destroy(app);
    fake_stop();
}

static void test_unadapted(void)
{
    NextuiAppInfo info;
    NextuiApp* app;
    unsigned kinds = 0xFFFFu;

    if (!fake_start("unadapted")) return;
    app = app_connect(&info);
    if (app == NULL)
    {
        fake_stop();
        return;
    }
    CHECK(info.overlay_host == 0);
    CHECK(nextui_app_overlay_host(app) == 0);
    CHECK(nextui_app_overlay_set_open(app, 1) == NEXTUI_APP_ERR_NOT_ADAPTED);
    CHECK(nextui_app_overlay_set_host(app, 1) == NEXTUI_APP_ERR_NOT_ADAPTED);
    CHECK(nextui_app_hud_get_policy(app, &kinds) == NEXTUI_APP_OK);
    CHECK(kinds == 0);
    /* An app that does not host cannot draw the HUD: effective stays system
     * even if it stores an app policy. */
    CHECK(nextui_app_hud_set_policy(app, NEXTUI_APP_HUD_VOLUME, NEXTUI_APP_HUD_APP)
          == NEXTUI_APP_OK);
    CHECK(nextui_app_hud_get_policy(app, &kinds) == NEXTUI_APP_OK);
    CHECK(kinds == 0);

    nextui_app_destroy(app);
    fake_stop();
}

static void test_no_session(void)
{
    NextuiAppInfo info;
    NextuiApp* app;
    unsigned kinds = 0;

    if (!fake_start("no_session")) return;
    app = app_connect(&info);
    if (app == NULL)
    {
        fake_stop();
        return;
    }
    CHECK(info.session_active == 0);
    CHECK(nextui_app_overlay_set_open(app, 1) == NEXTUI_APP_ERR_NOT_ACTIVE);
    CHECK(nextui_app_hud_get_policy(app, &kinds) == NEXTUI_APP_ERR_NOT_ACTIVE);
    CHECK(nextui_app_sleep_inhibit(app, "playing_video", 60000) == NEXTUI_APP_ERR_NOT_ACTIVE);

    nextui_app_destroy(app);
    fake_stop();
}

static void test_legacy_server(void)
{
    NextuiAppConfig config;
    NextuiApp* app;
    NextuiAppStatus status;

    if (!fake_start("legacy")) return;
    memset(&config, 0, sizeof config);
    config.socket_path = fake_socket;
    app = nextui_app_create(&config);
    CHECK(app != NULL);
    if (app == NULL)
    {
        fake_stop();
        return;
    }
    status = nextui_app_connect(app, NULL);
    CHECK(status == NEXTUI_APP_ERR_UNSUPPORTED);
    CHECK(strcmp(nextui_app_last_error(app), "bad_request") == 0);
    CHECK(strstr(nextui_app_last_detail(app), "unknown operation") != NULL);
    nextui_app_destroy(app);
    fake_stop();
}

static void test_garbage_server(void)
{
    NextuiAppConfig config;
    NextuiApp* app;
    NextuiAppStatus status;

    if (!fake_start("garbage")) return;
    memset(&config, 0, sizeof config);
    config.socket_path = fake_socket;
    app = nextui_app_create(&config);
    CHECK(app != NULL);
    if (app == NULL)
    {
        fake_stop();
        return;
    }
    status = nextui_app_connect(app, NULL);
    CHECK(status == NEXTUI_APP_ERR_PROTOCOL);
    nextui_app_destroy(app);
    fake_stop();
}

static void test_desktop_battery(void)
{
    NextuiAppInfo info;
    NextuiApp* app;
    NextuiBattery battery;

    if (!fake_start("desktop")) return;
    app = app_connect(&info);
    if (app == NULL)
    {
        fake_stop();
        return;
    }
    CHECK(nextui_app_battery_get(app, &battery) == NEXTUI_APP_OK);
    CHECK(battery.available == 0);
    CHECK(battery.percent == 0);
    nextui_app_destroy(app);
    fake_stop();
}

static void test_call(void)
{
    NextuiAppInfo info;
    NextuiApp* app;
    char value[128];

    if (!fake_start("adapted")) return;
    app = app_connect(&info);
    if (app == NULL)
    {
        fake_stop();
        return;
    }
    CHECK(nextui_app_call(app, "display.get", "get", NULL, value, sizeof value)
          == NEXTUI_APP_OK);
    CHECK(strstr(value, "\"level\":5") != NULL);
    nextui_app_destroy(app);
    fake_stop();

    if (!fake_start("unavailable_call")) return;
    app = app_connect(&info);
    if (app == NULL)
    {
        fake_stop();
        return;
    }
    CHECK(nextui_app_call(app, "net.wifi", "get", NULL, value, sizeof value)
          == NEXTUI_APP_ERR_UNSUPPORTED);
    CHECK(strcmp(nextui_app_last_error(app), "unavailable") == 0);
    nextui_app_destroy(app);
    fake_stop();
}

static void test_session_ended(void)
{
    NextuiAppInfo info;
    NextuiApp* app;
    NextuiAppEvent event;
    char response[256];

    if (!fake_start("adapted")) return;
    app = app_connect(&info);
    if (app == NULL)
    {
        fake_stop();
        return;
    }
    CHECK(control("{\"op\":\"test.session_ended\",\"reason\":\"stopped\"}", response,
                  sizeof response) == 1);
    CHECK(next_of_type(app, NEXTUI_APP_EVENT_SESSION_ENDED, 1000, &event) == 1);
    CHECK(strcmp(event.data.session_ended.reason, "stopped") == 0);
    CHECK(nextui_app_info(app)->session_active == 0);
    CHECK(nextui_app_overlay_set_open(app, 1) == NEXTUI_APP_ERR_NOT_ACTIVE);
    nextui_app_destroy(app);
    fake_stop();
}

static int callback_count;

static void event_callback(NextuiApp* app, const NextuiAppEvent* event, void* user)
{
    (void)app;
    (void)event;
    (void)user;
    callback_count++;
}

static void test_callback_and_timeout(void)
{
    NextuiAppInfo info;
    NextuiApp* app;
    NextuiAppEvent event;

    if (!fake_start("adapted")) return;
    app = app_connect(&info);
    if (app == NULL)
    {
        fake_stop();
        return;
    }
    callback_count = 0;
    nextui_app_set_event_callback(app, event_callback, NULL);
    (void)nextui_app_wait(app, 1000);
    CHECK(callback_count >= 1);
    CHECK(nextui_app_next_event(app, &event, 100) == NEXTUI_APP_ERR_INVALID_ARGUMENT);
    nextui_app_set_event_callback(app, NULL, NULL);
    CHECK(nextui_app_next_event(app, &event, 100) == NEXTUI_APP_ERR_TIMEOUT);
    nextui_app_destroy(app);
    fake_stop();
}

static void test_overflow(void)
{
    NextuiAppInfo info;
    NextuiApp* app;
    NextuiAppEvent event;
    char response[256];

    if (!fake_start("adapted")) return;
    app = app_connect(&info);
    if (app == NULL)
    {
        fake_stop();
        return;
    }
    CHECK(control("{\"op\":\"test.overflow\"}", response, sizeof response) == 1);
    CHECK(next_of_type(app, NEXTUI_APP_EVENT_OVERFLOW, 1000, &event) == 1);
    CHECK(event.data.overflow.dropped == 3);
    nextui_app_destroy(app);
    fake_stop();
}

static void test_reconnect(void)
{
    NextuiAppInfo info;
    NextuiApp* app;
    NextuiAppEvent event;
    char response[256];

    if (!fake_start("adapted")) return;
    app = app_connect(&info);
    if (app == NULL)
    {
        fake_stop();
        return;
    }
    /* The effective host is session state; the fake keeps it across a plain
     * event reconnect. Reset it to simulate a fresh session and verify that
     * the SDK re-applies its last overlay_set_host() after hello. */
    CHECK(nextui_app_overlay_set_host(app, 0) == NEXTUI_APP_OK);
    CHECK(control("{\"op\":\"test.reset_host\"}", response, sizeof response) == 1);
    CHECK(control("{\"op\":\"test.close_events\"}", response, sizeof response) == 1);
    CHECK(next_of_type(app, NEXTUI_APP_EVENT_TRANSPORT_ERROR, 1000, &event) == 1);
    CHECK(next_of_type(app, NEXTUI_APP_EVENT_STATUS_SNAPSHOT, 4000, &event) == 1);
    CHECK(event.data.status.battery.percent == 87);
    CHECK(nextui_app_overlay_host(app) == 0);
    CHECK(log_contains("\"host\":false"));
    nextui_app_destroy(app);
    fake_stop();
}

static void test_dynamic_host(void)
{
    NextuiAppInfo info;
    NextuiApp* app;
    unsigned kinds = 0xFFFFu;

    if (!fake_start("adapted")) return;
    app = app_connect(&info);
    if (app == NULL)
    {
        fake_stop();
        return;
    }
    CHECK(info.overlay_host == 1);
    CHECK(info.overlay_host_declared == 1);
    CHECK(nextui_app_overlay_host_declared(app) == 1);

    CHECK(nextui_app_overlay_set_host(app, 0) == NEXTUI_APP_OK);
    CHECK(nextui_app_overlay_host(app) == 0);
    CHECK(nextui_app_overlay_host_declared(app) == 1);
    CHECK(nextui_app_overlay_set_open(app, 1) == NEXTUI_APP_ERR_NOT_HOSTING);
    CHECK(nextui_app_hud_get_policy(app, &kinds) == NEXTUI_APP_OK);
    CHECK(kinds == 0);
    {
        char response[NEXTUI_APP_LINE_MAX];
        CHECK(nextui_app_request(app,
                                 "{\"op\":\"system-feedback\",\"action\":\"volume\"}",
                                 response, sizeof response) == NEXTUI_APP_OK);
        CHECK(strstr(response, "\"handled_by_system\":true") != NULL);
        CHECK(nextui_app_request(app,
                                 "{\"op\":\"app-action\",\"action\":\"overlay\","
                                 "\"at_ms\":1}",
                                 response, sizeof response) == NEXTUI_APP_OK);
        CHECK(strstr(response, "\"handled_by_system\":true") != NULL);
    }

    CHECK(nextui_app_overlay_set_host(app, 1) == NEXTUI_APP_OK);
    {
        char response[NEXTUI_APP_LINE_MAX];
        CHECK(nextui_app_request(app,
                                 "{\"op\":\"system-feedback\",\"action\":\"volume\"}",
                                 response, sizeof response) == NEXTUI_APP_OK);
        CHECK(strstr(response, "\"handled_by_app\":true") != NULL);
        CHECK(nextui_app_request(app,
                                 "{\"op\":\"app-action\",\"action\":\"overlay\","
                                 "\"at_ms\":1}",
                                 response, sizeof response) == NEXTUI_APP_OK);
        CHECK(strstr(response, "\"handled_by_app\":true") != NULL);
    }
    CHECK(nextui_app_overlay_host(app) == 1);
    CHECK(nextui_app_overlay_set_open(app, 1) == NEXTUI_APP_OK);
    CHECK(nextui_app_hud_get_policy(app, &kinds) == NEXTUI_APP_OK);
    CHECK(kinds == NEXTUI_APP_HUD_ALL);
    CHECK(nextui_app_overlay_set_open(app, 0) == NEXTUI_APP_OK);

    nextui_app_destroy(app);
    fake_stop();
}

static void test_stale_session(void)
{
    NextuiAppInfo info;
    NextuiApp* app;
    NextuiBattery battery;
    char response[256];

    if (!fake_start("adapted")) return;
    app = app_connect(&info);
    if (app == NULL)
    {
        fake_stop();
        return;
    }
    CHECK(control("{\"op\":\"test.rotate_session\"}", response, sizeof response) == 1);
    CHECK(nextui_app_hud_set_policy(app, NEXTUI_APP_HUD_VOLUME, NEXTUI_APP_HUD_APP)
          == NEXTUI_APP_ERR_STALE);
    CHECK(strcmp(nextui_app_last_error(app), "session_stale") == 0);
    CHECK(nextui_app_overlay_set_open(app, 1) == NEXTUI_APP_ERR_STALE);
    CHECK(nextui_app_overlay_set_host(app, 0) == NEXTUI_APP_ERR_STALE);
    /* System-wide reads do not depend on the session generation. */
    CHECK(nextui_app_battery_get(app, &battery) == NEXTUI_APP_OK);
    nextui_app_destroy(app);
    fake_stop();
}

static void test_overlay_home(void)
{
    NextuiAppInfo info;
    NextuiApp* app;
    NextuiAppEvent event;
    char response[256];

    if (!fake_start("adapted")) return;
    app = app_connect(&info);
    if (app == NULL)
    {
        fake_stop();
        return;
    }
    CHECK(control("{\"op\":\"test.overlay_home\"}", response, sizeof response) == 1);
    CHECK(next_of_type(app, NEXTUI_APP_EVENT_OVERLAY_HOME, 1000, &event) == 1);
    CHECK(strcmp(event.data.overlay_home.action, "home") == 0);
    nextui_app_destroy(app);
    fake_stop();
}

static void test_sleep_status_detail(void)
{
    NextuiAppInfo info;
    NextuiApp* app;
    NextuiSleepStatus sleep_status;

    if (!fake_start("sleep_state")) return;
    app = app_connect(&info);
    if (app == NULL)
    {
        fake_stop();
        return;
    }
    CHECK(nextui_app_sleep_status(app, &sleep_status) == NEXTUI_APP_OK);
    CHECK(strcmp(sleep_status.phase, "preparing") == 0);
    CHECK(sleep_status.pending == 1);
    CHECK(strcmp(sleep_status.pending_sleep_id, "aaaabbbbccccdddd") == 0);
    CHECK(strcmp(sleep_status.pending_reason, "user") == 0);
    CHECK(sleep_status.pending_deadline_ms == 800);
    CHECK(sleep_status.pending_subscribers == 1);
    CHECK(sleep_status.inhibit_active == 1);
    CHECK(strcmp(sleep_status.inhibit_scope, "automatic") == 0);
    CHECK(strcmp(sleep_status.inhibit_reason, "playing_video") == 0);
    CHECK(sleep_status.inhibit_expires_in_ms > 0);
    CHECK(sleep_status.last_valid == 1);
    CHECK(strcmp(sleep_status.last_sleep_id, "1111222233334444") == 0);
    CHECK(strcmp(sleep_status.last_result, "ok") == 0);
    CHECK(sleep_status.last_slept_ms == 61000);
    nextui_app_destroy(app);
    fake_stop();
}

static void test_sleep_status(void)
{
    NextuiAppInfo info;
    NextuiApp* app;
    NextuiSleepStatus sleep_status;

    if (!fake_start("adapted")) return;
    app = app_connect(&info);
    if (app == NULL)
    {
        fake_stop();
        return;
    }
    CHECK(nextui_app_sleep_status(app, &sleep_status) == NEXTUI_APP_OK);
    CHECK(strcmp(sleep_status.phase, "awake") == 0);
    CHECK(sleep_status.pending == 0);
    CHECK(sleep_status.inhibit_active == 0);
    CHECK(sleep_status.last_valid == 0);
    nextui_app_destroy(app);
    fake_stop();
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc > 1) setenv("NEXTUI_FAKE_DEVSERV", argv[1], 1);
    printf("-- connect and reads --\n");
    test_connect_and_reads();
    printf("-- events --\n");
    test_events();
    printf("-- sleep ack and wake --\n");
    test_sleep_ack_and_wake();
    printf("-- sleep inhibit --\n");
    test_sleep_inhibit();
    printf("-- stale sleep --\n");
    test_stale_sleep();
    printf("-- unadapted --\n");
    test_unadapted();
    printf("-- no session --\n");
    test_no_session();
    printf("-- legacy server --\n");
    test_legacy_server();
    printf("-- garbage server --\n");
    test_garbage_server();
    printf("-- desktop battery --\n");
    test_desktop_battery();
    printf("-- call --\n");
    test_call();
    printf("-- session ended --\n");
    test_session_ended();
    printf("-- callback and timeout --\n");
    test_callback_and_timeout();
    printf("-- overflow --\n");
    test_overflow();
    printf("-- reconnect --\n");
    test_reconnect();
    printf("-- dynamic host --\n");
    test_dynamic_host();
    printf("-- stale session --\n");
    test_stale_session();
    printf("-- overlay home --\n");
    test_overlay_home();
    printf("-- sleep status detail --\n");
    test_sleep_status_detail();
    printf("-- sleep status --\n");
    test_sleep_status();

    if (failures != 0)
    {
        fprintf(stderr, "%d client test(s) failed\n", failures);
        return 1;
    }
    printf("test_client: all checks passed\n");
    return 0;
}
