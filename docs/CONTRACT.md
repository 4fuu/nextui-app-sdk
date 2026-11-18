# NextUI app ↔ system cooperation contract (v0 draft)

Canonical protocol between a NextUI app (via `nextui-app-sdk`) and the system
(`devserv` / `keymon` / power path in `4fuu/NextUI`). Transport: the existing
devserv unix socket (`$NEXTUI_DEVSERV_SOCKET`, default `/tmp/nextui-devserv.sock`),
newline-delimited JSON requests/responses, plus a server-pushed event channel.

This document is the source of truth; both the system side and the client library
implement it. It extends — does not replace — `4fuu/NextUI` DEVICE-API.md (C1
capabilities, C2 LED lease) and SESSION-API.md (C3 session, C4 exit/overlay).

## Scope (first batch)

1. HOME overlay hosting (`overlay_host`, `handled_by_app`).
2. Volume/brightness/colortemp/mute HUD ownership (`system-feedback`).
3. Sleep/wake handshake (pre-sleep / post-wake events, sleep inhibit).
4. Battery/status read + change subscription.

## Existing pieces this builds on (verified in 4fuu/NextUI)

- C1: `ping`/`status`/`call` capabilities, default-allow.
- C3: launch-scoped session, dirty-only device-field snapshot/restore.
- C4: `app-action home|overlay`, `system-feedback`, `overlay_host` →
  `handled_by_app`; unadapted → `handled_by_system` (keymon fallback overlay);
  launch-parent bounded exit supervisor.

## To be specified (design task)

- Event channel: how the app receives async events (sleep/wake, status) over the
  socket without breaking the request/response model; session scoping; stale guard.
- Sleep/wake handshake: `sleep.prepare` event → app ack / `sleep.inhibit` request
  (with reason + timeout) → system proceeds or defers; `wake.done` event. Where the
  system power path (api.c PWR_sleep / PLAT_deepSleep, keymon power key) emits these.
- Overlay hosting: app declares host; system yields HOME + navigation; shared system
  rows the app should render (battery/wifi/bt/volume) and the data it needs; HUD
  ownership negotiation; grab semantics when app hosts.
- Battery/status: `battery.status` read shape; `status.subscribe` push cadence.
- Versioning + capability negotiation; backward compatibility with unadapted apps.
- C client API surface (init/connect, request, event poll/callback, overlay helpers,
  sleep inhibit, battery read), error model, thread-safety, no UI-framework deps.

(Filled in by the design/implementation task.)
