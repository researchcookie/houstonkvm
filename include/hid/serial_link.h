#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace houston_kvm {

// The plumbing every serial keyboard/mouse adapter shares (Ch9329Inject,
// KvmdHidInject): a raw 8N1 line with no flow control, opened non-blocking.
// What goes over the line is each backend's own business.

// The line speeds Linux termios can set that the adapters use. A CH9329's
// datasheet also lists 14400, which has no termios constant.
inline constexpr std::array<int, 8> kSerialBauds = {1200,  2400,  4800,  9600,
                                                     19200, 38400, 57600, 115200};
bool isSerialBaud(int baud);

// Opens `device` raw at `baud` (one not in kSerialBauds is logged and opened
// at 9600), with input and output flushed. Returns the fd, or -1 with
// `error` saying which step failed, ready to log after the backend's name.
// `lowLatency` asks the USB-serial driver to hand each received byte over
// at once rather than batching: an FTDI chip otherwise holds a short reply
// for up to 16 ms, which a backend that waits for every reply can't afford.
// Drivers without that setting ignore it.
int openRawSerial(const std::string& device, int baud, std::string& error, bool lowLatency = false);
bool setSerialSpeed(int fd, int baud);

// An adapter's packet is a handful of bytes — normally well within the
// kernel tty output buffer even under load. But clicks and keys can go out
// back to back, and the fd is O_NONBLOCK, so at low baud rates a write can
// still come back EAGAIN/short. This retries only for local buffer space to
// free up (poll for POLLOUT) — never for the adapter or target to respond.
// Bounded to ~30ms total so one stuck write can't stall the input worker.
bool writeAllRetrying(int fd, const uint8_t* data, size_t len);

} // namespace houston_kvm
