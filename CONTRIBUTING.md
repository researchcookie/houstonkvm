# Contributing to HoustonKVM

Thanks for helping. Bug reports, hardware reports, documentation fixes and pull
requests are all welcome.

## Hardware reports count

The goal is to work with as many CH9329 adapters and capture dongles as possible,
and only people who own them can say what works. Open an issue with the model, its
USB ID (`lsusb`), the output of `v4l2-ctl --list-formats-ext` for a capture device,
and what did or didn't work. For a CH9329 adapter,
[`scripts/ch9329-selftest.py`](scripts/ch9329-selftest.py) checks it on its own
first, and its output is worth including.

## Building

See the [README](README.md#install). For everyday development, use the dev build.
It adds debug symbols and the format, lint and memory-check targets, and lives in
its own directory so it never disturbs a normal build:

```console
$ scripts/dev-build.sh
$ cmake --build build-dev --target format         # clang-format, rewrites in place
$ cmake --build build-dev --target format-check   # clang-format, check only
$ cmake --build build-dev --target tidy           # clang-tidy
$ cmake --build build-dev --target valgrind       # the test suite under valgrind
```

The code is C++20 and follows the `.clang-format` and `.clang-tidy` in the
repository root.

## Testing

Run the tests before sending a change, and add one for what you changed.

```console
$ scripts/run-tests.sh          # backend: starts a real server and talks HTTP to it
$ scripts/run-ui-tests.sh       # web UI in a real browser
```

The backend tests use only the Python standard library. The keyboard and mouse
tests talk to a pseudo-terminal that behaves like a CH9329 chip and check the exact
bytes it receives, so **the tests never need real hardware**. They must never open
a real serial device on your machine.

The UI tests use Playwright, and skip themselves if it or a browser isn't installed:
`pip install --user playwright && playwright install chromium`. Set
`HOUSTONKVM_UI_BROWSER=firefox` (or `webkit`) to run them in another engine.

### Load testing

`scripts/loadtest.py` estimates how many targets and viewers a machine can
carry. It builds an optimised server with synthetic video sources into
`build-loadtest/`, runs it on a free port with a throwaway database (no real
hardware is touched), and adds MJPEG viewers, WebRTC viewers and targets step
by step until viewers stop getting the full frame rate. The report shows the
server's CPU, memory and API response time at each step.

```console
$ scripts/loadtest.py                  # about ten minutes
$ scripts/loadtest.py --quick          # smoke test: fails if no video reaches a viewer
$ scripts/loadtest.py --pattern motion --width 1920 --height 1080 --report report.md
```

The viewers run on the same machine and are counted separately, so run it
on an otherwise idle machine for numbers worth comparing.

## Sending a change

- Keep each change focused on one thing, with a clear commit message: a short
  summary line, then *why* it was needed, not just what changed.
- Comments should explain why the code is the way it is, in words a newcomer can
  follow. Please don't leave notes about who tested what, or on which machine.
- Some behaviour comes from real hardware rather than the datasheet, such as the
  extra mouse-button report the CH9329 backend sends. It is documented next to the
  code. Please don't "correct" it to match a datasheet without new evidence from a
  device.

### Sign your commits

Every commit needs a `Signed-off-by` line. It certifies the
[Developer Certificate of Origin](https://developercertificate.org/): that you wrote
the change, or otherwise have the right to submit it under this project's license.
Git adds the line for you:

```console
$ git commit -s
```

By contributing, you agree that your contribution is licensed under the
[Apache License 2.0](LICENSE), the same as the rest of the project.

## Reporting a security problem

Please don't open a public issue for a vulnerability. See [SECURITY.md](SECURITY.md).
