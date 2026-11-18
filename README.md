# nextui-app-sdk

Framework-agnostic **app ↔ system cooperation SDK** for
[NextUI](https://github.com/4fuu/NextUI).

A small C99 library plus the canonical wire contract that lets a third-party app
participate in system functions as a first-class citizen instead of being served
by the system fallback. It speaks the existing `devserv` Unix-socket protocol and
depends on nothing but libc/POSIX — no SDL, no Apostrophe, no borealis, no
msettings.

Protocol: **1** (this repository is the source of truth).
Status: host implementation; device verification and the system side are later
phases.

## What is in the first batch

| Feature | What the app can do |
| --- | --- |
| **HOME overlay hosting** | Declare the existing top-level `"capabilities": ["overlay"]` in `pak.json`, own HOME in-process, draw the shared battery/Wi-Fi/Bluetooth/volume rows plus its own rows. The system yields HOME, draws nothing, grabs nothing (`handled_by_app`). The app can yield and restore this dynamically with `overlay.host` (e.g. Moonlight: menu hosts, streaming yields to the system panel). |
| **HUD cooperation** | Choose, per kind and at runtime, whether the app or the system draws the volume/brightness/colour-temperature/mute HUD (`hud.policy`). The app receives the new value as a `feedback` event. |
| **Sleep/wake handshake** | Receive `sleep.prepare` before suspend, pause/save and `sleep.ack`, or defer with `sleep.inhibit` (reason + timeout). Receive `wake.done` after resume. |
| **Battery/status** | `battery.status` on demand, `status.get` for all overlay rows, and push updates over one event stream. |

Apps that do not link the SDK are untouched: the C4 panel/HUD fallback and the
existing `ping`/`status`/`call`/session/`app-action` behavior remain exactly as
they are.

The full contract — every message, field, event, ordering rule, timeout, error
code, phase-B integration point, and open decision — is
[docs/CONTRACT.md](docs/CONTRACT.md). It extends NextUI's `DEVICE-API.md` (C1,
C2, C4) and `SESSION-API.md` (C3).

## Build

```sh
make                # build/libnextui_app.a
make test           # host tests (JSON + real-socket fake devserv)
make example        # build/nextui-overlay-demo
```

or:

```sh
cmake -S . -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

Requirements: a C99 compiler, POSIX sockets, pthreads. No external libraries.
Install with `make install PREFIX=/usr/local` (header + static library).

## Quick start

```c
#include "nextui_app.h"

static void on_sleep(NextuiApp* app, const NextuiSleepPrepare* prepare,
                     NextuiAppSleepDecision* decision, void* user)
{
    pause_and_save(user);                 /* must finish before returning */
    decision->action = NEXTUI_APP_SLEEP_ACK;
    decision->paused = 1;
    decision->saved = 1;
}

static void on_event(NextuiApp* app, const NextuiAppEvent* event, void* user)
{
    switch (event->type) {
    case NEXTUI_APP_EVENT_STATUS_SNAPSHOT:
    case NEXTUI_APP_EVENT_STATUS_CHANGED:  draw_rows(&event->data.status); break;
    case NEXTUI_APP_EVENT_FEEDBACK:        draw_hud(&event->data.feedback); break;
    case NEXTUI_APP_EVENT_OVERLAY_HOME:    toggle_overlay(app, user); break;
    case NEXTUI_APP_EVENT_WAKE_DONE:       resume_after_wake(user); break;
    case NEXTUI_APP_EVENT_SESSION_ENDED:   quit(user); break;
    default: break;
    }
}

int main(void)
{
    NextuiAppConfig config = {0};
    config.client_name = "myapp/1.0";
    NextuiApp* app = nextui_app_create(&config);
    NextuiAppInfo info;

    if (nextui_app_connect(app, &info) != NEXTUI_APP_OK) {
        nextui_app_destroy(app);
        run_without_cooperation();       /* old devserv, no session, etc. */
        return 0;
    }

    nextui_app_set_sleep_handler(app, on_sleep, state);
    nextui_app_set_event_callback(app, on_event, state);

    if (info.overlay_host) {
        nextui_app_hud_set_policy(app, NEXTUI_APP_HUD_ALL, NEXTUI_APP_HUD_APP);
        /* when the user opens the panel: */
        nextui_app_overlay_set_open(app, 1);
    }

    while (running) {
        nextui_app_wait(app, 100);       /* pumps events and callbacks */
    }
    nextui_app_destroy(app);
}
```

`connect` reads `$NEXTUI_DEVSERV_SOCKET`, `$NEXTUI_PAK_PATH`, and
`$NEXTUI_APP_ID` when the config fields are `NULL`. `client_name` is diagnostic
only.

## API surface

```c
NextuiApp* nextui_app_create(const NextuiAppConfig*);
void       nextui_app_destroy(NextuiApp*);
NextuiAppStatus nextui_app_connect(NextuiApp*, NextuiAppInfo*);
void       nextui_app_disconnect(NextuiApp*);
int        nextui_app_is_connected(const NextuiApp*);
const NextuiAppInfo* nextui_app_info(const NextuiApp*);

NextuiAppStatus nextui_app_battery_get(NextuiApp*, NextuiBattery*);
NextuiAppStatus nextui_app_status_get(NextuiApp*, unsigned sections, NextuiStatus*);
NextuiAppStatus nextui_app_sleep_status(NextuiApp*, NextuiSleepStatus*);

NextuiAppStatus nextui_app_sleep_ack(NextuiApp*, const char* sleep_id,
                                     int paused, int saved, const char* detail);
NextuiAppStatus nextui_app_sleep_inhibit(NextuiApp*, const char* reason, int timeout_ms);
NextuiAppStatus nextui_app_sleep_inhibit_for(NextuiApp*, const char* sleep_id,
                                             const char* reason, int timeout_ms,
                                             const char* detail);
NextuiAppStatus nextui_app_sleep_release(NextuiApp*);
NextuiAppStatus nextui_app_sleep_release_for(NextuiApp*, const char* sleep_id);

NextuiAppStatus nextui_app_hud_get_policy(NextuiApp*, unsigned* kinds_app); /* effective */
NextuiAppStatus nextui_app_hud_set_policy(NextuiApp*, unsigned kinds, NextuiAppHudMode);
int             nextui_app_overlay_host(const NextuiApp*);          /* effective */
int             nextui_app_overlay_host_declared(const NextuiApp*); /* permission */
NextuiAppStatus nextui_app_overlay_set_host(NextuiApp*, int host);
NextuiAppStatus nextui_app_overlay_set_open(NextuiApp*, int open);
NextuiAppStatus nextui_app_call(NextuiApp*, const char* capability, const char* action,
                                const char* args, char* value_out, size_t value_size);

void nextui_app_set_event_callback(NextuiApp*, NextuiAppEventCallback, void*);
void nextui_app_set_sleep_handler(NextuiApp*, NextuiAppSleepHandler, void*);
int  nextui_app_poll(NextuiApp*);
int  nextui_app_wait(NextuiApp*, int timeout_ms);
NextuiAppStatus nextui_app_next_event(NextuiApp*, NextuiAppEvent*, int timeout_ms);
```

Blocking and poll-friendly models are both supported:

- **Callbacks**: set an event callback and call `nextui_app_wait()` from your
  loop. Events are delivered on that thread.
- **Queue**: leave the callback unset and call `nextui_app_next_event()`; it
  blocks up to the timeout and copies the event into your struct.

`NextuiAppEvent` has fixed-size fields; no pointer refers into library memory.
`NextuiAppEvent.data.status.sections` tells you which sections a patch carried
(merge patches into your last snapshot).

### Thread safety

- Request functions (`battery_get`, `status_get`, `sleep_*`, `hud_*`,
  `overlay_*`, `call`) are serialized by an internal recursive mutex and may be
  called from any thread.
- `poll` / `wait` / `next_event` must be used by exactly one thread at a time
  and not concurrently with each other. Callbacks run on that thread; they may
  call request functions, but not poll/wait/next_event recursively.
- `destroy` must not race with other calls.
- The sleep handler consumes `sleep.prepare`; with no handler the library acks
  immediately (`paused:0`) and delivers the event for observation.

### Errors

`NEXTUI_APP_OK` or a negative `NextuiAppStatus`. `nextui_app_last_error()`
returns the stable server error code (`session_stale`, `sleep_stale`,
`not_adapted`, `not_hosting`, `unavailable`, …) when the server rejected a
request;
`nextui_app_last_detail()` is a diagnostic string. `NEXTUI_APP_ERR_UNSUPPORTED`
also covers an old devserv (`hello` → `bad_request`/`unknown operation`); the app
should then run without cooperation.

## How it works

### Two connection classes

- **Requests** keep the existing C1/C3/C4 shape: one line in, one line out, per
  connection, over `$NEXTUI_DEVSERV_SOCKET` (default
  `/tmp/nextui-devserv.sock`).
- **Events** use one persistent connection opened with `subscribe`. The server
  replies with `subscription.ready` (`seq:0`) and then streams
  newline-delimited events with strictly increasing `seq`, a `heartbeat` every
  15 s, and `session.ended` before closing on session end. Requests are never
  sent on this connection.

This is why async events do not break request/response: the two channels are
separate, and everything stays line-oriented JSON.

### Sleep handshake

```
sleep.request (system) ──► sleep.prepare (app)
                              ├─ sleep.ack      → decision: proceed
                              └─ sleep.inhibit   → decision: defer
sleep.finished (system) ──► wake.done (app)
```

- The system waits for `sleep_ack_ms` (default 1500), all acks, or the first
  inhibit. A missing ack never blocks sleep forever.
- `sleep.inhibit` with a `sleep_id` defers the current attempt; without one it
  suppresses automatic (idle) sleep for `timeout_ms` (1 s–5 min) but never an
  explicit user sleep.
- `sleep_id` is a 16-hex-digit stale guard. Late acks/inhibits get
  `sleep_stale` and change nothing.
- A subscription that opens during a pending prepare receives it immediately
  with the remaining deadline (replay), so an app that is still starting does
  not miss sleep.
- `wake.done` carries `slept_ms`, `woke_by`, `result`, and a battery snapshot.

### Overlay data flow

1. `pak.json` declares top-level `"capabilities": ["overlay"]` (not the C1
   `device.capabilities` list); `session-start` records the declaration and
   starts with `overlay_host` equal to it.
2. `hello` returns the session id and `overlay_host`.
3. `status.get` / `status.snapshot` provide the shared rows: battery
   (percent/charging/usb), Wi-Fi (enabled/connected/strength/ssid), Bluetooth,
   volume (level/min/max/mute/sink/locked), display, theme colours, clock
   preferences.
4. HOME goes to the app: keymon's `app-action overlay` returns
   `handled_by_app`, the system draws nothing, and devserv also emits
   `overlay.home` for apps that do not read `KEY_HOMEPAGE`.
5. `overlay.state` tells devserv whether the app's panel is open. While it is,
   all HUD kinds route to the app. `overlay.host host:false` yields both HOME
   and the HUD to the system at runtime (and `true` restores them within the
   declaration); while yielded, `overlay.state` is `not_hosting` and keymon
   draws the C4 fallback panel over the app.
6. Volume/brightness/colour-temperature/mute stay owned by keymon (it changes
   msettings and asks `system-feedback`); with the app policy devserv returns
   `handled_by_app` and emits `feedback`, and the app draws the HUD. Apps must
   not apply those keys a second time.
7. `status.changed` pushes coalesced patches; a full snapshot is sent every
   30 s and after every reconnect.

### Moonlight, streaming, and rendering routes

Moonlight's wrapper cannot draw while the engine owns fb0: its menu is an
Apostrophe/SDL process, but `stream_flow()` calls `ap_quit()` before spawning
`moonlight`. The recommended transition is:

- **menu → streaming**: close the panel → `overlay.state open:false` → release
  the input grab → `overlay_set_host(app, 0)` → tear down SDL → start the
  engine. HOME now opens the system C4 panel over the stream (safe), and the
  HUD is drawn by keymon.
- **streaming → menu**: rebuild SDL/UI → `overlay_set_host(app, 1)` → host the
  panel again.

The SDK only supplies **data** (`NextuiStatus` rows, `overlay.home`, `feedback`,
`sleep.prepare`/`wake.done`); it never draws. Both rendering routes are
therefore supported without contract changes:

- **lean** — Apostrophe's own status bar / `ap_draw_*` primitives;
- **full fidelity** — vendor NextUI's `UIOverlay`/software `GFX` and feed it the
  same rows.

An app that cannot draw must yield the host rather than keep a dead HOME key.
If the server predates `overlay.host`, use the documented fallback: HOME ends
the stream and returns to the menu.

While integrating, disable the app's own power/suspend path (Apostrophe:
`ap_set_power_handler(false)`) so `sleep.prepare`/`wake.done` is the only sleep
path, and use `sleep.inhibit` instead of `/tmp/stay_awake`-style flags.

### Compatibility

- Unadapted apps: system panel/HUD, `handled_by_system`, no events — unchanged.
- Dynamic host missing (old/partial server): `overlay_set_host` returns
  `unsupported`; static C4 routing remains. A streaming app that cannot draw
  must implement "HOME ends the stream" itself (see contract §7.9.5).
- Old devserv: `hello` is unknown → `NEXTUI_APP_ERR_UNSUPPORTED`; `ping`,
  `call`, sessions, `app-action` still work for legacy clients.
- No session: battery/status/sleep-status work; HUD/overlay calls return
  `not_active`; a system-scope subscription still receives sleep/status.
- Missing feature: the operation returns `unsupported`; the app degrades.
- The desktop preview reports battery/volume/display/theme `available:false`
  rather than inventing values.

## Tests

`make test` builds `tests/fake_devserv` and runs two host binaries:

- `tests/test_json.c` — nested lookup, UTF-8/`\u` escapes, numbers, arrays,
  malformed input, writer escaping/bounds.
- `tests/test_client.c` — a real Unix socket against the fake server, covering:
  negotiation and session discovery; battery/status reads; `status.snapshot` and
  `status.changed`; feedback; sleep prepare → ack and → inhibit; wake; stale
  `sleep_id`; unadapted fallback; no session; legacy server; malformed server;
  desktop battery; `call`; session end; callback vs queue; overflow; automatic
  reconnect (including re-applying `overlay_set_host`); dynamic host
  yield/restore and `not_hosting`; stale session; `overlay.home`;
  `sleep.status` detail.

The fixture is scripted through `test.*` control operations and logs every
request, so the tests assert the actual wire messages (for example
`{"op":"sleep.ack",...,"paused":true}`), not mock calls. See
`tests/fake_devserv.c`.

## Repository layout

```
include/nextui_app.h     public API
src/                     client implementation (app, JSON, transport)
docs/CONTRACT.md         canonical protocol (protocol 1)
examples/overlay_demo.c  host-buildable reference usage
tests/                   JSON + real-socket client tests, fake devserv
.github/workflows/ci.yml make + CMake/CTest on Ubuntu
```

## Phase B (system side) — what is required

The contract is implementable, but it is not yet implemented in NextUI. Phase B
must:

- turn `devserv` into a poll-based server with a subscription registry and event
  fan-out (the current `accept → one request → close` loop cannot host events);
- add the sleep coordinator and `sleep.*` ops, and wire it into
  `PWR_sleep`/`PWR_deepSleep` and keymon's power key (keymon currently observes
  no power key at all);
- add the status/battery provider (honest `available:false` on desktop) and the
  HUD policy/sampling;
- emit `overlay.home` and implement `overlay.state` and `overlay.host`
  (effective vs declared host, `not_hosting`);
- keep the C4 fallback intact.

The hard unknown is the **resident suspend path while a third-party app is
foreground**: the launcher is blocked in `nextui_session_run`, and `PWR_*` lives
in `api.c`. keymon has no power handling today. Phase B must choose a resident
suspend helper (or extend keymon) and document it. The full list of integration
points and open decisions is in [docs/CONTRACT.md §13 and §16](docs/CONTRACT.md).

## License

MIT. See [LICENSE](LICENSE). Apps under any license (GPLv3 wiliwili, GPL
Moonlight, …) can link it; NextUI itself keeps its own license.
