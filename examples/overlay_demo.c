/*
 * Minimal overlay-host + sleep + battery demo.
 *
 * It does not draw anything: it prints the data an in-process overlay would
 * render, reacts to HOME and HUD events, and participates in the sleep
 * handshake. This is the host-buildable reference for a real UI binding.
 *
 *   ./build/nextui-overlay-demo [socket-path]
 *
 * Without a devserv (or with an old one) it prints why and exits non-zero; a
 * real app should simply continue without cooperation.
 */
#define _POSIX_C_SOURCE 200809L

#include "nextui_app.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;

static void on_signal(int number)
{
    (void)number;
    running = 0;
}

static const char* kind_name(NextuiAppHudKind kind)
{
    switch (kind)
    {
    case NEXTUI_APP_HUD_VOLUME: return "volume";
    case NEXTUI_APP_HUD_BRIGHTNESS: return "brightness";
    case NEXTUI_APP_HUD_COLORTEMP: return "colortemp";
    case NEXTUI_APP_HUD_MUTE: return "mute";
    case NEXTUI_APP_HUD_ALL: break;
    }
    return "?";
}

static void print_battery(const NextuiBattery* battery)
{
    if (!battery->available)
        printf("battery: unavailable\n");
    else
        printf("battery: %d%%%s%s\n", battery->percent, battery->charging ? " charging" : "",
               battery->usb ? " usb" : "");
}

static void print_rows(const NextuiStatus* status)
{
    if (status->sections & NEXTUI_APP_SECTION_BATTERY) print_battery(&status->battery);
    if (status->sections & NEXTUI_APP_SECTION_WIFI)
        printf("wifi: %s, %s, %s\n", status->wifi.enabled ? "on" : "off",
               status->wifi.strength[0] ? status->wifi.strength : "?",
               status->wifi.ssid[0] ? status->wifi.ssid : "-");
    if (status->sections & NEXTUI_APP_SECTION_BLUETOOTH)
        printf("bluetooth: %s, %s\n", status->bluetooth.enabled ? "on" : "off",
               status->bluetooth.connected ? "connected" : "not connected");
    if (status->sections & NEXTUI_APP_SECTION_VOLUME)
        printf("volume: %d/%d%s%s\n", status->volume.level, status->volume.max,
               status->volume.mute ? " muted" : "",
               status->volume.locked ? " locked" : "");
    if (status->sections & NEXTUI_APP_SECTION_DISPLAY)
        printf("display: brightness %d/%d, colortemp %d/%d\n", status->display.brightness,
               status->display.brightness_max, status->display.colortemp,
               status->display.colortemp_max);
}

static void on_sleep(NextuiApp* app, const NextuiSleepPrepare* prepare,
                     NextuiAppSleepDecision* decision, void* user)
{
    (void)app;
    (void)user;
    printf("[sleep] prepare %s reason=%s deadline=%d ms\n", prepare->sleep_id,
           prepare->reason, prepare->deadline_ms);
    print_battery(&prepare->battery);

    /* A real app pauses its pipeline and flushes its save state here. */
    decision->action = NEXTUI_APP_SLEEP_ACK;
    decision->paused = 1;
    decision->saved = 1;
    decision->detail = "demo paused";
}

static void on_event(NextuiApp* app, const NextuiAppEvent* event, void* user)
{
    int* overlay_open = (int*)user;

    switch (event->type)
    {
    case NEXTUI_APP_EVENT_STATUS_SNAPSHOT:
        printf("[status] snapshot\n");
        print_rows(&event->data.status);
        break;
    case NEXTUI_APP_EVENT_STATUS_CHANGED:
        printf("[status] changed\n");
        print_rows(&event->data.status);
        break;
    case NEXTUI_APP_EVENT_FEEDBACK:
        printf("[hud] %s = %d (max %d)%s%s\n", kind_name(event->data.feedback.kind),
               event->data.feedback.value, event->data.feedback.max,
               event->data.feedback.mute ? " muted" : "",
               event->data.feedback.locked ? " locked" : "");
        break;
    case NEXTUI_APP_EVENT_OVERLAY_HOME:
        *overlay_open = !*overlay_open;
        printf("[overlay] HOME -> %s\n", *overlay_open ? "open" : "closed");
        if (nextui_app_overlay_set_open(app, *overlay_open) != NEXTUI_APP_OK)
            printf("[overlay] could not report state: %s\n", nextui_app_last_error(app));
        break;
    case NEXTUI_APP_EVENT_WAKE_DONE:
        printf("[sleep] woke after %llu ms (%s, %s)\n", event->data.wake_done.slept_ms,
               event->data.wake_done.woke_by, event->data.wake_done.result);
        print_battery(&event->data.wake_done.battery);
        break;
    case NEXTUI_APP_EVENT_SESSION_ENDED:
        printf("[session] ended (%s)\n", event->data.session_ended.reason);
        running = 0;
        break;
    case NEXTUI_APP_EVENT_OVERFLOW:
        printf("[event] overflow; resync needed\n");
        break;
    case NEXTUI_APP_EVENT_TRANSPORT_ERROR:
        printf("[event] transport error: %s (%s)\n", event->data.error.code,
               event->data.error.detail);
        break;
    case NEXTUI_APP_EVENT_SLEEP_PREPARE:
    case NEXTUI_APP_EVENT_NONE:
        break;
    }
}

int main(int argc, char** argv)
{
    NextuiAppConfig config;
    NextuiAppInfo info;
    NextuiApp* app;
    NextuiAppStatus status;
    int overlay_open = 0;
    int seconds = 15;

    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    memset(&config, 0, sizeof config);
    config.client_name = "nextui-overlay-demo/1.0";
    if (argc > 1) config.socket_path = argv[1];
    if (argc > 2) seconds = atoi(argv[2]);

    app = nextui_app_create(&config);
    if (app == NULL)
    {
        fprintf(stderr, "cannot create the SDK client\n");
        return 1;
    }
    status = nextui_app_connect(app, &info);
    if (status != NEXTUI_APP_OK)
    {
        fprintf(stderr, "not connected: %s (%s: %s)\n", nextui_app_status_string(status),
                nextui_app_last_error(app), nextui_app_last_detail(app));
        nextui_app_destroy(app);
        return 1;
    }

    printf("protocol %d, contract %s\n", info.protocol, info.contract);
    printf("session: %s, overlay_host: %s\n",
           info.session_active ? info.session_id : "(none)",
           info.overlay_host ? "yes" : "no");
    print_battery(&info.battery);

    if (getenv("NEXTUI_DEMO_STREAMING") != NULL)
    {
        /* Simulates Moonlight's streaming mode: SDL is gone, so HOME/HUD must
         * go to the system fallback until the app restores the host. */
        if (nextui_app_overlay_set_host(app, 0) == NEXTUI_APP_OK)
            printf("overlay host: yielded to the system (streaming)\n");
        else
            printf("overlay host: cannot yield (%s)\n", nextui_app_last_error(app));
    }

    if (nextui_app_overlay_host(app))
    {
        unsigned kinds = 0;
        if (nextui_app_hud_set_policy(app, NEXTUI_APP_HUD_ALL, NEXTUI_APP_HUD_APP)
            == NEXTUI_APP_OK)
            printf("HUD: drawn in-process\n");
        if (nextui_app_hud_get_policy(app, &kinds) == NEXTUI_APP_OK)
            printf("HUD app mask: 0x%x\n", kinds);
    }
    else if (nextui_app_overlay_host_declared(app))
    {
        printf("HUD: system fallback (host yielded)\n");
    }
    else
    {
        printf("HUD: system fallback (not adapted)\n");
    }

    nextui_app_set_sleep_handler(app, on_sleep, NULL);
    nextui_app_set_event_callback(app, on_event, &overlay_open);

    while (running && seconds-- > 0)
    {
        int result = nextui_app_wait(app, 1000);
        if (result < 0 && result != NEXTUI_APP_ERR_NOT_CONNECTED) break;
    }

    if (overlay_open) (void)nextui_app_overlay_set_open(app, 0);
    nextui_app_destroy(app);
    return 0;
}
