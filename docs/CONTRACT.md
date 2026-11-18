# NextUI app ↔ system cooperation contract

Status: **protocol 1 (final for the first batch)**. This document is the source of
truth for the system side (`devserv`, `keymon`, the power path in `4fuu/NextUI`)
and for every client that links `nextui-app-sdk`. It replaces the v0 draft and
extends — it does not replace — NextUI's existing
[DEVICE-API.md](https://github.com/4fuu/NextUI/blob/main/DEVICE-API.md) (C1
capabilities, C2 LED lease, C4 app exit + system overlay) and
[SESSION-API.md](https://github.com/4fuu/NextUI/blob/main/SESSION-API.md) (C3
device sessions).

The first batch covers four cooperative features:

1. **HOME overlay hosting** — an adapted app declares the existing `overlay`
   manifest capability, owns HOME in-process, and draws the shared system rows
   plus its own rows. The system yields HOME and draws/grab nothing.
2. **HUD cooperation** — the existing `system-feedback` ownership handshake is
   extended with a per-session policy, so an app can draw the
   volume/brightness/colour-temperature/mute HUD in-process or defer to the
   system, per kind, at runtime.
3. **Sleep/wake handshake** — the system emits `sleep.prepare`, the app acks or
   inhibits (reason + timeout), the system proceeds or defers, and `wake.done`
   follows the resume.
4. **Battery/status** — `battery.status` reads the battery, `status.get` reads
   the data the shared overlay rows need, and one event subscription pushes
   changes.

Everything is exchanged over the existing devserv Unix socket
(`$NEXTUI_DEVSERV_SOCKET`, default `/tmp/nextui-devserv.sock`) as
newline-delimited JSON. The request/response model of C1/C3/C4 is preserved
exactly; async events use a **separate, append-only connection class**.

Framework bindings (Apostrophe, borealis, raw SDL) are out of scope for this
repository; this contract and the C library are framework-agnostic.

---

## 1. What already exists and is reused

These are the real, implemented pieces this contract builds on. They are
normative and unchanged unless a later section explicitly extends them.

| Piece | Where | What this contract uses |
| --- | --- | --- |
| Newline-delimited JSON on a Unix stream socket | `workspace/all/devserv/devserv.c`, `workspace/all/common/device_protocol.{c,h}` | Same framing, same socket path selection, same 8192-byte line limit. |
| `ping` / `status` / `call` | `device_protocol.c`, `DEVICE-API.md` C1 | `ping` gains fields; `status` and `call` are untouched. |
| Default-allow policy | `device_gate_set_default_allow(1)`, `DEVICE-API.md` | Unchanged. This is a cooperation channel, not a security boundary. |
| Launch-scoped session | `device_session.c`, `SESSION-API.md` | One active session, 16-hex-digit generation id, `session_stale` guard. |
| Session snapshot/restore | `SESSION-API.md` | Untouched; this contract adds no session fields. |
| `app-action home\|overlay` | `device_protocol.c`, `DEVICE-API.md` C4 | `overlay` still returns `handled_by_app` for adapted apps and `handled_by_system` for unadapted apps. |
| `system-feedback volume\|brightness\|colortemp\|mute` | `devserv.c`, `DEVICE-API.md` C4 | Still returns `handled_by_app` / `handled_by_system`; now consults `hud.policy`. |
| System overlay fallback | keymon `system_overlay.c`, C4 | Unchanged for unadapted apps and for apps that never connect. |
| Session display name | `app-action` response `app` | Unchanged. |
| Exit supervisor | `app-exit-result`, C4 | Unchanged. |
| `pak.json` top-level `"capabilities": ["overlay"]` | `pak_manifest.c` (`PAK_CAPABILITY_OVERLAY`, the exact name `"overlay"`; the Clock pak is the in-tree example) | This is the **only** declaration an app makes to host the overlay. It is the top-level `capabilities` array, not the C1 `device.capabilities` list. No new manifest key is introduced. The session response field is `overlay_host` (effective) and `overlay_host_declared` (this declaration). |

Existing stable error names (`bad_request`, `unknown_capability`,
`unavailable`, `backend_error`, `session_active`, `session_stale`,
`not_active`, `restore_partial`) keep their meaning. New names are listed in
§9.

The existing `battery.read` and `power.info` capability names are registered in
`device_caps.c` but no backend implements them (`device_backend_display_audio`
does not handle them, and the `none` backend reports `unavailable`). This
contract adds `battery.status` as the canonical battery read. It does not
change or resurrect the old capability names.

---

## 2. Transport and framing

- **Socket**: `AF_UNIX`, `SOCK_STREAM`. Path: `$NEXTUI_DEVSERV_SOCKET` when set,
  else `/tmp/nextui-devserv.sock`. `devserv` keeps its `0600` socket and
  `umask(0077)`.
- **Encoding**: UTF-8 JSON, one JSON object per line, terminated by `\n`.
  A `\r` immediately before `\n` is ignored. Empty lines are invalid.
- **Line limit**: 8192 bytes including the terminating newline
  (`DEVICE_PROTOCOL_LINE_MAX`). A longer request is answered with
  `bad_request` (`"request line is too long"`), as today. A longer response or
  event is a server bug; clients must reject it with a protocol error.
- **Ordering**: requests are answered in the order they are sent on their own
  connection. Different connections are independent. There is no request
  pipelining on a single connection.
- **No HTTP, no TLS, no authentication.** All processes run as root on the
  device. The socket is a cooperative channel, not a sandbox.

### 2.1 Two connection classes

**Request connection (existing C1/C3/C4 behavior).**
The client connects, writes exactly one request line, may `shutdown(SHUT_WR)`,
reads exactly one response line, then closes. The server reads one line, writes
one line, and closes. This stays true for every operation in §8. No request may
expect more than one line on a request connection.

**Event connection (new).**
The client connects and sends exactly one line:

```json
{"op":"subscribe", ...}
```

The server answers with the `subscription.ready` event line and then keeps the
connection open, writing event lines until the client closes it, the session
ends, an overflow occurs, or the server shuts down. The client MUST NOT send
another line on this connection; the server MUST reject any additional line with
`bad_request` and close.

This split is deliberate: it preserves the existing one-request/one-response
contract and lets old clients keep using short-lived connections, while events
have a natural, ordered stream.

### 2.2 Monotonic timestamps

`at_ms` is milliseconds on `CLOCK_MONOTONIC` (the same clock keymon already puts
in `app-action.at_ms`). It is **not** wall-clock time. It is meaningful within
one boot and one server lifetime; it is used for ordering and stale guards, not
for display. `started_at` in session responses remains wall-clock seconds, as
defined by C3.

---

## 3. Versioning and capability negotiation

### 3.1 `ping` (backward compatible)

`ping` keeps its existing shape and gains additive fields:

```json
{"op":"ping"}
```

```json
{
  "ok": true,
  "service": "devserv",
  "version": 1,
  "protocol": 1,
  "contract": "nextui-app/1",
  "features": ["events", "sleep", "status", "hud", "overlay", "battery"],
  "limits": {
    "line_max": 8192,
    "event_queue": 64,
    "subscribe_max": 8,
    "sleep_ack_ms": 1500,
    "inhibit_max_ms": 300000,
    "sleep_cooldown_ms": 1000,
    "heartbeat_ms": 15000,
    "status_min_interval_ms": 500,
    "status_resync_ms": 30000,
    "feedback_min_interval_ms": 100
  }
}
```

- `version` is the C1 service revision and remains `1`. Existing clients only
  look at `ok`; `nextdev ping` prints the line unchanged.
- `protocol` is the cooperation protocol generation defined by this document.
  This document is protocol `1`.
- `features` is the set of event families and operations the server implements.
  The names are stable: `events`, `sleep`, `status`, `hud`, `overlay`,
  `battery`. `events` is required for every event family.
- `limits` may be omitted; clients use the defaults above. A server MUST NOT
  advertise a limit it does not enforce.

### 3.2 `hello`

`hello` is the SDK's initialization request. It is idempotent and cheap.

```json
{
  "op": "hello",
  "client": "wiliwili/1.0.0",
  "protocol": 1,
  "features": ["events", "sleep", "status", "hud", "overlay", "battery"],
  "app": "com.fuu.wiliwili",
  "pak": "/mnt/SDCARD/Tools/tg5040/Wiliwili.pak"
}
```

- `client` is optional and for diagnostics only (≤ 64 bytes). Never parsed for
  authorization.
- `protocol` is the client's maximum supported generation.
- `features` is optional; it declares what the client will consume. The server
  enables the intersection and reports it. Unknown names are ignored.
- `app` and `pak` are optional. When `pak` is present the server canonicalizes
  it exactly like `session-start` (`realpath`, manifest id). If both are present
  and disagree with the manifest, the response is `bad_request`, matching C3.
  A client that omits them is still served.

Success:

```json
{
  "ok": true,
  "service": "devserv",
  "version": 1,
  "protocol": 1,
  "contract": "nextui-app/1",
  "features": ["events", "sleep", "status", "hud", "overlay", "battery"],
  "limits": { "...": "same object as ping" },
  "session": {
    "active": true,
    "session_id": "0123456789abcdef",
    "app": "com.fuu.wiliwili",
    "pak": "/mnt/SDCARD/Tools/tg5040/Wiliwili.pak",
    "overlay_host": true,
    "overlay_host_declared": true,
    "overlay_open": false,
    "started_at": 1760000000,
    "matches_pak": true
  },
  "battery": {"available": true, "percent": 87, "charging": false, "usb": false},
  "at_ms": 120345
}
```

- `session` is always present; when no session is active it is
  `{"active": false}`. `overlay_host` is the effective host (static declaration
  unless changed with `overlay.host`); `overlay_host_declared` is the manifest
  permission.
- `matches_pak` is `true` when the request did not carry a `pak` or the
  canonical pak equals the active session's pak. A client that sees `false`
  MUST treat `session_id` as unusable (it belongs to another app) and must not
  issue session-scoped requests with it.
- `battery` is the same object as `battery.status` (§8.2), included because
  every overlay needs it at startup. It may be absent when `battery` is not in
  `features`.

If the client's `protocol` is **lower** than the server's minimum, the server
returns:

```json
{"ok":false,"error":"protocol_unsupported","server_protocol":1,"min_protocol":1,
 "detail":"this server requires protocol 1"}
```

A server that predates this contract returns `bad_request` with detail
`"unknown operation"` for `hello`. Clients MUST treat both
`protocol_unsupported` and `unknown operation` as "this server does not
implement the cooperation protocol" and degrade (§12).

### 3.3 Compatibility rules

- Within protocol 1, **adding** a field, an event name, an operation, a feature
  name, or an error name is allowed. Clients MUST ignore unknown fields, skip
  unknown events, and treat unknown feature names as unsupported.
- **Removing** a field, renaming one, changing a type, or changing the meaning
  of an existing value requires a new `protocol` number.
- Event lines always carry `event`, `seq`, and `at_ms`; unknown event names are
  ignored but still advance `seq`.
- A client that requests protocol 1 from a protocol 2 server keeps working if
  the server preserves the protocol 1 surface but may miss new features.
- The `contract` string is for humans and logs only. It is not parsed for
  decisions; `protocol` is.

---

## 4. Sessions, identity, and stale guards

C3 defines the session; this contract adds no snapshot/restore fields and no
session lifecycle operation. It uses the existing ones as follows.

- `session-start` records the manifest's top-level `"capabilities": ["overlay"]`
  (`device_session_note_overlay_host`). The session response includes
  `overlay_host` (effective) and `overlay_host_declared` (the manifest
  permission); at start they are equal.
- A client obtains `session_id` from `hello` (or from `session-status`). It is
  16 lowercase hex digits.
- Session-scoped operations are: `hud.policy`, `overlay.state`, `overlay.host`,
  `sleep.ack`, `sleep.inhibit` (with a `sleep_id`), `sleep.release`, and the
  existing `app-action`/`app-exit-result`. All of them validate `session_id`
  when it is present: it MUST equal the active session or the request fails
  with `session_stale` and changes nothing. `hud.policy`, `overlay.state`,
  `overlay.host`, and pre-inhibit `sleep.inhibit` require it; `sleep.ack` and
  `sleep.release` may omit it only for a system-scope subscription.
- `sleep.status`, `battery.status`, and `status.get` are system-wide and do not
  require a session.
- `subscribe` is bound to a session when `session_id` is supplied. When the
  session ends, the server sends `session.ended` and closes the stream. A
  subscription opened without a session is a **system-scope** subscription: it
  receives system-wide sleep/status events and never HUD/overlay events.
- The session's generation id is still not a secret and not an authorization
  token. Its only guarantee is stale-request isolation, exactly as C3 states.

**Stale event rule.** Every event that belongs to a session carries
`session_id`. A client bound to session `S` MUST drop an event whose
`session_id` is present and not `S`, and MUST reconnect after `hello` if the
active session changed. A server MUST NOT deliver events from a previous
session to a subscription bound to a newer one, and MUST send `session.ended`
before closing the old subscription.

---

## 5. Event channel

### 5.1 `subscribe`

Sent on a fresh connection. This is the **only** operation on that connection.

```json
{
  "op": "subscribe",
  "session_id": "0123456789abcdef",
  "features": ["sleep", "status", "hud", "overlay"],
  "status": {
    "fields": ["battery", "wifi", "bluetooth", "volume", "display", "theme", "clock"],
    "min_interval_ms": 500
  }
}
```

- `session_id` optional. When present it must match the active session,
  otherwise the server answers with an error line (`session_stale`) and closes.
- `features` optional; default is every family the server supports. Only the
  intersection is enabled. Requesting `hud` or `overlay` without a session is
  allowed but those events are never sent (there is no session to attribute
  them to); the server reports this in `subscription.ready` by omitting them
  from `features`.
- `status.fields` optional; default is all seven sections. Unknown names are
  ignored. `min_interval_ms` is optional, default
  `limits.status_min_interval_ms` (500), allowed range 250–5000; out of range
  is `bad_request`.
- The server enforces `limits.subscribe_max` concurrent subscriptions (default
  8) and answers `too_many_subscriptions` when exceeded.

The first line on the connection is the subscription acknowledgement, which is
itself an event envelope:

```json
{
  "ok": true,
  "event": "subscription.ready",
  "seq": 0,
  "at_ms": 120400,
  "service": "devserv",
  "protocol": 1,
  "features": ["sleep", "status", "hud", "overlay"],
  "session_id": "0123456789abcdef",
  "overlay_host": true,
  "limits": { "...": "same object as ping" }
}
```

`seq` on `subscription.ready` is always `0`; the first real event is `1`.
`features` here is the enabled **event-family** subset (`sleep`, `status`,
`hud`, `overlay`); the `events` feature from `ping`/`hello` is implied.
`overlay_host` is `false` when the subscription has no session or the session
is not adapted.

### 5.2 Event envelope

Every subsequent line is one event:

```json
{"event":"<name>","seq":7,"at_ms":120500,"session_id":"0123456789abcdef", ...payload}
```

- `seq` starts at 1 and increases by exactly 1 for every event on this
  subscription, including `heartbeat` and `overflow`. A client that sees a gap
  or a duplicate HAS missed or duplicated events and MUST resynchronize
  (`status.get`, `sleep.status`) and may reconnect.
- `session_id` is present for session-scoped events (`feedback`,
  `overlay.home`, `session.ended`, and the session's `sleep.prepare`/
  `wake.done` when a session was active when the sleep started). `status.*`
  events may also carry the active session id even though their data is
  system-wide. It is absent for `heartbeat`, `status.*` while no session is
  active, and sleep events while no session is active.
- Unknown fields are ignored. Unknown event names are ignored after `seq`
  handling.
- `at_ms` is the server's monotonic time when the event was generated.

### 5.3 Liveness

- **Heartbeat**: when no event has been written for `limits.heartbeat_ms`
  (default 15000 ms), the server sends
  `{"event":"heartbeat","seq":N,"at_ms":N}`. Heartbeats are cheap and keep
  middleboxes (none exist here) and the client's liveness check honest.
- A client MUST consider the stream dead after 3 × `heartbeat_ms` without any
  line (default 45 s) and reconnect.
- **Backpressure**: the server buffers at most `limits.event_queue` events per
  subscription (default 64). Status/heartbeat events are dropped first when
  the queue is full; `sleep.prepare`, `wake.done`, and `session.ended` are never
  dropped. If the queue still overflows, the server sends
  `{"event":"overflow","seq":N,"at_ms":N,"dropped":M}` (best effort) and closes
  the connection. The client MUST treat `overflow` as "state is unknown":
  reconnect, re-`hello`, re-`subscribe`, then `status.get` and `sleep.status`.
- **Server shutdown / restart**: the server closes event connections. Clients
  reconnect with backoff and re-run `hello`; if the session is gone they receive
  `session.ended` semantics from `hello.session.active == false`.
- `session.ended`:

```json
{"event":"session.ended","seq":N,"at_ms":N,"session_id":"0123456789abcdef",
 "reason":"stopped|replaced|server_shutdown"}
```

  The server sends it (best effort) and closes. The client MUST stop using the
  old `session_id` and MUST NOT automatically re-subscribe to the same session.

### 5.4 Pending `sleep.prepare` replay

If a client subscribes while a `sleep.prepare` is still pending (the system is
waiting for acks), the server MUST send that `sleep.prepare` to the new
subscriber immediately after `subscription.ready`, with the same `sleep_id`,
the original `reason`, and `deadline_ms` set to the **remaining** time (never
negative). This closes the race where an app is still starting while the device
is already trying to sleep. `sleep.status` returns the same pending record.

### 5.5 What is not replayed

`status` and `feedback` events are not replayed. A new subscriber gets a fresh
`status.snapshot` (if it subscribed to `status`), which is sufficient. `hud`
and `overlay` events are transient by nature.

---

## 6. Sleep/wake handshake

### 6.1 Overview

```
                    sleep.request (system only)
                            │
                            ▼
   sleep.prepare ──► app: sleep.ack (ready)      ──► decision: proceed
                 └─► app: sleep.inhibit (reason) ──► decision: defer
                            │
              (after the caller suspends and resumes)
                            │
   sleep.finished (system only) ──► wake.done (to subscribers)
```

The system is the only initiator. The app never asks the system to sleep; it
can only allow, defer, or pre-inhibit.

### 6.2 `sleep.request` (system operation)

Only system components (`keymon`, `api.c`, a future idle timer) send this. It is
not part of the app-facing SDK surface and SDK clients MUST NOT send it: a
process that both initiates and must ack would deadlock its own event loop.

```json
{"op":"sleep.request","reason":"user","initiator":"keymon","timeout_ms":1500}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `reason` | string | `user`, `idle`, `low_battery`, or `lid`. Required. |
| `initiator` | string | `keymon`, `api`, or `system`. Diagnostic. Required. |
| `timeout_ms` | int | Ack deadline, 0–5000. `0` or absent means the server default `limits.sleep_ack_ms` (1500). |

The response is sent **after** the handshake resolves:

```json
{
  "ok": true,
  "decision": "proceed",
  "sleep_id": "9f3c1a2b4d5e6f70",
  "subscribers": 1,
  "acks": 1,
  "inhibit": null,
  "defer_until_ms": 0,
  "detail": "all subscribers ready"
}
```

```json
{
  "ok": true,
  "decision": "defer",
  "sleep_id": "9f3c1a2b4d5e6f70",
  "subscribers": 1,
  "acks": 0,
  "inhibit": {"reason": "playing_video", "detail": "streaming", "until_ms": 360000},
  "defer_until_ms": 360000,
  "detail": "deferred by the application"
}
```

- The server MUST NOT respond `proceed` while a `sleep.inhibit` with a valid
  `sleep_id` for this attempt is active.
- `subscribers` counts subscriptions with the `sleep` feature that were present
  when the prepare started, including one that received the replay in §5.4.
- `acks` counts distinct subscribers that answered `sleep.ack`.
- If `subscribers == 0`, the server responds `proceed` immediately. This is the
  unadapted-app and desktop case and is the backward-compatible path.
- If a prepare is already pending, or the phase is `sleeping`, the server
  answers `{"ok":false,"error":"sleep_busy",...}` and does not start a second
  handshake.
- The caller MUST NOT suspend before receiving `decision:"proceed"`.
- The caller MUST call `sleep.finished` after resume or failure. If the caller
  never does (crash, power loss with a stale in-memory phase), the next
  `sleep.request` finalizes the old cycle as `result:"unknown"`, emits a
  `wake.done` with `woke_by:"unknown"`, and then starts the new attempt. A
  `sleep.status` whose `phase` is `sleeping` for more than 24 h reports
  `awake` and `last.result:"unknown"`.

**Timing.** The server emits `sleep.prepare` to every subscriber, then waits
until either all counted subscribers acked, or one inhibited, or the deadline
expires. A missing ack never blocks sleep forever: at the deadline the server
proceeds unless an inhibit is active. This is a deliberate liveness guarantee
for a crashed or wedged app.

**Server concurrency.** `devserv` must be able to serve `sleep.ack` /
`sleep.inhibit` requests while a `sleep.request` is waiting. The current
`accept → handle one line → close` loop cannot do this; phase B must move the
server to a multiplexed loop (see §13). A request connection that is waiting
inside `sleep.request` does not block other connections.

### 6.3 `sleep.prepare` (event)

```json
{
  "event": "sleep.prepare",
  "seq": 7,
  "at_ms": 120500,
  "session_id": "0123456789abcdef",
  "sleep_id": "9f3c1a2b4d5e6f70",
  "reason": "user",
  "deadline_ms": 1500,
  "deadline_at_ms": 122000,
  "battery": {"available": true, "percent": 87, "charging": false, "usb": false}
}
```

- `sleep_id` is 16 lowercase hex digits, generated per attempt and unique for
  the server's lifetime. It is the stale guard for acks and inhibits.
- `deadline_ms` is the remaining time at send; a replayed prepare carries the
  remaining time, not the original.
- `battery` is a convenience snapshot for the app's save UI; it may be absent
  when the battery feature is unavailable.

### 6.4 `sleep.ack`

```json
{"op":"sleep.ack","session_id":"0123456789abcdef","sleep_id":"9f3c1a2b4d5e6f70",
 "paused":true,"saved":false,"detail":"video paused at 00:14:32"}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `sleep_id` | string | Required. Must equal the pending attempt. |
| `paused` | bool | The app has paused playback/gameplay. Informational. |
| `saved` | bool | The app has flushed its save state. Informational. |
| `detail` | string | Optional, ≤ 160 bytes, diagnostic only. |

Response:

```json
{"ok":true,"sleep_id":"9f3c1a2b4d5e6f70","result":"accepted",
 "decision":"proceed","detail":"last subscriber"}
```

- `result` is `accepted`. `decision` is the aggregate at the moment the ack was
  processed: `proceed` when every counted subscriber has acked and no inhibit is
  active, otherwise `defer` (another subscriber inhibited, or some subscribers
  are still pending). The ack itself succeeded either way.
- A second ack from the same subscription for the same `sleep_id` is
  idempotent and returns the current decision.
- Wrong/expired/superseded `sleep_id` → `sleep_stale`, no state change.
- Session mismatch → `session_stale`.
- No active session: allowed for a system-scope subscription; the request must
  omit `session_id`. If `session_id` is supplied and no session is active →
  `session_stale`.
- Acking after the deadline has passed and the system proceeded →
  `sleep_stale`.
- `paused`/`saved` are recorded in `session-status` and devserv logs; they are
  never used to decide whether sleep is safe.

### 6.5 `sleep.inhibit`

Two forms share one operation.

**Prepare-scoped inhibit** (defers the current attempt):

```json
{"op":"sleep.inhibit","session_id":"0123456789abcdef",
 "sleep_id":"9f3c1a2b4d5e6f70","reason":"playing_video",
 "timeout_ms":60000,"detail":"watching a stream"}
```

**Automatic-sleep pre-inhibit** (no `sleep_id`; suppresses future automatic
initiation):

```json
{"op":"sleep.inhibit","session_id":"0123456789abcdef",
 "reason":"playing_video","timeout_ms":60000}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `sleep_id` | string | Optional. Present: defer this prepare. Absent: pre-inhibit. |
| `reason` | string | Required, 1–64 bytes, a short token (`playing_video`, `recording`, `saving`, `update`, …). |
| `timeout_ms` | int | Required, 1000–`limits.inhibit_max_ms` (300000). Out of range → `bad_request`; no silent clamp. |
| `detail` | string | Optional, ≤ 160 bytes, diagnostic only. |

Response:

```json
{"ok":true,"scope":"prepare","sleep_id":"9f3c1a2b4d5e6f70",
 "decision":"defer","inhibit_active":true,"expires_at_ms":360500,
 "reason":"playing_video"}
```

```json
{"ok":true,"scope":"automatic","sleep_id":null,
 "decision":"suppressed","inhibit_active":true,"expires_at_ms":360500,
 "reason":"playing_video"}
```

Semantics:

- **Prepare-scoped**: the current attempt is deferred immediately. The system
  must not suspend for that attempt. For `reason:"idle"` the system may retry
  automatically after `expires_at_ms`; for `reason:"user"` the user must press
  power again (the system does not silently re-sleep after an explicit request
  was deferred). The app can refresh by sending `sleep.inhibit` again with the
  same `sleep_id` while the attempt is still pending; after the server has
  answered `defer`, the attempt is over and a retry needs a new
  `sleep.prepare`.
- **Automatic pre-inhibit**: suppresses *automatic* sleep initiation
  (`reason:"idle"`, and a future low-battery policy if the system chooses) until
  `expires_at_ms`. It never suppresses an explicit user sleep request: a user
  pressing power still produces `sleep.prepare`, and the app can then inhibit
  that prepare if it wants. This is what keeps the power key responsive.
- Only one inhibit lease is tracked per session; a second `sleep.inhibit`
  replaces the reason/detail and refreshes the expiry. An inhibit without a
  session is `not_active` (pre-inhibits belong to an app session).
- An inhibit lease is cleared by: expiry, `sleep.release`, session end, or a
  newer inhibit for another attempt (prepare-scoped only). A crashed app is
  covered by the hard timeout, bounded by 300 s.
- Inhibiting after the system already proceeded → `sleep_stale`.
- A `sleep.inhibit` with a `sleep_id` from an older attempt that has already
  resolved → `sleep_stale`; it never affects a newer attempt.

### 6.6 `sleep.release`

```json
{"op":"sleep.release","session_id":"0123456789abcdef","sleep_id":"9f3c1a2b4d5e6f70"}
```

- With `sleep_id`: withdraw this app's inhibit for that pending prepare, if it
  is still pending. If the system already resolved the attempt, `released` is
  `false`; the attempt is not revived.
- Without `sleep_id`: drop the automatic pre-inhibit lease.
- Response: `{"ok":true,"released":true,"scope":"prepare"}`,
  `scope` ∈ `prepare`, `automatic`, `none`.
- `sleep.release` is idempotent; releasing when nothing is held returns
  `released:false` and `ok:true`.

### 6.7 `sleep.finished` (system operation)

```json
{"op":"sleep.finished","sleep_id":"9f3c1a2b4d5e6f70",
 "result":"ok","woke_by":"power","detail":"resumed after 61 s"}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `sleep_id` | string | Required; the attempt returned by `sleep.request`. |
| `result` | string | `ok` or `failed` (suspend attempted but failed/aborted). |
| `woke_by` | string | `power`, `lid`, `timer`, `unknown`. Required. |
| `detail` | string | Optional, ≤ 160 bytes. |

Response: `{"ok":true,"sleep_id":"...","slept_ms":61000}`.

- The server computes `slept_ms` from its monotonic clock between the
  `proceed` decision and this call.
- On success the server emits `wake.done` to subscribers, then returns to
  `phase:"awake"`.
- Unknown/stale `sleep_id` → `sleep_stale`, no `wake.done`.
- Calling `sleep.finished` twice for the same attempt → the second is
  `sleep_stale`.
- This operation exists so that `devserv` remains the single event emitter and
  every subscriber sees one consistent `wake.done` per cycle.

### 6.8 `wake.done` (event)

```json
{
  "event": "wake.done",
  "seq": 12,
  "at_ms": 181000,
  "session_id": "0123456789abcdef",
  "sleep_id": "9f3c1a2b4d5e6f70",
  "slept_ms": 61000,
  "reason": "user",
  "woke_by": "power",
  "result": "ok",
  "battery": {"available": true, "percent": 86, "charging": false, "usb": false}
}
```

- Exactly one `wake.done` per completed cycle, sent after `sleep.finished`.
- `result:"failed"` still sends `wake.done` so apps resume their normal loop.
- The session that was active when the cycle started owns `session_id`. If the
  session ended during sleep (for example the launch parent killed the app),
  the event is not delivered to any subscription; the next app will not see it.
  Apps must treat "no session" at startup as normal.
- Apps SHOULD resume rendering/playback immediately; they have already paused
  on `sleep.prepare` (or chosen to inhibit).

### 6.9 `sleep.status`

```json
{"op":"sleep.status"}
```

```json
{
  "ok": true,
  "service": "devserv",
  "phase": "awake",
  "at_ms": 181100,
  "pending": null,
  "inhibit": null,
  "last": {
    "sleep_id": "9f3c1a2b4d5e6f70",
    "reason": "user",
    "result": "ok",
    "woke_by": "power",
    "started_at_ms": 120000,
    "ended_at_ms": 181000,
    "slept_ms": 61000
  }
}
```

- `phase` ∈ `awake`, `preparing`, `sleeping`.
- `pending` is non-null only in `preparing`:
  `{"sleep_id":"...","reason":"user","deadline_ms":800,"deadline_at_ms":122000,"subscribers":1,"acks":0}`.
- `inhibit` is non-null while a lease or prepare-scoped inhibit is active:
  `{"scope":"prepare|automatic","reason":"playing_video","expires_at_ms":360000,"sleep_id":"..."}`.
- `last` is the most recent completed or failed cycle, or `null`.
- This operation is how a late client resynchronizes after a `seq` gap or a
  reconnect.

---

## 7. Overlay hosting

### 7.1 Declaration and ownership

- The app declares `"overlay"` in the **top-level** `pak.json`
  `"capabilities"` array (`"capabilities": ["overlay"]`). This is the existing
  `PAK_CAPABILITY_OVERLAY` (verified in `pak_manifest.c`; `Clock.pak` is the
  in-tree example), not the C1 `device.capabilities` list. There is no new key.
- `session-start` records the declaration and starts with
  `overlay_host = overlay_host_declared = true`. The app cannot claim it in the
  request; the manifest is authoritative, and `overlay.host` can only yield and
  restore within it (§7.9).
- For an adapted session, `app-action action:"overlay"` returns
  `handled_by_app:true`, records no exit, and keymon draws and grabs nothing.
  This is unchanged from C4.
- This declaration is the **permission** to host; the effective host can be
  yielded and restored at runtime with `overlay.host` (§7.9), for apps that can
  only draw in some modes (Moonlight: menu draws, streaming cannot).
- **New**: devserv also emits `overlay.home` to the session's subscribers for
  that press. This gives framework-agnostic apps a HOME signal even if they do
  not read `KEY_HOMEPAGE` themselves. Apps that do read the key can ignore the
  event; both carry the same `at_ms`.

```json
{"event":"overlay.home","seq":9,"at_ms":120500,"session_id":"0123456789abcdef",
 "action":"home"}
```

- For an unadapted session, `app-action overlay` returns
  `handled_by_system:true` and keymon opens the system panel; no `overlay.home`
  event is emitted. The system fallback is unchanged.

### 7.2 What the app must draw

The app owns the whole HOME panel. It should reproduce the shared rows from the
same data the system overlay uses:

| Row | Data | Source in NextUI |
| --- | --- | --- |
| Battery | `percent`, `charging`, `usb` | `PLAT_getBatteryStatusFine`, `PLAT_isUSBConnected` |
| Wi-Fi | `enabled`, `connected`, `strength` (`off`/`disconnected`/`low`/`med`/`high`), `ssid` | `WIFI_enabled`, `PLAT_connectionStrength`, `WIFI_connectionInfo` |
| Bluetooth | `enabled`, `connected` | `PLAT_bluetoothEnabled`, `PLAT_btIsConnected` |
| Volume | `level`, `min`, `max`, `mute`, `mute_supported`, `sink`, `locked` | `GetVolume`, `GetMute`, `GetAudioSink`, mute profile |
| Brightness / colour temperature | `brightness`, `colortemp`, min/max, `brightness_locked`/`colortemp_locked` | `GetBrightness`, `GetColortemp`, mute profile |
| Theme (optional) | seven `#RRGGBB` colours and the clock/battery-display preferences | `CFG_getColor`, `CFG_getShowBatteryPercent`, `CFG_getShowClock`, `CFG_getClock24H` |

`status.get` returns this data in one request; `status.snapshot` /
`status.changed` push it. The app does not need to call five capabilities and
stitch together inconsistent values.

`locked` mirrors the system HUD's lock icon: it is true when mute is active and
the corresponding muted override is not `SETTINGS_DEFAULT_MUTE_NO_CHANGE`, i.e.
the user cannot change that value right now.

The owner has chosen **full fidelity**: the panel, top-right hardware row,
bottom hint bar, and HUD must match the system overlay pixel-for-pixel — the
same `UIOverlay`/`GFX` sources, asset atlas, font, theme, and reference scale.
The recommended way to get that is the shared optional renderer in §7.10; an
app that vendors the same NextUI sources and assets is equally conformant. The
lean data-only route remains possible for apps that do not need full fidelity,
but it is not the target. The visual reference is the "全局浮层界面方案"
artifact: centred panel, system hardware row top-right, bottom hint bar,
dimmed app frame.

### 7.3 Input ownership

- While the app's overlay is open, the app owns navigation input (d-pad, A/B,
  HOME). keymon does not draw, does not grab, and does not act on d-pad/A/B for
  an adapted session.
- **System keys stay with keymon.** Volume, brightness, colour temperature and
  mute are owned by keymon: it reads them with `EVIOCGKEY`, applies the change
  through msettings, and asks `system-feedback` who draws. An app MUST NOT also
  apply those keys, even if it has grabbed the evdev node: keymon's
  `EVIOCGKEY` query keeps working while another process holds `EVIOCGRAB`
  (this is stated in `keymon.c`), so handling them in both places would
  double-apply. The app observes the change through the `feedback` event and
  draws the HUD.
- HOME reaches an adapted app through its normal input path (`KEY_HOMEPAGE`
  via PAD/evdev) and/or the `overlay.home` event. Do not consume it twice.
- The app may hold `EVIOCGRAB` on the nodes it needs for its own overlay, as
  the B5 host does. It must release it on every close/exit path (the existing
  overlay contract's rule). The system does not grab for an adapted session.

### 7.4 `overlay.state`

The app reports whether its in-process overlay is open. This is not required
for drawing, but it lets the HUD policy behave correctly and gives the system
an honest view of the session.

```json
{"op":"overlay.state","session_id":"0123456789abcdef","open":true}
```

```json
{"ok":true,"service":"devserv","session_id":"0123456789abcdef",
 "overlay_host":true,"overlay_open":true}
```

- `open` is required and boolean. Unknown fields ignored.
- No active session → `not_active`. Session not adapted → `not_adapted`. A
  session that declared the overlay but currently yields it → `not_hosting`
  (§7.9).
- While `overlay_open` is true (and the effective host is true), all HUD kinds
  route to the app regardless of `hud.policy` (the HUD would otherwise be
  drawn behind/under the panel). While the effective host is false, they route
  to the system regardless (§7.9.2). The
  `feedback` event includes `"overlay_open":true` so the app can integrate it
  into the panel.
- On session end the state resets to closed. A client that reconnects should
  call `overlay.state` again if its overlay is still open.
- `status.get` includes `overlay_open` for the active session.

### 7.5 HUD cooperation

`hud.policy` sets who draws each kind for this session:

```json
{"op":"hud.policy","session_id":"0123456789abcdef","mode":"app",
 "kinds":["volume","brightness"]}
```

- `mode` is `app` or `system`. `kinds` is optional; default all four
  (`volume`, `brightness`, `colortemp`, `mute`). Unknown kinds → `bad_request`.
- With no `mode`, the operation reads the current policy.
- Response:

```json
{"ok":true,"service":"devserv","session_id":"0123456789abcdef",
 "overlay_open":false,"overlay_host":true,
 "policy":{"volume":"app","brightness":"app","colortemp":"system","mute":"system"},
 "effective":{"volume":"app","brightness":"app","colortemp":"system","mute":"system"}}
```

- Defaults: a session that declared `"overlay"` starts with all four `app`
  (consistent with C4's `handled_by_app`); an unadapted session starts with all
  four `system` (C4's `handled_by_system`). HUD app drawing requires the
  effective host: an app that cannot render in-process (unadapted, or currently
  yielded) always gets `system`, and `hud.policy` responses report that in
  `effective`. The effective routing is further
  gated by the dynamic host (§7.9): a declared session that yielded hosting
  gets `system` until it restores it. An unadapted app may still opt into
  `app` for HUDs; the policy is independent of HOME ownership.
- `system-feedback` is keymon's ownership query, not an app request; the SDK
  does not expose it. The app's input is the resulting `feedback` event.
- `system-feedback` consults the effective host and the policy: `app` →
  `handled_by_app:true`, `system` → `handled_by_system:true`.
  `overlay_open:true` forces `app` (only while the effective host is true).
- When the result is `app`, devserv emits a `feedback` event for the change.
  keymon draws nothing. When it is `system`, keymon draws its existing 1.6 s
  HUD and extends the B5 overlay lifecycle; no `feedback` event is sent.
- The policy is per session and resets on session start. Changes are immediate.

### 7.6 `feedback` (event)

```json
{
  "event": "feedback",
  "seq": 10,
  "at_ms": 121000,
  "session_id": "0123456789abcdef",
  "kind": "volume",
  "value": 8,
  "min": 0,
  "max": 20,
  "mute": false,
  "sink": "default",
  "locked": false,
  "overlay_open": false,
  "source": "key"
}
```

- `kind` ∈ `volume`, `brightness`, `colortemp`, `mute`.
- `value` is the new level; for `mute` it is `0` or `1` and `min`/`max` are
  `0`/`1`.
- `sink` is present for `volume` (`default`, `bluetooth`, `usbdac`).
- `locked` is the mute-profile lock described in §7.2.
- `source` is a hint: `key` when a `system-feedback` request for this kind
  arrived within the last 250 ms (the keymon path), otherwise `change` (a
  sampled change, which may be the app itself or another process). Apps should
  compare with their own last value if they want to ignore self-caused
  changes.
- Cadence: at most one `feedback` per kind per
  `limits.feedback_min_interval_ms` (default 100 ms), always carrying the
  latest value, with a trailing event when a burst ends. This matches keymon's
  100 ms key-repeat cadence.
- Feedback is sent only to subscribers with the `hud` feature on the session
  whose policy is `app` (or whose overlay is open). A subscriber that opts out
  sees no event; the system does not draw either — the app chose to own it.

### 7.7 Data flow, end to end

1. `pak.json` declares `"overlay"`; `launch.sh` calls `session-start`; devserv
   records the declaration and starts the effective host at `true`.
2. The app calls `hello`, gets `session.session_id` and `overlay_host:true`.
3. The app calls `subscribe` with `sleep`, `status`, `hud`, `overlay`; it
   receives `subscription.ready` + `status.snapshot`.
4. The app renders its normal UI and keeps the latest `NextuiStatus`.
5. The user presses HOME. keymon reads `KEY_HOMEPAGE` (`EVIOCGKEY`), sends
   `app-action overlay` with `at_ms`. devserv returns `handled_by_app` and emits
   `overlay.home`. keymon draws and grabs nothing.
6. The app opens its overlay, calls `overlay.state open:true`, draws the system
   rows from the snapshot and its own rows, and grabs input if it needs to.
7. Volume changes: keymon changes msettings and asks `system-feedback`. With
   the default `app` policy (and/or `overlay_open`), devserv returns
   `handled_by_app` and emits `feedback`; the app draws the HUD. keymon draws
   nothing.
8. The app closes the overlay, calls `overlay.state open:false`, releases its
   grab, and resumes gameplay.
9. Battery/Wi-Fi/Bluetooth/volume changes arrive as `status.changed` patches;
   the app updates any visible row.

### 7.8 Toggles

The shared Wi-Fi and Bluetooth rows are *data* in this contract. Changing them
uses the existing C1 capability path (`call net.wifi set`, `call net.bluetooth
set`). Those capability names are registered in `device_caps.c` but currently
have no backend, so they answer `unavailable`. Phase B must decide whether to
wire them (the system overlay calls `WIFI_enable`/`BT_enable` directly) or to
leave toggles to the app's own platform code. The SDK exposes a generic
`nextui_app_call()` so an app can use either path once a backend exists. Until
then, an app that must toggle should do so through its own platform binding and
refresh `status.get`.

### 7.9 Dynamic overlay ownership

Static `"overlay"` declares that an app is **capable** of hosting. Some apps can
only draw during part of their lifetime. Moonlight is the concrete case: the
Apostrophe wrapper hosts the panel in its menu, but `stream_flow()` calls
`ap_quit()` and the separate `moonlight` engine owns fb0 during streaming, so
the wrapper cannot draw in-process. The effective host can therefore change at
runtime.

This is a **downgrade/restore within the static permission**, not a way to claim
hosting an app did not declare.

#### 7.9.1 `overlay.host`

```json
{"op":"overlay.host","session_id":"0123456789abcdef","host":false,
 "detail":"streaming; SDL is down"}
```

| Field | Type | Meaning |
| --- | --- | --- |
| `host` | bool | Required. `true` restores hosting; `false` yields it. |
| `session_id` | string | Required. The active generation. |
| `detail` | string | Optional, ≤ 160 bytes, diagnostic. |

Response:

```json
{"ok":true,"service":"devserv","session_id":"0123456789abcdef",
 "overlay_host":false,"overlay_host_declared":true,
 "host_changed_at_ms":123456,"overlay_open":false,"effective_hud":"system"}
```

- `overlay_host` is the **effective** value; `overlay_host_declared` is the
  static manifest permission. The session response and `hello` use the same two
  fields; existing clients that only read `overlay_host` keep working and see
  the effective routing.
- `host:true` requires `overlay_host_declared`; otherwise `not_adapted`.
  `host:false` is valid for any active session (it is a no-op for an app that
  never declared the overlay).
- No active session → `not_active`. Session id mismatch → `session_stale` and
  nothing changes.
- Idempotent: setting the current value returns `ok` and does not change
  `host_changed_at_ms`.
- The effective host starts equal to the declaration at every `session-start`;
  it is runtime state of that session, not a manifest mutation. A new session
  never inherits a previous session's `false`.
- On a transition to `false`, devserv closes any open overlay state
  (`overlay_open:false`) and records the change. The app MUST close its panel
  and release its input grab before sending `host:false`; devserv's reset is a
  safety net, not a substitute.
- On a transition to `true`, the stored `hud.policy` applies again.

#### 7.9.2 Routing while the effective host is false

| Path | Result |
| --- | --- |
| `app-action action:"overlay"` | `handled_by_system:true`, no `overlay.home` event. keymon opens the C4 fallback panel (disp2 layer). |
| `system-feedback` | `handled_by_system:true` for every kind, regardless of `hud.policy`. keymon draws the HUD. |
| `feedback` events | Not sent. The app cannot draw them. |
| `hud.policy` | Accepted and stored; the response's `effective` object shows `system`. When hosting returns, the stored `app` entries are effective again. |
| `overlay.state open:true` | `not_hosting` (the app declared the overlay but currently yields it). `open:false` is accepted and idempotent. |
| `overlay.home` | Not emitted. |
| Sleep/status/battery | Unaffected. A streaming wrapper can still ack/inhibit sleep and read status. |

The system panel's Resume simply closes the panel; its Exit sends
`app-action home`, which ends the streaming app through the existing launch
parent. This is exactly the C4 fallback, reached dynamically.

#### 7.9.3 Recommended transitions

The app should order the switch so HOME is never routed to a process that
cannot draw:

- **Menu → streaming**: close the panel → `overlay.state open:false` → release
  the input grab → `overlay.host host:false` → tear down SDL → start the
  engine. From the `host:false` acknowledgement onward, HOME goes to the system
  panel (safe over the engine's fb0).
- **Streaming → menu**: rebuild SDL/UI → `overlay.host host:true` → open the
  panel when HOME arrives. Before the acknowledgement, HOME stays with the
  system panel, which is safe.

`app-action` is routed by the effective host at the moment devserv processes
it; host changes are not retroactive. The ordering above keeps the window
where the app owns HOME but cannot draw as short as possible.

#### 7.9.4 Reconnect and session lifetime

- The effective host is server-side session state. An event-stream reconnect
  does not reset it (the session did not end).
- A new session starts from the declaration again. The SDK remembers the last
  `nextui_app_overlay_set_host()` request in the process and re-applies it after
  a successful `hello` + `subscribe` (best effort); an app that connects
  manually should re-assert it after `hello`.
- Session end resets the effective host and closes the overlay state. A stale
  `overlay.host` from the old generation is `session_stale`.

#### 7.9.5 Degradation

If the server does not implement `overlay.host` (pre-protocol-1 or an
intermediate phase-B build), the SDK returns `unsupported` and the app runs with
the static C4 behavior: an `"overlay"` app is `handled_by_app` for its whole
session. A streaming app that cannot draw MUST then implement the documented
fallback itself — HOME ends the stream and returns to the menu — instead of
leaving the key dead. This is the only supported degradation; the system never
guesses.

---

### 7.10 Full-fidelity rendering component (owner decision)

The owner chose **full fidelity** for the hosted overlay: an in-process panel
must be visually indistinguishable from keymon's system panel and HUD. This
section records the design decision and the recommended sharing model. It is
not an implementation in this repository; the MIT core stays data-only.

#### 7.10.1 Decision

- The app-side panel must reuse the real NextUI drawing code:
  `UIOverlay_draw()` (panel, rows, header, dim; the footer is optional), the software `GFX`
  blitters (`GFX_blitAsset`, `GFX_blitPill`, `GFX_blitBattery`,
  `GFX_blitHardwareGroup`, `GFX_blitHardwareIndicator`), the
  `RES_PATH/assets@2x.png` atlas, the `RES_PATH` font, and the user's
  `minuisettings.txt` theme via `CFG_getColor()`.
- Do **not** reimplement the panel in Apostrophe/borealis primitives and call
  that full fidelity. A reimplementation is a "lean" renderer and will drift.
- The recommended sharing model is an **optional, separately packaged,
  source-level SDL2 renderer component** (`nextui-overlay-render`) that wraps
  those NextUI modules in a small framework-neutral API. It is not part of the
  MIT `libnextui_app` core and must not be added to its default build.
- Apps may instead vendor the same NextUI sources and assets directly. Both
  routes must pass the same golden-image conformance test (§7.10.4).

**Recommendation: use the shared component, not contract-only vendoring.** A
single optional renderer package gives one drawing implementation, parity by
construction, one golden test, and both wiliwili and Moonlight benefit. Vendoring
the same sources per app is the fallback (no shared package, but the same
conformance test). The component must stay outside the MIT core because it
pulls SDL2 and NextUI's rendering code/assets under NextUI's license.

Note that keymon and the app cannot literally share one process or one surface:
keymon presents its panel through the disp2 layer while the app composites into
its own `SDL_Renderer`. "Same code" means the same `UIOverlay`/`GFX` sources,
assets, font, theme, and geometry, with two presentation backends. The shared
component is the app-side presentation adapter.

The existing `workspace/all/common/apostrophe_overlay_host.c` is the reference
for the app-side presentation technique: it calls `GFX_initAttached(width,
height)`, owns a `UIOverlay`, draws it into an offscreen ARGB `SDL_Surface`,
uploads that to an `SDL_Texture`, and composites it into the app's existing
`SDL_Renderer`. It never creates a window, renderer, or GFX display and never
touches the disp2 layer. The shared component generalizes that adapter and
removes the Apostrophe dependency.

#### 7.10.2 Boundary: what is shared and what is app-owned

| Concern | Core SDK (`libnextui_app`, MIT) | Optional renderer (`nextui-overlay-render`) | App |
| --- | --- | --- | --- |
| Protocol, events, sleep/HUD policy, status data | Yes | No | Calls it |
| SDL2 window/renderer/event loop | No | No | Yes |
| Panel/HUD drawing, assets, font, theme | No | Yes (NextUI code) | Chooses the route |
| Input mapping (d-pad/A/B/HOME) | No | Normalized `NextuiOverlayInput` only | Yes (borealis/Apostrophe/SDL) |
| `EVIOCGRAB` / input ownership | No | No | Yes |
| Wi-Fi/Bluetooth/exit actions | No | Calls app callbacks | Yes |
| `overlay.host` / `overlay.state` | Yes | No | Calls it |
| Golden-image conformance | No | Yes (host test) | Must run it |

The renderer is **not** a second compositor: keymon keeps drawing into the
disp2 layer with the same `UIOverlay`/`GFX` code; the app renderer composites
the same drawing into its own SDL renderer. The shared part is the source code
and the visual model, not a cross-process surface.

#### 7.10.3 Proposed component API

The component is SDL2-based and framework-neutral: no Apostrophe, borealis, or
NextUI PAD types in its public header. This is a proposal for the package; the
core SDK does not depend on it.

```c
typedef struct NextuiOverlayRenderer NextuiOverlayRenderer;

typedef enum {
    NEXTUI_OVERLAY_INPUT_UP,
    NEXTUI_OVERLAY_INPUT_DOWN,
    NEXTUI_OVERLAY_INPUT_ACCEPT,
    NEXTUI_OVERLAY_INPUT_CANCEL
} NextuiOverlayInput;

typedef struct {
    const char* button;            /* e.g. "U/D", "A", "B", "Home" */
    const char* hint;              /* e.g. "Select", "OPEN", "Close" */
} NextuiOverlayHint;

typedef struct {
    const char* title;             /* default "System" */
    const char* resume;
    const char* exit_to_desktop;
    const char* wifi;
    const char* bluetooth;
    const char* on;
    const char* off;
    const char* unavailable;
    const char* not_connected;
    /* Resident system footer: two GFX_blitButtonGroup() calls, exactly like
     * keymon's home_footer(). Defaults: left U/D Select + A OPEN,
     * right B Close + Home Close. */
    const NextuiOverlayHint* left_hints;
    int left_hint_count;
    const NextuiOverlayHint* right_hints;
    int right_hint_count;
} NextuiOverlayStrings;

typedef struct {
    int (*wifi_get)(void* user);          /* -1 unavailable, 0 off, 1 on */
    int (*wifi_set)(void* user, int on);
    int (*bluetooth_get)(void* user);
    int (*bluetooth_set)(void* user, int on);
    int (*exit_to_desktop)(void* user);   /* return 1 when accepted */
} NextuiOverlayActions;

typedef struct {
    SDL_Renderer* renderer;        /* the app's existing renderer */
    const char* res_path;          /* NULL: /.system/res */
    const char* font_path;         /* NULL: <res_path>/font.ttf */
    int width, height;             /* system reference size, not the app viewport */
    int close_first;               /* 1 = system resident order (Continue first) */
    const char* app_name;          /* from hello/session */
    const NextuiOverlayStrings* strings;   /* NULL: built-in English */
    const NextuiOverlayActions* actions;   /* NULL: rows unavailable */
} NextuiOverlayRendererConfig;

NextuiOverlayRenderer* nextui_overlay_renderer_create(
    const NextuiOverlayRendererConfig* config);
void nextui_overlay_renderer_destroy(NextuiOverlayRenderer* renderer);

/* Feed the latest status.get/snapshot data before drawing. The renderer uses
 * it for the shared rows and to keep GFX's battery cache in sync. */
void nextui_overlay_renderer_set_status(NextuiOverlayRenderer*,
                                        const NextuiStatus* status);

int  nextui_overlay_renderer_open(NextuiOverlayRenderer*);   /* UIOverlay_open */
int  nextui_overlay_renderer_close(NextuiOverlayRenderer*);
int  nextui_overlay_renderer_is_open(const NextuiOverlayRenderer*);
int  nextui_overlay_renderer_input(NextuiOverlayRenderer*, NextuiOverlayInput);

/* Draw the panel + top-right hardware group. Call after the app frame. */
void nextui_overlay_renderer_draw(NextuiOverlayRenderer*);

/* Draw a HUD at the system position. The app owns the 1.6 s hide timer. */
void nextui_overlay_renderer_draw_hud(NextuiOverlayRenderer*,
                                      const NextuiFeedback* feedback);
```

Implementation notes for the package:

- `create()` calls `GFX_initAttached(width, height)` after the app has
  initialized SDL video, creates a `UIOverlay` with the app's hooks/strings,
  and allocates a texture at the same size. `destroy()` calls
  `UIOverlay_destroy()`/`GFX_quitAttached()` and frees the texture.
- For system parity the component must reproduce keymon's resident panel
  setup, not the adapted-app defaults: `hooks.close_first = 1`;
  `hooks.capabilities = UI_OVERLAY_CAP_EXIT` plus Wi-Fi/Bluetooth only when
  the app supplies those actions (no Save/Resume in the first batch);
  `UIOverlay_setFooterVisible(ui, 0)`; and a `home_footer`-equivalent drawn
  with two `GFX_blitButtonGroup()` calls and the resident key labels. The
  default hint model is `{U/D, Select}, {A, OPEN}` on the left and
  `{B, Close}, {Home, Close}` on the right; pass translated equivalents for
  other locales. The panel text strings should match
  `UIOverlay_defaultStrings()` / `I18N_overlayStrings()`.
- `input()` translates the app's normalized event to `UIOverlay_handleEvent()`
  and returns `UIOverlayResult` (1/2 consumed/closed). It must not use
  `UIOverlay_pollInput()`, because that reads NextUI's `PAD` and would tie the
  component to NextUI's input path. The app owns `EVIOCGRAB`; the renderer
  never grabs.
- `draw()` calls `UIOverlay_draw()` on the offscreen surface, then the
  resident footer. `UIOverlay_draw()` already calls `GFX_blitHardwareGroup()`
  for the top-right battery/Wi-Fi/Bluetooth/clock group; do not call it a second
  time. Upload the surface and `SDL_RenderCopy`; never call
  `SDL_RenderPresent`.
- `open()`/`draw()` should re-apply the theme from `minuisettings.txt` before
  painting (the system overlay does this in `SystemOverlay_applyTheme()`), so a
  theme change while the app is running does not leave a stale panel.
- The HUD path is the system HUD only: `GFX_blitHardwareIndicator()` at the
  standard top-right position, no dim, no panel, no grab. The app hides it
  after 1.6 s.
- The panel must be laid out at the system's reference resolution
  (`FIXED_WIDTH`/`FIXED_HEIGHT`, `FIXED_SCALE`) and the resulting texture
  scaled to the app's viewport. Laying it out at a different resolution
  changes padding/row geometry and breaks parity.
- `set_status()` sets the battery cache (`PWR_setBatteryStatus`) so
  `GFX_blitBatteryAtPosition()` matches the system. Wi-Fi strength and
  Bluetooth state come from the app's NextUI platform/msettings layer, as in
  the system overlay; the panel's row text uses the `NextuiStatus` values.
- The component assumes the app links the NextUI `common` rendering subset
  (`ui_overlay`, the GFX part of `api.c`, `config`, `palette`, `defines`,
  `utils`, the SDL compatibility layer, and the platform library). That
  dependency is the price of exact reuse; it is why the component is not in
  the MIT core.
- One renderer instance per process. `GFX`/`CFG` are process-global; do not
  initialize GFX a second time or use `GFX_flip`/`PLAT_*` video calls.

#### 7.10.4 Visual conformance and the golden test

Full fidelity is a testable property, not a style claim. The component (and
any app that vendors the sources) MUST pass a host golden-image test:

1. Render keymon's system path (`SystemOverlay_showHome`/`UIOverlay_draw` +
   `GFX_blitHardwareGroup`, and the HUD path) into a malloc-backed ARGB surface
   at the reference size, using the same `minuisettings.txt`, assets, and font.
   `workspace/all/common/tests/system_overlay_test.c` is the existing pattern.
2. Render the app renderer with the same model: app name, selected row,
   Wi-Fi/Bluetooth states, battery/charging, volume, theme.
3. Assert zero differing pixels for the dim wash, panel (header, rows, icons,
   text, selection), top-right hardware group (drawn by `UIOverlay_draw`), the
   resident footer/hint bar, and the HUD; separately assert the HUD's 1.6 s
   lifecycle is app-driven.
4. Run the test for at least the default theme and one custom theme.

A pixel mismatch is a failure. "Looks close" is not full fidelity. The test is
host-only; real disp2 composition and the app's SDL backend still need device
verification (phase C/D).

#### 7.10.5 Versioning and pinning

- The component must be built from the **same NextUI commit** as the system
  overlay it is meant to match. Pin it as a submodule (or a vendored snapshot)
  and record `NEXTUI_OVERLAY_RENDERER_VERSION` plus the NextUI commit in the
  build.
- A panel drawn by a different NextUI commit is not conformant even if the API
  is unchanged; the assets, font, theme colours, or geometry may have moved.
- Protocol 1 does not negotiate rendering. If a future system wants to expose
  a diagnostic, an optional `hello.ui` object (renderer name, version, scale,
  reference width/height) may be added; clients compare it and log a mismatch,
  never fail the handshake. This is not required for protocol 1 and is not
  part of the core SDK API today.

#### 7.10.6 Licensing (hard constraint, owner decision)

NextUI is licensed under **PolyForm Noncommercial License 1.0.0**
(`4fuu/NextUI/LICENSE`), not MIT or GPL. This SDK is MIT. Consequences:

- The MIT core MUST NOT bundle NextUI's `ui_overlay`, `GFX`, assets, font, or
  other NextUI code. Doing so would relicense the core and conflict with its
  MIT distribution.
- A shared renderer that reuses those sources must be a **separate package**
  under NextUI's license (or another license the owner chooses), consumed by
  apps that already ship in the NextUI ecosystem. It cannot be merged into
  `libnextui_app` as MIT code.
- GPLv3 apps (wiliwili) linking a PolyForm-NC renderer raise a GPL
  compatibility question. The owner must confirm the distribution posture;
  this contract cannot decide it.
- If a purely MIT renderer is required, it must be a clean-room
  reimplementation with recreated assets; pixel parity with the original
  assets is then not guaranteed. That is a different project, not a refactor
  of the shared component.

Recommended packaging: a separate repository
`nextui-overlay-render` (or a clearly separated `render/` subtree with its own
LICENSE and opt-in build) that pins NextUI as a submodule and produces a static
library. The core `nextui-app-sdk` stays MIT/libc-only; wiliwili and Moonlight
add the renderer as a submodule/dependency in their paks. If the owner prefers
one repository, the renderer subtree must not be built by default and must
carry its own license file.

#### 7.10.7 Why not contract-only, and why not in the core

| Option | Result |
| --- | --- |
| Contract data + visual spec, every app vendors | No license mixing, but two copies of the panel/GFX/asset code and guaranteed drift; fixes twice. Acceptable only as a fallback. |
| Shared component in the MIT core | Rejected: SDL2 + NextUI code/assets + PolyForm NC relicense the core and break its "libc-only, MIT" promise. |
| Shared optional component, separate package (recommended) | One drawing implementation, pixel parity by construction, one golden test, both wiliwili and Moonlight benefit; cost is the NextUI/SDL dependency and the license boundary. |
| Clean-room MIT renderer | Only if MIT distribution is mandatory; cannot guarantee pixel parity with NextUI's assets. |

The component stays framework-neutral at its API (SDL2 + normalized input +
`NextuiStatus`/`NextuiFeedback`); the UI framework binding is a thin layer in
each app.

---

## 8. Status and battery

### 8.1 `status.get`

```json
{"op":"status.get","fields":["battery","wifi","bluetooth","volume","display","theme","clock"]}
```

- `fields` optional; default all. Unknown names are ignored.
- No session required. `session_id` is accepted and validated when present, for
  consistency with other ops, but is not needed.

Response (all requested sections; a section that cannot be read is present with
`available:false` and a `detail`):

```json
{
  "ok": true,
  "service": "devserv",
  "at_ms": 121000,
  "overlay_open": false,
  "battery": {"available": true, "percent": 87, "charging": false, "usb": false},
  "wifi": {"available": true, "enabled": true, "connected": true,
           "strength": "high", "ssid": "HomeNet"},
  "bluetooth": {"available": true, "enabled": true, "connected": false,
                "name": ""},
  "volume": {"available": true, "level": 8, "min": 0, "max": 20,
             "mute": false, "mute_supported": true,
             "sink": "default", "locked": false},
  "display": {"available": true, "brightness": 5, "brightness_min": 0,
              "brightness_max": 10, "brightness_locked": false, "colortemp": 20,
              "colortemp_min": 0, "colortemp_max": 40, "colortemp_locked": false},
  "theme": {"available": true, "main": "#F5F5FA", "accent": "#2257FF",
            "accent2": "#3A3A44", "list_text": "#F5F5FA",
            "list_text_selected": "#FFFFFF", "hint": "#9E9EAA",
            "background": "#141419"},
  "clock": {"show": true, "hour24": true}
}
```

- **Availability is honest.** `available:false` plus `detail` when the platform
  or settings path is absent. The desktop preview must report battery, volume,
  display, and theme `available:false` (the desktop battery function is a stub
  returning 1/100 and msettings is a no-op); it must not fabricate a reading.
- `percent` is `PLAT_getBatteryStatusFine`'s `charge` (0–100). `charging` is its
  `is_charging` (charger present and the platform says charging). `usb` is
  `PLAT_isUSBConnected` (the UDC is enumerated by a computer), which is a
  different signal from "charger present" on tg5040.
- `wifi.strength` is the `SIGNAL_STRENGTH_*` enum as a string; `off` means the
  radio is off or there is no connection, `disconnected` means associated but
  no IP/rssi, and `low`/`med`/`high` are the rssi buckets. `ssid` and
  `bluetooth.name` are optional; they are empty when the platform cannot
  provide them.
- `volume.sink` and `volume.mute_supported` mirror the C1 `audio.volume`
  response.
- `locked` (volume) and `display.brightness_locked` / `display.colortemp_locked`:
  see §7.2. They mirror the system HUD's lock icon when the mute profile
  overrides that value.
- `theme` colours are the user's `minuisettings.txt` values (`CFG_getColor`),
  formatted `#RRGGBB`. This is optional data for visual parity; an app may
  ignore it.
- `clock.show` is `CFG_getShowClock`; `clock.hour24` is `CFG_getClock24H`. The
  app formats the time itself (no formatted string is sent).
- Values may be cached briefly: battery up to 1 s, network up to 2 s,
  volume/display up to 250 ms. `at_ms` is the response time.

### 8.2 `battery.status`

```json
{"op":"battery.status"}
```

```json
{"ok":true,"service":"devserv","at_ms":121000,
 "battery":{"available":true,"percent":87,"charging":false,"usb":false}}
```

- Same battery object as `status.get`.
- No session required.
- A failed platform read is `ok:true` with `available:false` and `detail`; it is
  not a transport error. This is the same "never fake success" rule the
  display/audio backend uses.
- The response must never report `percent` outside 0–100. A platform returning
  an out-of-range value is a `backend_error` with `available:false`, not a
  clamped success.

### 8.3 Push cadence (`subscribe` with `status`)

- Immediately after `subscription.ready`, the server sends `status.snapshot`
  with all requested sections and `"initial":true`.
- The server samples:
  - battery: every 5 s (the same cadence as `PWR_updateBatteryStatus`), and on
    demand for a read op;
  - Wi-Fi/Bluetooth: every 2 s;
  - volume/display/mute: every 250 ms;
  - theme/clock: every 5 s and after a session start.
- `status.changed` is coalesced per subscription at `min_interval_ms` (default
  500, floor 250, max 5000). At most one patch per interval, carrying the
  latest value of every section that changed during it; a trailing patch is
  sent when a burst ends. A change during the interval never bypasses the
  floor.
- Only changed sections are present. `changed` lists their names:

```json
{"event":"status.changed","seq":11,"at_ms":121500,
 "session_id":"0123456789abcdef",
 "changed":["battery","volume"],
 "battery":{"available":true,"percent":86,"charging":false,"usb":false},
 "volume":{"available":true,"level":7,"min":0,"max":20,"mute":false,
           "mute_supported":true,"sink":"default","locked":false}}
```

- A full `status.snapshot` is sent every `limits.status_resync_ms` (default
  30000) regardless of changes, and after any reconnect. This bounds the
  damage of a missed patch.
- An unchanged system sends no `status.changed`; only the 30 s snapshot and the
  15 s heartbeat.
- Battery percent/charging/USB are the only battery fields. There is no
  `low`/`critical` flag in the protocol: thresholds are a UI decision and
  differ between the LED rules and the battery icon.

---

## 9. Error model

All errors use the existing envelope:

```json
{"ok":false,"error":"<stable_code>","detail":"<diagnostic text>",
 "service":"devserv", "...": "optional extra fields"}
```

`detail` is human-readable and MUST NOT be parsed for decisions. Extra fields
(`session_id`, `sleep_id`, `server_protocol`, …) are allowed. Table:

| Code | Meaning | HTTP-ish analogue |
| --- | --- | --- |
| `bad_request` | Malformed JSON, missing/extra/typed-wrong field, invalid id, overlong line. | 400 |
| `unknown_op` | The server does not implement this operation. | 404 |
| `unsupported` | The operation is known but this build/feature is disabled. | 501 |
| `protocol_unsupported` | The client's protocol is below the server's minimum. | 426 |
| `unknown_capability` | C1: capability name not in the registry. | 404 |
| `unavailable` | C1: permitted but hardware/backend absent. | 503 |
| `backend_error` | C1: the backend failed or returned an invalid value. | 500 |
| `session_active` | C3: another session is already active. | 409 |
| `session_stale` | C3/C4: the session id does not match, or the event predates the session. | 409 |
| `not_active` | No application session is active where one is required. | 409 |
| `not_adapted` | `overlay.*` used by a session that never declared the overlay. | 403 |
| `not_hosting` | The session declared the overlay but currently yields it (`overlay.host false`). | 409 |
| `restore_partial` | C3: at least one field restore failed. | 500 |
| `sleep_busy` | A sleep attempt is already preparing or in progress. | 409 |
| `sleep_stale` | Wrong, expired, or already-resolved `sleep_id`; ack/inhibit/finished arrived too late. | 409 |
| `too_many_subscriptions` | `limits.subscribe_max` reached. | 429 |

Notes:

- `unknown_op` is new. A **pre-protocol-1** devserv answers unknown operations
  with `bad_request` and detail `"unknown operation"`. Clients probing `hello`
  MUST map both to "cooperation protocol not supported".
- A `sleep.ack` whose prepare already resolved is `sleep_stale` with a detail;
  it is never silently accepted.
- `unavailable` is a data-availability answer for C1 calls. `status.get` and
  `battery.status` instead return `ok:true` with `available:false` per section,
  because the query itself succeeded.
- A malformed request line (including an event connection that sends a second
  line) always gets exactly one error line and a close, as today.

---

## 10. Complete message index

### Operations (request → one response)

| Op | New? | Connection | Session | Purpose |
| --- | --- | --- | --- | --- |
| `ping` | extended | request | no | Liveness + protocol/features/limits. |
| `hello` | new | request | no (reports) | SDK init: negotiate, discover session, battery. |
| `status` | existing | request | no | C1 capability status. |
| `call` | existing | request | no | C1 capability dispatch. |
| `session-start` | existing | request | — | C3. |
| `session-stop` | existing | request | — | C3. |
| `session-status` | existing | request | no | C3. |
| `app-action` | existing | request | yes | C4 HOME/overlay and exit. Emits `overlay.home` for adapted sessions. |
| `system-feedback` | extended | request | yes (optional) | HUD ownership; now consults `hud.policy`. |
| `app-exit-result` | existing | request | yes | C4. |
| `battery.status` | new | request | no | Battery read. |
| `status.get` | new | request | no | Overlay row data snapshot. |
| `subscribe` | new | event | optional | Open the event stream. |
| `sleep.status` | new | request | no | Handshake state. |
| `sleep.ack` | new | request | optional | App is ready. |
| `sleep.inhibit` | new | request | yes | Defer/suppress sleep. |
| `sleep.release` | new | request | yes | Drop an inhibit. |
| `hud.policy` | new | request | yes | Get/set HUD ownership. |
| `overlay.state` | new | request | yes | Report the in-process overlay open/closed. |
| `overlay.host` | new | request | yes | Dynamically yield/restore in-process overlay hosting. |
| `sleep.request` | new | request | system | Begin a sleep attempt. |
| `sleep.finished` | new | request | system | Report the resume. |

### Events (event connection only)

| Event | Family | Session-scoped | Purpose |
| --- | --- | --- | --- |
| `subscription.ready` | all | optional | Handshake; `seq:0`. |
| `heartbeat` | all | no | Liveness every 15 s. |
| `overflow` | all | optional | Queue overflow; close after. |
| `session.ended` | all | yes | Session ended; close after. |
| `sleep.prepare` | sleep | yes if session | Sleep attempt; ack/inhibit. |
| `wake.done` | sleep | yes if session | Resume. |
| `status.snapshot` | status | no | Full row data (initial + every 30 s). |
| `status.changed` | status | no | Sparse patch. |
| `feedback` | hud | yes | Draw a HUD in-process. |
| `overlay.home` | overlay | yes | HOME was routed to the adapted app. |

---

## 11. Conformance: the happy path

An adapted app, on a device with all features:

1. `ping` → protocol 1, features, limits.
2. `hello` → session active, `overlay_host:true`, battery.
3. `subscribe` → `subscription.ready`, `status.snapshot`.
4. `hud.policy mode:app` (optional; it is the adapted default).
5. If the app cannot draw all the time (Moonlight streaming), call
   `overlay.host host:false` before tearing down its renderer and `host:true`
   after rebuilding it (§7.9).
6. `overlay.state open:true` when the user opens the panel; `false` on close.
7. On `sleep.prepare`: pause/save, `sleep.ack` (or `sleep.inhibit` while
   playing), before the deadline.
8. On `wake.done`: resume.
9. On `feedback`: draw the HUD for the kind.
10. On `status.changed`: update rows.
11. On `session.ended`: stop; the process is about to be torn down by the
    launch parent anyway.

Rendering is out of band: a full-fidelity app uses the shared renderer
(§7.10) or vendors the same NextUI sources, pinned to the system's NextUI
commit. Protocol 1 does not negotiate rendering.

An unadapted app, or an app that never connects:

- `app-action overlay` → `handled_by_system:true`, keymon panel.
- `system-feedback` → `handled_by_system:true`, keymon HUD.
- `sleep.request` with no subscribers → `proceed` immediately.
- Nothing else changes.

---

## 12. Backward compatibility and degradation

| Situation | Behaviour |
| --- | --- |
| Old devserv (pre-protocol-1), new SDK | `hello` → `bad_request`/`unknown operation`; SDK `connect` returns `NEXTUI_APP_ERR_UNSUPPORTED`; the app must fall back to running without the SDK. `ping`, `call`, sessions, `app-action` and `system-feedback` keep working exactly as before. |
| New devserv, old app / old nextdev | Unchanged C1/C3/C4 wire shapes. Additive `ping` fields are ignored. No events are subscribed, so no event traffic. |
| Unadapted app (no `"overlay"` capability) | `overlay_host:false`; `overlay.state` → `not_adapted`; HUD defaults to `system`; HOME goes to the system panel. Sleep and status still work. |
| No active session | `hello.session.active:false`; `sleep.status`/`battery.status`/`status.get` work; `hud.policy`/`overlay.state` → `not_active`; a system-scope subscription receives sleep/status events without `session_id`. |
| Feature missing (e.g. no `hud`) | The operation returns `unsupported`; the SDK reports `NEXTUI_APP_ERR_UNSUPPORTED` and the app degrades (system draws the HUD). |
| `overlay.host` missing (old/partial server) | SDK returns `unsupported`; static C4 routing remains for the whole session. A streaming app that cannot draw must implement "HOME ends the stream" itself (§7.9.5). |
| `subscribe` rejected (`too_many_subscriptions`) | SDK connect fails with `BUSY`; the app runs without events. |
| Event stream drops | SDK reconnects with backoff, re-runs `hello` + `subscribe`; if the session ended it emits `SESSION_ENDED` and stops. |
| Server restarts | Same as a drop. The session is gone (C3 state is in-memory); the app should expect termination by the launch parent or a failed `session-status`. |
| Desktop preview | Battery/volume/display/theme report `available:false`; sleep has no resident trigger; everything else works for development. |

**The system fallback is never removed.** An app that does not connect, does
not declare `overlay`, or fails to negotiate still gets the C4 panel and HUD.

---

## 13. Phase B integration points (system side)

This section is normative for `4fuu/NextUI`. It names the real functions and
files so phase B can implement the contract without guessing.

### 13.1 devserv: multiplexed loop and subscription registry

The current `main()` accepts one connection, calls `handle_connection()`
(which reads one line, writes one line, closes), and loops
(`workspace/all/devserv/devserv.c`). The current per-connection
`SO_RCVTIMEO`/`SO_SNDTIMEO` is 2 s and `read_request_line()` reads byte by byte.
This cannot host a persistent event connection or serve acks during a sleep
handshake. Phase B must:

1. Replace the blocking `accept` + per-connection handler with a `poll()` loop
   over the listener, request connections, and event connections.
2. Keep the request contract: one line in, one line out, close. A request that
   takes time (`sleep.request`) stays open while the loop continues to serve
   other connections; add a per-request deadline (ack deadline + 500 ms) and a
   `sleep_busy` timeout path.
3. Add an event subscription registry: fd, session scope, enabled feature
   families, status field mask, `min_interval_ms`, last status snapshot, last
   send time, `seq`, bounded queue, and last heartbeat time.
4. Add timers to the loop: heartbeat (15 s), status sampling (5 s / 2 s /
   250 ms / 5 s), status resync (30 s), sleep ack deadline, inhibit expiry,
   `sleep_cooldown_ms` (1 s after a cycle), and request deadlines. `poll()`
   should use the nearest deadline as its timeout.
5. Fan out events from one publisher function so `seq`, `at_ms`, and
   `session_id` are consistent. One event per subscription increments `seq`.
6. On `session-stop`, `session-start`, or service shutdown: emit
   `session.ended` to that session's subscriptions, clear HUD/overlay/inhibit
   state, then close them. On prepare, remove ended subscriptions from the ack
   set so they cannot block sleep.
7. Enforce `limits.subscribe_max` (8) and the per-subscription queue (64).
8. Keep the socket `0600`, `SIGPIPE` ignored, and the existing error response
   for malformed lines.

`device_protocol.{c,h}` needs new parsed fields (`sleep_id`, `mode`, `open`,
`features[]`, `kinds[]`/`fields[]`, `status` object) and captured string arrays.
The parser already skips unknown keys (`json_lite_skip_value`), so old clients
remain compatible. `DEVICE_PROTOCOL_LINE_MAX` stays 8192. Prefer extending
`DeviceRequestMessage` with fixed-size arrays over a second parser.

### 13.2 Session state

`device_session.c` already tracks `overlay_host`
(`device_session_note_overlay_host`) and reports it in the session response.
Phase B adds, either to the session struct or to a devserv-side per-session
record:

- HUD policy per kind (default derived from the declaration),
- effective `overlay_host` (starts at the declaration) and its
  `host_changed_at_ms`; `overlay_host_declared` from the manifest,
- `overlay_open` (reset on session start/end and on `overlay.host false`),
- the active inhibit lease and its expiry,
- the pending sleep ack set and `sleep_id`,
- the last sleep summary (`result`, `woke_by`, `slept_ms`).

C3's snapshot/restore fields are untouched. No new session field is added to
`SESSION-API.md`; these are runtime cooperation state, not device state.

### 13.3 Sleep: where the events are emitted

`devserv` is the only event emitter and the only handshake coordinator.
The suspend path is split into two sides:

**Request/coordinate**

- `keymon` (resident, works while any app is foreground) sends
  `sleep.request` and waits for the decision.
- `api.c`'s `PWR_sleep()` and `PWR_deepSleep()` may call the same coordinator
  when the process is the desktop/system app (no third-party session). They
  MUST NOT call `sleep.request` from a process that is also an SDK subscriber,
  or the handshake deadlocks its own poll loop. The SDK never calls it.
- `PWR_sleep()` currently clears the screen, calls `PWR_enterSleep()`,
  `PWR_waitForWake()`, `PWR_exitSleep()` (api.c). The handshake belongs
  **before** `PWR_enterSleep()`, and `sleep.finished` **after**
  `PWR_exitSleep()` returns. `PWR_update()`'s sleep branch calls `PWR_sleep()`
  and its deep-sleep branch calls `PWR_deepSleep()`; both inherit the
  handshake from the functions they call.
- `PWR_deepSleep()` runs `${BIN_PATH}/suspend` first and falls back to
  `PLAT_deepSleep()` (api.c). The handshake belongs **before** that branch, not
  inside `PLAT_deepSleep()`: `PLAT_deepSleep` is the platform primitive and may
  also be used by other callers.

**Suspend/resume**

- The caller that received `decision:"proceed"` performs the actual suspend
  (existing `PWR_enterSleep`/`PWR_waitForWake`/`PWR_exitSleep`, or the resident
  path), then calls `sleep.finished` with `result` and `woke_by`.
- `devserv` computes `slept_ms`, emits `wake.done`, and returns `phase` to
  `awake`.

**App-local power paths.** An app that links the SDK MUST disable its own power
button/suspend handling (for Apostrophe:
`ap_set_power_handler(false)`) so the only sleep trigger is the system
handshake. A second, app-local suspend path would bypass `sleep.prepare` and
`wake.done`. The legacy `/tmp/stay_awake` flag is not part of this contract;
playing apps should use `sleep.inhibit` instead.

**Where keymon comes in.** keymon currently has no power-key handling:
`discover_system_keys()` watches HOME, volume, brightness, MENU/SELECT and
navigation, and the main loop only acts on those (`workspace/tg5040/keymon/
keymon.c`). Phase B must:

1. Add `KEY_POWER` to discovery and an edge-detected handler (the existing
   `system_key_events` state machine is the right place; do not use a repeat).
2. Send `sleep.request` with `initiator:"keymon"` and `reason:"user"`, using a
   read timeout of at least `sleep_ack_ms + 1000` ms. The existing
   `devserv_request()` uses a 100 ms poll loop and would return before the
   handshake completes; add a long-timeout variant for this call.
3. On `decision:"proceed"`, perform the resident suspend and then
   `sleep.finished`. On `defer`, do nothing (the app is playing).
4. Coalesce repeated power presses with `limits.sleep_cooldown_ms` (1 s) and
   ignore presses while a request is in flight.

**Hard unknown for phase B.** The actual resident suspend while a third-party
app is foreground has no implementation today: the launcher is blocked in
`nextui_session_run`, and `PWR_*` lives in api.c inside the app process. keymon
does not currently know how to perform the platform sleep sequence. Phase B
must choose and document one:

- a small resident `nextui-sleep` helper that performs the platform
  backlight/mute/suspend steps and is invoked by keymon after `proceed`; or
- extend keymon with the platform suspend path (it already links SDL and the
  platform library, but not api.c's PWR state machine).

Until that exists, the handshake is implementable and testable on the host, but
a wiliwili session has no system sleep trigger. This is called out as an open
decision, not assumed.

**Idle/autosleep.** `PWR_update()`'s screen-timeout branch runs only in the
process that owns `PWR_update` (the launcher/system app). While a third-party
app is foreground, no resident idle source exists. Phase B decides whether to
add one (keymon idle timer or a devserv timer); until then only user-initiated
sleep (`reason:"user"`) is expected. The contract supports `reason:"idle"` and
pre-inhibits for when it lands.

### 13.4 Battery and status data

Add a status provider in devserv (or a small common module it links) using the
same platform functions the desktop already uses:

- battery: `PLAT_getBatteryStatusFine`, `PLAT_isUSBConnected`;
- network: `WIFI_enabled`, `PLAT_connectionStrength`, `WIFI_connectionInfo`,
  `PLAT_bluetoothEnabled`, `PLAT_btIsConnected`;
- audio/display: `GetVolume`, `GetMute`, `GetAudioSink`, `GetBrightness`,
  `GetColortemp`, mute-profile getters for `locked`;
- theme/clock: `CFG_getColor`, `CFG_getShowBatteryPercent`, `CFG_getShowClock`,
  `CFG_getClock24H`.

Constraints:

- Do **not** initialize msettings from a status query. `devserv` starts before
  the desktop; the display/audio backend has an explicit comment about this
  (`device_backend_display_audio_platform.c`: "A presence/status query must
  therefore stay side-effect free"). Report `available:false` until the shared
  settings segment exists.
- Desktop preview reports battery/volume/display/theme `available:false` and
  must not fabricate. The desktop `PLAT_getBatteryStatusFine` returning 1/100
  is a stub, not a battery.
- Wi-Fi/Bluetooth status needs devserv to link the platform path (or a small
  provider). If that dependency is not acceptable, report those sections
  `available:false` and let the app use its own binding. Do not scrape sysfs
  ad hoc.
- The existing `net.wifi` / `net.bluetooth` C1 capabilities remain
  unimplemented. If the app should toggle the rows through C1, phase B wires
  them; otherwise the app owns the toggle. Flagged in §7.8.
- Cache as described in §8.3; never let a read block the poll loop (read sysfs
  values are cheap; `WIFI_connectionInfo` may shell out, so sample it at the
  slow cadence, not per request).

### 13.5 HUD sampling and `system-feedback`

- In the poll loop, sample volume/brightness/colortemp/mute every 250 ms, diff
  against the last values, and emit `feedback` only for sessions whose
  **effective** route is `app` (host true and policy `app`, or overlay open).
- `system-feedback` keeps its exact response shape. Add the effective-host,
  policy, and `overlay_open` checks before choosing `handled_by_app` /
  `handled_by_system`:
  - effective host false → `handled_by_system:true`;
  - effective host true and (`app` policy or overlay open) →
    `handled_by_app:true`;
  - otherwise → `handled_by_system:true`.
  With no session, both are false (existing behavior).
- Mark the last feedback time per kind so `source:"key"` can be set when a
  `system-feedback` request just arrived; otherwise `source:"change"`.
- Do not change the system HUD drawing; the existing keymon path
  (`showIndicator`, 1.6 s, no grab) stays for `system` mode.

### 13.6 `pak.json` and the unadapted fallback

Nothing to add. `pak_manifest.c` already maps the exact capability name
`"overlay"` in the top-level `"capabilities"` array (the Clock pak is the
in-tree example; this is not the C1 `device.capabilities` list). The launcher
and keymon paths are unchanged for unadapted apps. `overlay.host` adds runtime
routing state on top; it must not rewrite the manifest or the session snapshot.
`app-action` and `system-feedback` must consult the effective host on every
request, not just the manifest flag.
The system overlay's panel/HUD drawing, grab lifecycle, and auto-hide stay as
they are; this contract only says *when* keymon uses them.

### 13.7 Host tests for phase B

Mirror the SDK fixture and the existing `run_devserv_test.sh` style: compile
the real `devserv` with fake session/backend ops, drive real Unix sockets, and
assert:

- one request per connection still works and returns one line;
- a second line on an event connection is rejected;
- `subscription.ready` then `seq` 1..N, heartbeat, and session end;
- a sleep prepare with zero subscribers proceeds immediately;
- ack/inhibit paths, deadline behavior, stale `sleep_id`, `sleep_busy`,
  inhibit expiry, and `wake.done` after `sleep.finished`;
- `hud.policy` flipping `system-feedback` between app and system;
- `overlay.state` forcing HUD routing while open;
- `status.changed` coalescing and the 30 s snapshot;
- unadapted `handled_by_system` unchanged;
- the desktop/build with no status backend reports `available:false`, never a
  fake value;
- the full-fidelity renderer passes the golden-image test (§7.10.4) against
  keymon's system path for the panel, hardware group, and HUD.

---

## 14. Security and trust

- The socket is `0600`, owned by the user running devserv (root on the device).
  Every local process can connect as root; there is no per-app authentication.
- The manifest and session identity are descriptive, not authorization. This
  matches C1's default-allow decision and C3's "the id is not a secret".
- `sleep.inhibit` is advisory cooperation, not a privilege: any local process
  could already write `/sys/power/state`; the value is coordination, not
  enforcement.
- The client path is not a sandbox. Do not treat `session_id`, `pak`, or
  `overlay_host` as proof of identity for anything security-sensitive.
- No event contains secrets. Wi-Fi `ssid` is user-visible data; a future
  `status` field must not add credentials.

---

## 15. Limits and defaults

| Name | Default | Range / notes |
| --- | --- | --- |
| `line_max` | 8192 | Hard; includes the newline. |
| `event_queue` | 64 | Per subscription; status/heartbeat dropped first. |
| `subscribe_max` | 8 | Concurrent event connections. |
| `sleep_ack_ms` | 1500 | 0–5000 in `sleep.request.timeout_ms`; 0 means default. |
| `inhibit_max_ms` | 300000 | `sleep.inhibit.timeout_ms` 1000..this. |
| `sleep_cooldown_ms` | 1000 | Between completed cycles (system-side). |
| `heartbeat_ms` | 15000 | Client dead after 3×. |
| `status_min_interval_ms` | 500 | Allowed 250..5000 per subscription. |
| `status_resync_ms` | 30000 | Full snapshot period. |
| `feedback_min_interval_ms` | 100 | Per kind. |
| `session_id` / `sleep_id` | 16 hex | Lowercase hex, exactly 16 chars. |
| `app` | 128 | Fixed in `PAK_MANIFEST_ID_MAX`. |
| `pak` | 512 | Fixed in `DEVICE_GRANT_PATH_MAX` (C1). |
| `detail` | 160 | Diagnostics; longer is truncated by the sender, never split. |

---

## 16. Open decisions and hard unknowns

These are explicitly not decided here; phase B must resolve them and update
this section or the relevant NextUI document. Dynamic overlay ownership itself
is **decided** (protocol 1, §7.9); phase B must implement it. If it slips, the
documented degradation in §7.9.5 applies. The unknowns below are separate:

1. **Resident suspend path** while a third-party app is foreground. keymon has
   no power handling today and api.c's `PWR_*` is not running while the
   launcher waits in `nextui_session_run`. Options in §13.3. This blocks real
   sleep on wiliwili; it does not block the contract or the host tests.
2. **Resident idle/autosleep source.** The screen-timeout branch in
   `PWR_update` is app-local. A resident idle timer is not implemented. Until
   it is, automatic sleep while an app is foreground does not occur; the
   pre-inhibit API is still correct and forward-compatible.
3. **Wi-Fi/Bluetooth status source in devserv** and whether `net.wifi` /
   `net.bluetooth` get/set backends are wired. Without them, rows can be
   displayed but not toggled through C1.
4. **msettings initialization policy** for status reads. The recommendation is
   "never initialize from a status query; report unavailable until the desktop
   has", matching the display/audio backend's stated rule.
5. **HOME visibility.** On Brick Pro HOME is `KEY_HOMEPAGE`, not a joystick
   button. An app that reads it through PAD works; an app that does not gets
   the `overlay.home` event. wiliwili's borealis/evdev path must be checked on
   device; the event is the fallback.
6. **keymon input coexistence** while an adaptive app holds `EVIOCGRAB`.
   `keymon.c` states that `EVIOCGKEY` works under another process's grab, which
   is the basis for "system keys stay with keymon". Device verification is
   still required.
7. **Sleep timing/jitter.** The 1500 ms ack deadline is a product choice
   aligned with the launch-parent grace. On device, measure whether a video
   player can pause within it; if not, raise `sleep_ack_ms` in `limits` rather
   than special-casing apps.
8. **`battery.read` / `power.info`.** They remain registered but
   unimplemented. `battery.status` is the canonical read; do not silently make
   the old names return different shapes.
9. **Renderer packaging and licensing.** The technical route is decided
   (§7.10: shared optional SDL2 renderer, separate package, same NextUI
   commit). The owner must decide the distribution/license posture: separate
   PolyForm-NC package, per-app vendoring, or a clean-room MIT renderer. This
   is the only blocker to publishing the renderer as a reusable package; it
   does not block phase B or the protocol.

---

## 17. Conformance checklist

A system implementation is protocol 1 conformant when:

- [ ] `ping` returns `ok`, `protocol`, `features`, `limits`; old fields are
      unchanged.
- [ ] `hello` reports the active session, `overlay_host`, `matches_pak`, and
      battery; unknown request features are ignored; `protocol_unsupported`
      works.
- [ ] Request connections still do exactly one line in / one line out / close.
- [ ] `subscribe` returns `subscription.ready` with `seq:0`, then events with
      strictly increasing `seq`; a second line on the connection is rejected.
- [ ] Heartbeats at 15 s; a 64-event queue with the documented drop order; a
      third line after overflow closes the connection.
- [ ] `session.ended` on stop/replace, with the old session never receiving new
      session events.
- [ ] A pending `sleep.prepare` is replayed on a new subscription with the
      remaining deadline.
- [ ] `sleep.request` waits for acks/inhibits/deadline, then answers
      `proceed`/`defer`; no subscriber → immediate proceed; concurrent request →
      `sleep_busy`.
- [ ] `sleep.ack`/`sleep.inhibit`/`sleep.release` use the `sleep_id` stale
      guard; a late ack is `sleep_stale`.
- [ ] An inhibit with `timeout_ms` is bounded, expires, and never suppresses an
      explicit user sleep (pre-inhibit is automatic-only).
- [ ] `sleep.finished` emits exactly one `wake.done` and computes `slept_ms`.
- [ ] `sleep.status` reports `phase`, pending, inhibit, and last consistently.
- [ ] `battery.status`/`status.get` report `available:false` on the desktop and
      never fabricate; percent is 0–100.
- [ ] `status.changed` is coalesced at `min_interval_ms`; a full snapshot every
      30 s; battery at 1 s or slower; a 250 ms display/volume sample.
- [ ] `hud.policy` persists for the session; `system-feedback` follows it and
      `overlay_open`; `feedback` is emitted only for `app`.
- [ ] `overlay.state` requires an effective `overlay_host` and resets on session end.
- [ ] `overlay.host` can yield/restore within the static declaration; while
      false, `app-action`/`system-feedback` route to the system, `overlay.home`
      is not emitted, and `overlay.state open:true` is `not_hosting`.
- [ ] The effective host resets to the declaration on a new session; a stale
      `overlay.host` is `session_stale`.
- [ ] An adapted `app-action overlay` still returns `handled_by_app` and emits
      `overlay.home`; an unadapted one still returns `handled_by_system`.
- [ ] The C4 fallback panel/HUD/grab behavior is untouched for unadapted apps.
- [ ] Every session-scoped write validates `session_id`; every sleep write
      validates `sleep_id`.
- [ ] The socket stays `0600`; no new file or secret is introduced.

---

## 18. Changelog

- **v1 (this document)** — first final protocol: event channel (`subscribe`),
  sleep/wake handshake (`sleep.request`/`prepare`/`ack`/`inhibit`/`release`/
  `finished`/`wake.done`/`status`), overlay hosting data (`status.get`,
  `status.snapshot`/`status.changed`, `overlay.home`, `overlay.state`, HUD
  policy/feedback), dynamic overlay ownership (`overlay.host`, `not_hosting`,
  effective vs declared host), `battery.status`, `hello`, versioning/features/
  limits, and the expanded error model.
- **v1 addendum (full-fidelity rendering)** — owner decision recorded in
  §7.10: reuse `UIOverlay`/`GFX`/assets/font/theme, recommended as a separate
  optional SDL2 renderer package with a golden-image test; the MIT core stays
  data-only. NextUI's PolyForm-NC license makes this a separate package or a
  per-app vendor decision, not core code. No wire-protocol change. Replaces the v0 draft. `status.subscribe` from the
  draft is **not** a separate operation: one `subscribe` op opens the single
  event stream and carries the `status` family plus sleep/HUD/overlay events.
- **v0 draft** — placeholder scope and open questions.
