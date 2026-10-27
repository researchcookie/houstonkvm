# Changelog

What changed in each release of HoustonKVM. Versions follow
[Semantic Versioning](https://semver.org/); while the version is 0.x, a minor
release can change behaviour.

## 0.2.1 (2026-10-26)

- With `--log-format=json`, every line is JSON again on EL9. EPEL's
  libdatachannel 0.23 makes GLib print a warning as each WebRTC connection
  is set up, and GLib wrote it to the log as plain text; it now comes out as
  a JSON line with GLib's domain as its `component`. This also kept 0.2.0's
  EL9 package from building, so 0.2.1 is the first 0.2 release with
  packages.

## 0.2.0 (2026-10-26)

- A second kind of keyboard and mouse adapter: a Raspberry Pi Pico, an Arduino
  Micro or Leonardo, or a 5 V Pro Micro running kvmd's open-source HID
  firmware, wired to a USB-serial adapter. HoustonKVM doesn't include the
  firmware: you build and flash it yourself. For anyone who would rather not
  plug a closed CH9329 into their machines. Choose it per target in Admin, or
  set the new `serial_adapter` field to `kvmd-hid` in the API; existing targets
  stay `ch9329`. The board confirms each command, and a key or click that didn't
  arrive is sent again. Input health and the input test work as for a CH9329.
  [docs/install.md](docs/install.md#keyboard-and-mouse-with-open-source-firmware)
  covers what to buy, flashing and wiring, and `scripts/kvmd-hid-selftest.py`
  checks a board on its own.
- A target's control lock is released when the driver's browser tab closes,
  and by the server after `--control-idle-seconds` (default 600) with no input
  and no status poll from the driver — so a closed tab or a dropped network no
  longer holds a machine until the session expires. A tab that is still open
  and polling keeps its lock, and so does a driver whose pasted text is still
  being typed.
- Deleting or disabling a target while its video is capturing, or stopping
  the server, no longer writes into freed memory: the capture thread is now
  stopped before the target's state is torn down, rather than after part of
  it. It could crash the server.
- A QEMU virtual machine that exits, or is shut down from inside, while
  someone drives it no longer takes the whole server down. The next key or
  mouse move sent to its closed QMP socket used to kill HoustonKVM, and with
  it every target; now that target's input is reported down.
- The QEMU QMP input backend no longer hangs a target's startup when the QMP
  socket connects but never completes the handshake; it gives up after a short
  timeout and the target comes up with input marked unavailable.
- The serial-adapter speed `14400` is no longer offered: it has no Linux
  termios speed and was silently falling back to 9600. The accepted rates are
  1200, 2400, 4800, 9600, 19200, 38400, 57600 and 115200.

## 0.1.4 (2026-10-13)

- A request body larger than a route's limit is now answered `413 Payload Too
  Large` once and nothing further happens. Previously the rest of a body split
  across network packets could still reach the route handler after the `413`
  had been sent, which ran the handler past the size limit and wrote a second
  time into the already-finished response. Affects every route that reads a
  body, including ones reachable before sign-in.

## 0.1.3 (2026-10-08)

Documentation only; the server is unchanged.

- A new README introduction: what HoustonKVM is for, and what each machine
  needs, before the install commands.
- The license and documentation checks are now
  `tests/integration/test_licensing_and_docs.py`.

## 0.1.2 (2026-10-08)

Documentation only; the server is unchanged.

- Signed packages for EL9 and EL10 from a dnf repository. The README's quick
  start installs from it in a few commands (it needs EPEL and CRB).
- A shorter README that opens with a demo. Installing, building, first run and
  hardware are in [docs/install.md](docs/install.md); HTTPS, reverse proxies,
  sign-in, the audit log and API tokens in [docs/security.md](docs/security.md).

## 0.1.1 (2026-10-07)

- When a CH9329 adapter answers at a different baud rate than its target is
  set to, the server says so straight away after its machine boots, instead
  of up to five minutes later.

## 0.1.0 (2026-10-07): first public release

HoustonKVM goes public. This release is everything it does so far.

**Machines and video**

- Many machines from one server. Each target has its own capture device,
  keyboard and mouse connection, and settings, and the directory finds it by
  name, group or tag.
- Video in the browser as MJPEG, or as low-latency H.264 over WebRTC with
  Cisco's OpenH264. A target is only encoded while someone watches, and its
  video recovers by itself when a capture dongle or HDMI cable is pulled and
  plugged back in.
- The target's sound, over low-latency video.
- Keyboard and mouse through CH9329 serial-to-USB adapters, a USB HID gadget,
  or QEMU's QMP socket. Each target's input link is checked and reported as
  good, degraded or down, and an Owner can run an input test.
- Paste text into a target, and the server types it.

**People and access**

- Roles: Viewers watch, Operators drive (one at a time per target), and Owners
  administer. Keys held down are released whenever control ends.
- Passwords hashed with Argon2id. The first Owner needs a one-time setup code
  from the server's log. Sign-in is rate limited per address.
- API tokens scoped to `full`, `read` or `metrics`, expiring after 90 days by
  default. Everything in the web UI is in the [API](docs/API.md).
- An audit log of sign-ins, changes, and who drove which target, for how long
  and how many keys and mouse events they sent. What was typed is never
  recorded.

**Security and operations**

- HTTPS built in and managed from the web UI, with no restart: a certificate
  from your organization's CA (by a signing request whose key never leaves the
  server) or from HoustonKVM's own small CA, which renews it. Turning HTTPS on
  only sticks once a browser confirms it works. It can also be fixed in the
  configuration, with `systemctl reload` for renewed certificates.
- No connections out of your network: no STUN or TURN server unless an Owner
  adds one.
- A strict Content-Security-Policy, and Origin checks on every request that
  uses the session cookie and on the input socket. Works behind a reverse
  proxy (`--trusted-proxy`, `--allowed-origin`).
- Prometheus metrics at `/metrics` and JSON logs (`--log-format=json`).

**Installing**

- An RPM for CentOS Stream 9 and 10 and their relatives that follows the
  Fedora packaging guidelines: a locked-down systemd service running as its
  own user, a firewalld service that opens nothing by itself, and the test
  suite run while building.
- A web interface of plain HTML, CSS and JavaScript built into the executable,
  with its two fonts, so it never reaches out to the Internet.
