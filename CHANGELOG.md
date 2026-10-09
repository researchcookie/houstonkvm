# Changelog

What changed in each release of HoustonKVM. Versions follow
[Semantic Versioning](https://semver.org/); while the version is 0.x, a minor
release can change behaviour.

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
