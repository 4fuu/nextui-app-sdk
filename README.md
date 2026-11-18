# nextui-app-sdk

Framework-agnostic **app ↔ system cooperation SDK** for [NextUI](https://github.com/4fuu/NextUI).

A thin C client library + canonical protocol contract that lets a third-party app
participate in system functions as a first-class citizen, instead of being managed
by the system fallback. Independent of any UI framework (works with Apostrophe,
borealis, raw SDL, or anything else) — it speaks the `devserv` unix-socket protocol.

## Why

NextUI system apps (launcher, settings, clock) cooperate with the desktop through
`api.c` and the device session contract. Third-party apps currently run isolated and
only get the system *fallback* overlay. This SDK closes that gap: an app that links it
can host its own HOME overlay, cooperate on volume/brightness HUDs, take part in the
sleep/wake handshake, and read battery/status — the same cooperative behaviors system
apps have. Apps that do not integrate still work via the system fallback.

## First batch (in scope)

1. **HOME overlay hosting** — app declares `overlay_host`, owns HOME in-process
   (`handled_by_app`), renders the shared system rows (battery / Wi-Fi / Bluetooth /
   volume) plus its own rows; the system yields and draws nothing.
2. **Volume / brightness HUD cooperation** — `system-feedback` ownership handshake;
   app draws the HUD in-process or defers to the system.
3. **Sleep / wake handshake** — system notifies the app before suspend (pause / save)
   and after wake (resume); the app can request a sleep inhibit while playing.
4. **Battery / status reporting** — app reads battery/charging/USB state on demand and
   subscribes to changes.

Later batches (not now): save/resume + game quick-switcher participation, LED control
cooperation, richer status/now-playing.

## Architecture

- **Canonical contract** lives in `docs/CONTRACT.md` (protocol messages, semantics,
  versioning). The system side (`4fuu/NextUI` devserv/keymon/power path) and this client
  library both implement it; this repo is the source of truth.
- **Client library** (`src/`, `include/`): pure C, no UI-framework dependency, links
  against libc only. Connects to the devserv socket, sends requests, receives events
  (sleep/wake, status). Blocking + poll-friendly API.
- **System side** is implemented in `4fuu/NextUI` against the same contract.

## License

MIT (see `LICENSE`) so apps under any license (GPLv3 wiliwili, GPL Moonlight, etc.)
can link it. The NextUI system itself remains under its own license.

## Status

Design + first-batch implementation in progress. See `docs/CONTRACT.md`.
