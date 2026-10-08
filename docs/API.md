# HoustonKVM API

Everything the web UI does goes through this HTTP API, so scripts and other
programs can do it too: list the machines, watch one, and drive its keyboard
and mouse.

Responses are JSON unless noted. Errors are a status code with a short plain-text
message (for example `401 Invalid credentials`).

- [Authentication](#authentication)
- [Roles](#roles)
- [Finding a target](#finding-a-target)
- [Video](#video)
- [Controlling a target](#controlling-a-target)
- [Input messages](#input-messages)
- [Accounts and tokens](#accounts-and-tokens)
- [Audit log](#audit-log)
- [Metrics and logs](#metrics-and-logs)
- [The default target and the unscoped routes](#the-default-target-and-the-unscoped-routes)

## Authentication

There are two ways to authenticate, and every route that needs a login accepts either.

**Session cookie** (what the web UI uses):

```console
$ curl -c jar -X POST http://kvm.local:8080/api/login \
       -H 'Content-Type: application/json' \
       -d '{"username":"op","password":"operator-pass-1"}'
{"ok":true}
$ curl -b jar http://kvm.local:8080/api/me
{"id":2,"ok":true,"role":"operator","username":"alice"}
```

**API token** (for scripts and other programs): create one while signed in
(web UI: *Settings → API tokens*, or `POST /api/tokens`), then send it on every
request. The token is shown once, so store it when you create it.

```console
$ curl -X POST -b jar http://kvm.local:8080/api/tokens \
       -H 'Content-Type: application/json' -d '{"label":"my-script"}'
{"created_at":"2026-10-01 18:20:04","expired":false,"expires_at":"2026-12-30 18:20:04",
 "id":1,"label":"my-script","last_used_at":"","scope":"full","token":"<64 hex characters>"}

$ curl -H 'Authorization: Bearer <token>' http://kvm.local:8080/api/targets
```

Requests that carry the session cookie and change something (and WebSocket
upgrades) are refused with `403` if the browser says they came from a page on
another origin; see `--allowed-origin` in `HoustonKVM --help` for proxies that
rewrite `Host`. Clients that send no `Origin` header, like curl, aren't affected,
and neither are API tokens.

A token can do at most what the account that created it can, and its `scope`
can limit it further:

| `scope` | The token can |
|---|---|
| `full` (default) | Everything the account can. |
| `read` | What a Viewer can: find and watch targets and read their status. It can't drive a target or change anything. |
| `metrics` | Read `GET /metrics` and nothing else. Only an Owner can create one. |

Tokens expire: after 90 days unless `expires_in_days` says otherwise (1 to
3650), or never with `"expires_in_days": null`. An expired token gets `401`;
it stays listed, with `"expired": true`, until you revoke it. Tokens created
before scopes and expiry existed keep `full` scope and never expire.

Expiry, deleting the account or revoking the token (`DELETE /api/tokens/:id`)
ends its access, including control of a target it is currently driving.

Sessions last seven days. Changing your password
(`POST /api/account/password`) signs your account out of every other session at
once, including control of any target one of them was driving. The session
that made the change stays signed in, and the reply says how many others ended:
`{"ok":true,"signed_out_sessions":2}`. API tokens are kept; revoke them too if
they may have leaked.

Checking a password takes the server real work, so sign-in, setup, creating an
account and changing a password answer `503` with `Retry-After: 1` when too
many are already waiting. Try again after a second.

`GET /api/status` needs no login. It reports `{"authenticated":…,"needs_setup":…}`,
where `needs_setup` is true only until the first (owner) account exists.
Creating that account (`POST /api/setup` with `username`, `password` and
`setup_code`) needs the one-time code the server prints to its log when it starts
with no accounts; a wrong code is refused with `403` and counts toward the
sign-in lockout.

## Roles

| Role | Can |
|---|---|
| **Viewer** | Find and watch targets. |
| **Operator** | Everything a Viewer can, plus take control of a target and send it keyboard and mouse input. |
| **Owner** | Administer: add, change and delete targets and users. |

Roles are deliberately separate capabilities. **An Owner cannot drive a target**;
acquiring control and sending input require exactly the Operator role. An Owner
who wants to pilot a machine uses a separate Operator account. (An Owner can
force-release a control lock that someone left held.)

Changing configuration (targets, users, video settings) needs a **browser
session**: API tokens get `401` there. A token can list and watch targets and, if
it belongs to an Operator, drive them.

## Finding a target

A **target** is one machine you can watch and control: it has a name, an optional
description, group and tags, and its own capture device and keyboard/mouse
connection.

| Route | Role | |
|---|---|---|
| `GET /api/targets` | Viewer | List or search. Query: `q` (every whitespace-separated term must appear in the name, description, group or a tag), `tag`, `group`, `enabled=1`. |
| `GET /api/targets/facets` | Viewer | The groups and tags that exist, with counts. |
| `GET /api/targets/:id` | Viewer | One target. |
| `POST /api/targets` | Owner | Add one. |
| `PUT /api/targets/:id` | Owner | Change it; only the fields you send change. |
| `DELETE /api/targets/:id` | Owner | Remove it. |

```console
$ curl -H 'Authorization: Bearer <token>' 'http://kvm.local:8080/api/targets?q=web'
```

```json
{
  "targets": [
    {
      "id": 2,
      "name": "Web Server 1",
      "description": "Front door nginx box",
      "group": "Rack 3",
      "tags": ["linux", "prod"],
      "enabled": true,
      "default": false,
      "state": "running",
      "status": { "capture_active": false, "video_state": "reconnecting", "controlled": false, "driver": "", "input_health": "good", "input_ready": true, "webrtc_viewers": 0, "audio_state": "idle" }
    }
  ]
}
```

`status` says what you can do right now: `capture_active` means there is a
picture, `video_state` says why not when there isn't (see [Video
health](#video-health)), `input_ready` means the keyboard/mouse connection is up, `input_health`
is that connection's health (`good`, `degraded`, `down` or `unknown`, see
[Input health](#input-health)), and
`controlled` means someone currently holds control, and `driver` is
who (the username, or `""` when nobody does). `webrtc_viewers` is how many
people are watching over low-latency (WebRTC) video; a target only spends CPU
on encoding it while this is above zero. `audio_state` is the target's sound
(see [Sound](#sound)).

An Owner's browser session also gets a `config` object with the hardware wiring
(device paths, serial speed, capture mode, WebRTC bitrate, sound device). Nobody
else ever sees device paths.

Creating a target (`POST /api/targets`, Owner):

```json
{
  "name": "Web Server 1",
  "description": "Front door nginx box",
  "group": "Rack 3",
  "tags": ["prod", "linux"],
  "enabled": true,
  "v4l2_device": "/dev/video2",
  "capture_width": 1280, "capture_height": 720, "capture_fps": 30,
  "serial_device": "/dev/ttyUSB0", "serial_baud": 9600,
  "webrtc_bitrate_kbps": 4000,
  "audio_device": "auto"
}
```

Only `name` is required, but say how keyboard and mouse reach the machine.
That is one of three ways, chosen by which field is set: a CH9329 adapter
(`serial_device`), a QEMU virtual machine (`qmp_socket`), or, if neither is set,
this computer's own USB gadget port, which only one target can use. Names must be
unique, and one capture device or serial port can belong to only one target
(`409` otherwise). Tags may contain letters, digits and `. _ : / -`.
`audio_device` is where the sound comes from (see [Sound](#sound)).

A fresh install starts with one placeholder target named "Default target", set up
for the gadget port and `/dev/video0`. Edit or delete it when you add your own.

## Video

| Route | Role | |
|---|---|---|
| `GET /api/targets/:id/stream` | Viewer | MJPEG (`multipart/x-mixed-replace`). Works in an `<img>` tag. The connection stays open, and simply sends no frames while there is no picture; use `snapshot` to find out whether there is one. |
| `GET /api/targets/:id/snapshot` | Viewer | One JPEG. `503` while there is no picture. |
| `GET /api/targets/:id/video/settings` | Operator, browser session | Capture mode and bitrate, and what is actually streaming now. |
| `PUT /api/targets/:id/video/settings` | Operator, browser session | Change resolution, frame rate or bitrate (`capture_width`, `capture_height`, `capture_fps`, `webrtc_bitrate_kbps`). Rebuilds that target's capture, which pauses the picture for up to about ten seconds for everyone watching. |
| `GET /api/targets/:id/video/capabilities` | Operator, browser session | The modes the capture device reports. |

Low-latency H.264 video over WebRTC is offered through
`POST /api/targets/:id/webrtc/subscribe` and `/webrtc/answer` (Viewer). The web UI's
[`ui/js/webrtc.js`](../ui/js/webrtc.js) is the reference client. Media travels
over UDP ports 50000–50100 directly to the server.

Subscribe with `?candidates=on-answer`, as the web UI does:

1. `POST /api/targets/:id/webrtc/subscribe?candidates=on-answer` answers
   `{"subscriberId": …, "sdp": …, "iceServers": […]}` at once. The offer
   carries none of the server's ICE candidates. `iceServers` is the STUN/TURN
   servers to use, in the form `RTCPeerConnection` takes (`[{"urls":
   "turn:…", "username": …, "credential": …}]`); usually `[]`, since none are
   needed on a LAN. See [STUN and TURN servers](#stun-and-turn-servers).
2. Gather your own candidates, then post your answer, with all of them in it,
   to `/webrtc/answer` with the same `subscriberId`.
3. The response is `{"ok": true, "candidates": [{"candidate": "candidate:…",
   "mid": "video"}, …]}`. Add each one (`addIceCandidate({candidate, sdpMid:
   mid})` in a browser). The connection comes up from there.

Holding the server's candidates back until it has your answer matters. A client
that has them sooner starts connecting at once, and its handshake can reach the
server between libdatachannel applying the answer to ICE and storing it, so the
server can't check the client's certificate. The connection then fails for good.
Under load, that happened to about 1 connection in 200.

Without `?candidates=on-answer` you get the original flow, still supported: the
offer comes once the server has gathered its candidates, and includes them. If
gathering is still waiting on a STUN or TURN server after 2 seconds, the offer
comes with the candidates found so far; `503` if there are none by 5 seconds. `answer` then returns
`{"ok": true, "candidates": []}`.

Either way, a subscription that isn't connected 30 seconds after its offer is
closed, and `answer` then returns `404`: subscribe again.

### STUN and TURN servers

None by default: on a LAN or VPN, browsers connect to the server's own
addresses, and the server contacts nothing outside the network. Add one only if
browsers reach the server through NAT. The server and every viewer use the same
list.

| Route | Role | |
|---|---|---|
| `GET /api/admin/ice-servers` | Owner | `{"ice_servers": [{"url", "username", "credential"}], "managed_by_config": false}`. |
| `PUT /api/admin/ice-servers` | Owner, browser session | Replace the list: `{"ice_servers": [...]}`, at most 8. Viewers who connect afterwards use it. `409` when `managed_by_config`. |

- `url` is `stun:HOST[:PORT]`, `turn:HOST[:PORT]` or `turns:HOST[:PORT]`. A
  TURN address may end in `?transport=udp` or `?transport=tcp`. `HOST` is a
  name, an IPv4 address or a bracketed IPv6 address.
- A TURN server needs `username` and `credential`; a STUN server takes neither.
- Every viewer's browser is sent the TURN credentials, since it needs them, so
  use credentials meant for that.
- If a server on the list doesn't answer, the server's ICE gathering goes ahead
  without it after 2 seconds, with the candidates it has (its own addresses),
  and logs that it did. Only a server with no candidates at all gives up, after
  5 seconds.
- `--ice-server` on the command line (repeatable, or `--ice-server=none`)
  fixes the list instead: `managed_by_config` is then `true`, and Admin shows
  the list read-only.
- Changes are in the audit log as `server.ice_servers`, with the addresses
  but never the credentials.

## Controlling a target

Only one person drives a target at a time. Take control, send input, release:

| Route | Role | |
|---|---|---|
| `GET /api/targets/:id/control/status` | Viewer | `{"controlled":true,"is_driver":false,"driver":"alice","typing_remaining":0}`. `driver` is `""` when nobody has control. `typing_remaining` counts characters of `text` (below) not yet typed. |
| `POST /api/targets/:id/control/acquire` | Operator | Take control. `409` if someone else has it or an input test is running; `503` if the target has no working keyboard/mouse connection. |
| `POST /api/targets/:id/input` | Operator (driver) | Send one input message (below). |
| `WS  /api/targets/:id/input/ws` | Operator (driver) | The same messages over a WebSocket, for lower overhead. Browser sessions only, since a browser WebSocket cannot send an `Authorization` header. |
| `POST /api/targets/:id/control/release` | Viewer | Give control up. |

```console
$ curl -X POST -H 'Authorization: Bearer <token>' \
       http://kvm.local:8080/api/targets/2/control/acquire
{"ok":true}

$ curl -X POST -H 'Authorization: Bearer <token>' -H 'Content-Type: application/json' \
       -d '{"type":"key","code":"KeyA","pressed":true}' \
       http://kvm.local:8080/api/targets/2/input
```

Whenever control ends (released, taken over by an Owner, the driver logging out,
their account or token being removed, or the target stopping), every key and mouse
button the driver was holding is released on the target, so nothing stays stuck
down. Closing the input WebSocket releases the held keys too, but keeps the
control lock.

Every stretch of control is recorded in the [audit log](#audit-log), with how
many keys were pressed and how much mouse input was sent. Which keys, and where
the mouse went, are never recorded.

## Video health

A capture device that stops (a dongle pulled out) or can't be opened is
retried by itself, 1, 2, 4 … up to 30 seconds apart, so video comes back
without anyone touching the target's settings. An open `/api/targets/:id/stream`
connection stays open meanwhile and carries on once frames return; snapshots
answer `503` until then.

`GET /api/targets/:id/health` (below) has a `video` object:

```json
"video": {"state":"reconnecting","last_error":"VIDIOC_DQBUF: No such device",
          "reconnects":2,"retry_in_ms":3500}
```

- `state`: `good` (frames arriving); `starting` (opened, no frame yet);
  `no_signal` (open, but no frame for 3 seconds: check the video cable to the
  target, and that it's on. Some capture dongles don't start sending again by
  themselves when their HDMI cable is plugged back in, so the server restarts
  the capture 10 seconds after the picture went, then at doubling intervals up
  to every 5 minutes, until a picture comes); `reconnecting` (it was working and stopped);
  `absent` (it hasn't opened since the target started: check it's plugged in,
  and the device path); `off` (no capture device set). The same value is
  `status.video_state` in the target list.
- `last_error`: why the last open failed or the capture stopped, kept after it
  recovers, or `null`. The device path is replaced by "the capture device".
- `reconnects`: how often the video has come back by itself since the target
  started.
- `retry_in_ms`: time until the next attempt while `reconnecting` or `absent`,
  otherwise `null`.

## Sound

Most HDMI capture dongles are also a USB sound card carrying the target's
sound. It reaches viewers with low-latency (WebRTC) video as an Opus audio
track (48 kHz stereo, 96 kbps), and is captured only while someone watches over
WebRTC. Standard (MJPEG) video has no sound. Browsers start it muted.

A target's `audio_device` says where it comes from:

- `"auto"` (the default): the sound card on the same USB device as the
  target's capture device, looked up afresh each time it opens.
- `""`: no sound.
- An ALSA device: `plughw:CARD=Video,DEV=0` and the like, as
  `GET /api/admin/devices` lists them in `audio_devices`. Only `hw:`, `plughw:`
  and `sysdefault:` names are accepted. One sound device can belong to only one
  target (`409` otherwise).

`GET /api/targets/:id/health` has an `audio` object, and the same `state` is
`status.audio_state` in the target list:

```json
"audio": {"state":"absent","last_error":"The capture device has no sound card of its own."}
```

- `state`: `good` (capturing and sending); `idle` (nobody is watching over
  WebRTC, so nothing is captured); `absent` (no sound card found, or it can't be
  opened or has stopped: retried 1, 2, 4 … up to 30 seconds apart);
  `off` (`audio_device` is `""`); `unsupported` (this build has no sound
  support).
- `last_error`: why the last attempt to open or capture failed, kept until it
  next works, or `null`.

## Input health

Whether a target's keyboard/mouse link works, and if not, which cable to look at.

| Route | Role | |
|---|---|---|
| `GET /api/targets/:id/health` | Viewer | The link's state, a one-line `summary` saying what's wrong, and the last hour's numbers; plus `video` (see [Video health](#video-health)). |
| `POST /api/targets/:id/health/test` | Owner (browser session) | Start a self-test: 20 status checks to the adapter plus a read of its settings. Nothing is typed or clicked on the target. `202` when started; `409` while someone has control or a test is already running; `400` for connections that can't be tested. Taking control is refused until it ends, a few seconds later. |
| `POST /api/targets/:id/health/baud` | Owner (browser session) | Body `{"baud":115200}`. Change a CH9329 adapter's line speed: rewrite the rate stored in the adapter, reset it if it needs that to switch, and check it answers reliably at the new rate. If it does, the target's `serial_baud` is saved to match. Otherwise the adapter is put back as it was. `202` when started; `409` while someone has control or a change is already running; `400` for an unsupported rate or a connection with no adjustable speed. Taking control is refused until it ends, a few seconds later. |

```console
$ curl -H 'Authorization: Bearer <token>' http://kvm.local:8080/api/targets/2/health
{"kind":"ch9329","state":"degraded",
 "summary":"3 of 9 checks failed (3 reconnects): likely a loose or failing cable or connector. Reseat it, or swap it to confirm.",
 "device_open":true,"target_enumerated":true,"firmware":"3.0",
 "configured_baud":115200,"chip_baud":115200,"answers_at_baud":null,
 "last_hour":{"checks_ok":6,"checks_failed":3,"reply_ms_avg":2.1,"reply_ms_max":3.4,
              "reconnects":3,"dropped_commands":0},
 "test":{"state":"idle"},"baud_change":{"state":"idle"},
 "video":{"state":"good","last_error":null,"reconnects":0,"retry_in_ms":null}}
```

- `state`: `good`; `degraded` (working, but checks failed recently); `down` (input
  can't get through); `unknown` (not measured yet, e.g. the target is starting).
- `kind`: `ch9329`, `usb-gadget` or `qemu`. Only a CH9329 adapter can answer back,
  so the measured fields and `test` are `null` for the others, whose state only
  says whether the device is there.
- A CH9329 is checked every 5 seconds. `target_enumerated` is whether the target
  machine has recognised the adapter as a keyboard and mouse. `chip_baud` is the rate
  stored in the adapter itself; `answers_at_baud` is set when the adapter stopped
  answering at the configured rate but answers at another one.
- `test.state` is `idle`, `running` or `done`. Once `done` it also has
  `verdict_state`, `verdict`, `probes`, `ok`, `reply_ms_avg`, `reply_ms_max`,
  `target_enumerated`, `firmware`, `chip_baud`, `work_mode`, `serial_mode` and
  `finished_at` (Unix time).
- `baud_change.state` is `idle`, `running` or `done` (`null` for connections with no
  adjustable speed). Once started it has `from_baud` and `to_baud`, and once `done`
  also `ok`, a one-sentence `message` and `finished_at`. The CH9329 supports 1200,
  2400, 4800, 9600, 14400, 19200, 38400, 57600 and 115200 baud. Adapters ship at
  9600, and at 115200 each input report crosses the serial link 12 times faster.

## Input messages

JSON objects with a `type`:

| `type` | Fields | |
|---|---|---|
| `mousemove` | `x`, `y` | Absolute position as a fraction of the picture: `0.0`–`1.0` on each axis, `(0,0)` at the top left. |
| `mousebutton` | `button`, `pressed` | `button` is the browser's index: `0` left, `1` middle, `2` right. `pressed` is `true` or `false`. |
| `mousescroll` | `delta` | Scroll steps: positive scrolls up, negative scrolls down. |
| `key` | `code`, `pressed` | `code` is a browser [`KeyboardEvent.code`](https://developer.mozilla.org/docs/Web/API/KeyboardEvent/code) such as `KeyA`, `Digit1`, `Enter` or `ControlLeft`. `pressed` is `true` or `false`. |

A key stays down until you send it with `pressed: false` (or control ends), so
pair every press with a release. Typing a capital `A` is `ShiftLeft` down,
`KeyA` down, `KeyA` up, `ShiftLeft` up.

To type a whole string, send it as one message and let the server press the keys:

| `type` | Fields | |
|---|---|---|
| `text` | `text` | Typed on the target one key at a time, as on a **US keyboard layout**. Only printable ASCII, newlines and tabs can be typed; `\r\n` and a lone `\r` are each one Enter. Anything else answers `400`, naming the first character it can't type, and nothing is typed. Sending more while typing is under way adds it to the end. |
| `text_cancel` | none | Stops typing what's left. A key it had down, and Shift, are released. |

Typing is paced for the keyboard link: a CH9329 at 9600 baud manages about 13
characters a second, and 50 or so at 115200. Your own `key` and mouse messages
aren't held up behind it. Up to 65,536 characters can be waiting at once; past
that, `text` answers `503`. Typing stops, with nothing left held, whenever control
ends, and if the keyboard link goes down. Follow it with `typing_remaining` in
`control/status`.

```console
$ curl -X POST -H 'Authorization: Bearer <token>' -H 'Content-Type: application/json' \
       -d '{"type":"text","text":"uname -a\n"}' \
       http://kvm.local:8080/api/targets/2/input
{"ok":true}
```

## Accounts and tokens

| Route | Role | |
|---|---|---|
| `POST /api/setup` | none | Create the first account (Owner): `username`, `password`, and the `setup_code` from the server log. Only works while none exists. |
| `POST /api/login`, `POST /api/logout` | none / any | Session sign-in and sign-out. Repeated failures from one address are locked out for a while. |
| `GET /api/me` | any | Who you are: id, username and role. |
| `POST /api/account/password` | any, browser session | Change your own password (`current_password`, `new_password`, 8 to 1024 characters). Signs out your other sessions; see [Authentication](#authentication). |
| `GET/POST /api/tokens`, `DELETE /api/tokens/:id` | any, browser session | Your API tokens. `POST` takes `label`, `scope` and `expires_in_days`; see [Authentication](#authentication). |
| `GET/POST /api/users`, `DELETE /api/users/:id`, `PUT /api/users/:id/role` | Owner | Manage accounts. Roles: `viewer`, `operator`, `owner`. |
| `GET/PUT /api/settings` | any | Your own preferences (theme, default stream mode). |
| `GET /api/admin/devices` | Owner | The capture devices, serial adapters and sound cards currently plugged in, and which target uses each. `audio_supported` says whether this build can capture sound. |

Usernames are letters, digits, `_` and `-` (up to 64). Passwords must be at
least 8 characters.

## HTTPS

The server starts on plain HTTP. An Owner turns on HTTPS at runtime, with no
restart: get a certificate, enable, then confirm from a browser over HTTPS.

There are three ways to get a certificate:
- **HoustonKVM's own CA.** On first use the server makes a small CA of its
  own, valid 10 years, and issues the server certificate from it, valid 397
  days. Trust the CA once (`GET /api/tls/ca.crt`, then a browser, GPO or
  Ansible). Its certificates then renew themselves 30 days before they expire
  (`auto_renew`), and nothing that trusts the CA notices.
- **A CSR for your own CA.** The key is made on the server and never leaves
  it; install the signed certificate on its own.
- **A certificate and key** from elsewhere.
A browser session or an Owner's API token both work, so a certbot deploy hook
can install renewals.

| Route | Role | |
|---|---|---|
| `GET /api/tls` | Owner | Status, below. |
| `PUT /api/tls/settings` | Owner | Any of `{"https_port": 8443, "http_redirect": true, "hsts": false, "auto_renew": true}`, applied at once. `auto_renew` only ever applies to the own CA's certificates. |
| `POST /api/tls/generate` | Owner | `{"names": [...], "key_type": "ecdsa-p256"}`, both optional: a certificate from HoustonKVM's own CA (made first if needed), installed at once. `names` defaults to `suggested_names`. Replies like `certificate`. |
| `GET /api/tls/ca.crt` | Viewer | The own CA's certificate (PEM), to trust. `404` before there is one. |
| `POST /api/tls/csr` | Owner | `{"names": [...], "key_type", "organization", "unit", "country"}`: a key kept on the server and a CSR for it (`{"csr": PEM}`), replacing any earlier one. |
| `GET /api/tls/csr`, `DELETE /api/tls/csr` | Owner | The pending CSR (PEM), or discard it. |
| `POST /api/tls/certificate` | Owner | `{"cert": PEM, "key": PEM}`: the certificate chain (the server's own first, then any intermediates) and its unencrypted key. Or `{"cert": PEM}` alone, issued for the pending CSR. Replies `{"certificate": {...}, "warnings": [...]}`. Used for new connections at once. |
| `POST /api/tls/enable` | Owner | Starts HTTPS beside HTTP. Replies `{"https_url", "confirm_code", "expires_in", "warnings"}`. |
| `POST /api/tls/confirm` | none, over HTTPS only | `{"code": ...}`: proves a browser reached HTTPS, which makes it stick. |
| `POST /api/tls/disable` | Owner | Back to plain HTTP, or cancels an enable that's waiting. |

**Confirm or revert.** `enable` keeps HTTP serving as before and starts HTTPS
on `https_port`. Opening `https_url` (HTTPS, with the code after `#`) and posting
the code to `/api/tls/confirm` from there makes HTTPS stick: it's saved, and
plain HTTP then only redirects (`307`) to HTTPS, or, with `http_redirect` off,
stops listening. With no confirmation in 2 minutes, HTTPS is torn down and
nothing is saved, so a blocked port or a certificate the browser refuses can't
lock the Owner out. `409` when the port can't be had, or there's no certificate.

**The certificate** is refused (`400`) when it isn't PEM, its key doesn't match
or is protected by a passphrase, it has expired or isn't valid yet, or its key
is neither RSA of 2048 bits or more nor ECDSA P-256 or P-384. It's accepted with
a warning when it doesn't name the host the request came to, or when the chain
looks incomplete. The key is never sent back by any route.

**Status:**

```json
{"mode": "https", "pinned": null, "http_port": 8080, "https_port": 8443,
 "https_port_managed_by_config": false, "http_redirect": true, "hsts": false,
 "certificate_managed_by_config": false, "certificate_error": "", "pending": null,
 "certificate": {"subject": "CN=kvm.example.com", "issuer": "CN=Example CA",
   "self_signed": false, "names": ["kvm.example.com", "192.0.2.10"],
   "not_before": "2026-10-01T00:00:00Z", "not_after": "2027-11-01T00:00:00Z",
   "days_left": 396, "expired": false, "covers_this_host": true,
   "fingerprint_sha256": "AB:CD:...", "key_type": "ECDSA P-256"}}
```

`mode` is `http`, `pending` (waiting for confirmation; `pending` then has
`expires_in`) or `https`. `last_revert` is `null`, or `{seconds_ago,
https_port}` once a switch went back unconfirmed, until the next `enable`.
Also in the status: `auto_renew`, `ca` (the own CA's
`subject`, `fingerprint_sha256` and `not_after`, or `null`), `pending_csr`,
`suggested_names` (the name in use, then this machine's names and addresses),
and in `certificate`, `issued_by_houstonkvm_ca`. `key_type` is `ecdsa-p256`,
`ecdsa-p384` or `rsa-3072`; names are host names (a `*.` wildcard allowed) or
IP addresses, at most 20.

**Under HTTPS:**
- The session cookie is `__Host-session`, always `Secure`. The plain-HTTP
  cookie isn't honoured there, so switching signs everyone in once more.
- Requests with the cookie must come from an `https://` page.
- `hsts` adds `Strict-Transport-Security`. It is refused for a self-signed
  certificate: HSTS takes away the browser's way past a certificate warning.

**The configuration decides first.** `--tls=on` or `--tls=off` fixes whether
HTTPS is on (`pinned`; `enable` and `disable` answer `409`). `--tls=off` is also
the way back in after a mistake: it turns the Owner's HTTPS off too (audited as
`tls.disabled` by `(--tls=off)`), so it stays off once the option is removed. `--https-port` fixes the port. A certificate
in `/etc/houstonkvm/tls/` (`houstonkvm.crt` with its chain, `houstonkvm.key`),
or named by `--tls-cert` and `--tls-key`, is served instead of the Owner's, and
`POST /api/tls/certificate` answers `409`. The server won't start when a
certificate the configuration names can't be served.

**Renewal:** SIGHUP (`systemctl reload houstonkvm`) re-reads the certificate
files, whoever's they are. New connections get the new certificate; open ones
keep theirs. A pair that doesn't load leaves the one in use in place, and
`certificate_error` says why.

## Audit log

The server records what happens on it, and keeps it for 90 days (change that
with `--audit-retention-days`). Only Owners can read it. A browser session or an
Owner's API token both work, so a collector can fetch it.

| Route | Role | |
|---|---|---|
| `GET /api/audit/events` | Owner | What happened, newest first. |
| `GET /api/audit/control-sessions` | Owner | Who drove which target, when, for how long, how many keys they pressed and mouse events they sent, and why it ended. |

**Events** cover sign-ins (including refused ones and the name that was tried),
sign-outs, setup, password changes, API tokens, accounts and roles, targets
(with the fields each change touched), video settings, input tests and speed
changes, and taking and losing control. Each event carries the account, the
client address, whether it came with an API token, the target, an `outcome`
(`ok`, `denied`, `busy` when the server turned it away as overloaded, or
`failed`) and
type-specific `detail`:

```console
$ curl -H 'Authorization: Bearer <owner token>' \
       'http://kvm.local:8080/api/audit/events?type=auth.login&limit=1'
{"events":[{"id":42,"at":"2026-09-30T14:02:11.513Z","type":"auth.login","outcome":"denied",
  "user":{"id":null,"username":"admin"},"ip":"192.0.2.7","via":"session","target":null,
  "detail":{"reason":"unknown_user"}}],"next_before":42}
```

The event types are `auth.setup`, `auth.login`, `auth.logout`,
`auth.password_change`, `token.create`, `token.revoke`, `user.create`,
`user.role_change`, `user.delete`, `target.create`, `target.update`,
`target.delete`, `target.video_settings`, `target.input_test`,
`target.baud_change`, `server.ice_servers`, `tls.ca_created`, `tls.csr_created`,
`tls.certificate_installed` (`source` is `upload`, `csr`, `houstonkvm-ca`; a
self-renewal is by `(auto-renew)`),
`tls.enabled` (confirmed over HTTPS), `tls.reverted` (not confirmed in time),
`tls.disabled`, `tls.settings`, `tls.reloaded` (`failed` when the new pair
didn't load), `control.acquire`, `control.release`, and `control.force_release`
(an Owner taking control away).

Filters, all optional: `type` (an exact type, or a prefix ending in `.` such as
`auth.`), `user_id`, `target_id`, `since` and `until` (ISO 8601 UTC, like `at`),
and `limit` (1 to 500, default 100). For older rows, pass the previous reply's
`next_before` as `before`; it is `null` on the last page. Control sessions take
`user_id`, `target_id`, `limit` and `before` the same way.

A control session's `end_reason` is `released`, `owner_override`, `signed_out`,
`credentials_revoked`, `target_stopped`, `server_stopped`, or `interrupted` (the
server stopped without shutting down cleanly, so when it ended isn't known).
`key_count` is how many keys were pressed, and `mouse_count` how many mouse
moves, clicks and scrolls were sent. **What was typed is not recorded anywhere**,
not in the log, not in the database: keeping it would keep every password typed
into a target.

## Metrics and logs

`GET /metrics` reports the server's state in Prometheus's text format. An
Owner can read it, and so can a `metrics`-scoped token, which is what to give
Prometheus:

```yaml
scrape_configs:
  - job_name: houstonkvm
    authorization:
      credentials_file: /etc/prometheus/houstonkvm.token
    static_configs:
      - targets: ["kvm.local:8080"]
```

| Metric | |
|---|---|
| `houstonkvm_events_total{type,outcome}` | Every [audit log](#audit-log) event since the server started, by type and outcome. Refused sign-ins are `{type="auth.login",outcome="denied"}`. |
| `houstonkvm_event_loop_lag_seconds` | Histogram of how late the server's event loop ran. Every request and video stream waits while it's late, so alert on this first. |
| `houstonkvm_target_enabled`, `houstonkvm_target_running` | Per target (labels `target_id`, `target`). |
| `houstonkvm_target_video_state{state}` | 1 for the target's current video state: `off`, `starting`, `good`, `no_signal`, `reconnecting`, `absent`. |
| `houstonkvm_target_video_reconnects_total` | Times its video came back by itself since the target started. |
| `houstonkvm_target_input_health{state}` | 1 for its input link's current health: `unknown`, `good`, `degraded`, `down`. |
| `houstonkvm_target_input_checks_failed`, `_input_reconnects`, `_input_dropped_commands`, `_input_reply_seconds_max` | The input link over the last hour, as in [Input health](#input-health). |
| `houstonkvm_target_audio_state{state}` | 1 for its sound's current state. |
| `houstonkvm_target_viewers{transport}` | Viewers watching it, `mjpeg` or `webrtc`. |
| `houstonkvm_target_controlled` | 1 while someone drives it. |
| `houstonkvm_build_info{version}`, `process_*` | Version, CPU time, memory, open files, start time. |

With `--log-format=json` the server writes each log line as one JSON object,
for a log shipper:

```json
{"ts":"2026-10-01T19:17:14.481Z","level":"warning","component":"InputQueue","msg":"InputQueue: SCHED_FIFO unavailable (needs CAP_SYS_NICE) — input worker runs at normal scheduling priority, not pinning CPU affinity either (see input_queue.cpp)"}
```

`level` is `info` for what the server writes to standard output and `warning`
for standard error. `component` is the part of the server the line came from,
when the line names one.

## The default target and the unscoped routes

Older clients, and simple scripts, can leave the target out: `/api/stream`,
`/api/snapshot`, `/api/input`, `/api/input/ws`, `/api/control/acquire`,
`/api/control/release`, `/api/control/status`, `/api/video/settings`,
`/api/video/capabilities`, `/api/webrtc/subscribe` and `/api/webrtc/answer`
mean the same as their `/api/targets/:id/…` forms, applied to the **default
target**.

The default target is one an Owner has designated on purpose (the *Make this the
default target* box in the target editor). It is never guessed. If none is
designated, the unscoped routes answer `404` (except `control/status`, which just
reports that nobody is driving). If it is changed while someone is
driving the old one, they simply stop being the driver, rather than silently
steering a different machine.

For anything new, address the target explicitly.
