#pragma once

#include "hid/input_backend.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace houston_kvm {

// Keyboard and mouse through a microcontroller running kvmd's open-source
// HID firmware (hid/pico and hid/arduino in the kvmd repository): a
// Raspberry Pi Pico, or an ATmega32U4 board such as an Arduino Micro or a
// Pro Micro, is the keyboard and mouse the target sees, and takes commands
// on its UART from a USB-serial adapter plugged into this server. This
// speaks the firmware's serial protocol; it shares no code with it.
//
// Unlike a CH9329, the firmware answers every request, and reads requests as
// back-to-back 8-byte frames with nothing to mark where one starts. So each
// command waits for its reply — a couple of milliseconds at 115200 baud —
// before the next goes out. That makes delivery known rather than hoped
// for: a key or click that gets no good reply is sent again, once the
// firmware has dropped whatever part of a frame it was holding (see
// resyncLocked() and send()).
class KvmdHidInject final : public InputBackend {
public:
    explicit KvmdHidInject(const std::string& serialDevice, int baudRate = 115200);
    ~KvmdHidInject() override;

    KvmdHidInject(const KvmdHidInject&) = delete;
    KvmdHidInject& operator=(const KvmdHidInject&) = delete;

    bool isReady() const override;
    const char* kind() const override { return "kvmd-hid"; }
    InputHealth health() const override;
    bool startSelfTest() override;
    bool hasSelfTest() const override { return true; }
    InputSelfTest selfTest() const override;

    void mouseMoveAbsolute(double x, double y) override;
    void mouseButton(int button, bool pressed) override;
    void mouseScroll(int delta) override;
    void keyEvent(const std::string& jsCode, bool pressed) override;
    void releaseAll() override;
    std::chrono::milliseconds typingInterval() const override;

    // A request: 0x33, the command, four argument bytes, then crc16() of
    // those six bytes, high byte first. Replies have the same shape (see
    // Reply), starting 0x34.
    using Frame = std::array<uint8_t, 8>;
    static Frame buildRequest(uint8_t cmd, uint8_t a0 = 0, uint8_t a1 = 0, uint8_t a2 = 0,
                              uint8_t a3 = 0);
    // CRC-16/MODBUS: reflected polynomial 0xA001, starting from 0xFFFF.
    static uint16_t crc16(const uint8_t* data, size_t len);
    // The firmware's own number for the key a browser KeyboardEvent.code
    // names — not its USB usage ID — or 0 if it has none.
    static uint8_t keyNumber(const std::string& jsCode);

private:
    // Bytes 1 and 2 of a reply.
    struct Reply {
        uint8_t code    = 0;   // bit 7 set: done, with status flags; otherwise an error code
        uint8_t outputs = 0;   // the keyboard and mouse modes in use
        bool ok() const { return code & 0x80; }
    };

    const std::string device_;
    const int         baudRate_;
    // How long an input command waits for its reply: a margin, plus the time
    // the request and reply take on the wire at baudRate_ — most of it at
    // the slowest rates.
    const std::chrono::milliseconds replyTimeout_;

    // Guards fd_ and all I/O on it: input (InputQueue's worker thread), the
    // watchdog and the self-test all exchange frames, and a request and its
    // reply must never be split by someone else's.
    mutable std::mutex ioMutex_;
    int fd_ = -1;
    // Mirrors for isReady(), which the event loop calls and which must not
    // wait on ioMutex_. Ready means the device is open AND the board
    // answered the last probe.
    std::atomic<bool> deviceOpen_{false};
    std::atomic<bool> lastProbeOk_{false};
    // Once per down-stretch, like Ch9329Inject's: a dropped key or click
    // leaves a trace without a line per mouse move.
    bool loggedDropWhileDown_ = false;

    // Checks the board every few seconds, reopening the device if it stops
    // answering, and runs the self-test on request.
    std::thread             watchdog_;
    std::atomic<bool>       running_{false};
    std::mutex              cvMutex_;
    std::condition_variable cv_;
    void watchdogLoop();
    // Watchdog thread only.
    bool linkDownLogged_ = false;
    std::optional<std::chrono::steady_clock::time_point> lastBaudScan_;

    // ── Health (see health()) ──
    // Guarded by healthMutex_, taken after ioMutex_ when both are held, so
    // health() never waits on I/O.
    struct HealthEvent {
        enum class Kind { CheckOk, CheckFailed, Reconnect, Dropped };
        std::chrono::steady_clock::time_point at;
        Kind   kind;
        double ms    = 0;   // CheckOk: reply time
        int    count = 1;   // Dropped: coalesced per second
    };
    mutable std::mutex              healthMutex_;
    mutable std::deque<HealthEvent> events_;   // last hour, oldest first
    bool                   probedOnce_ = false;
    std::optional<bool>    targetEnumerated_;
    std::optional<uint8_t> outputs_;
    std::optional<int>     answersAtBaud_;
    InputSelfTest          test_;
    std::atomic<bool>      testRequested_{false};
    void recordEvent(HealthEvent::Kind kind, double ms = 0);
    void pruneEventsLocked() const;
    // What every good reply says about the board, kept for health().
    void noteStatus(const Reply& r);
    void runSelfTest();   // watchdog thread

    // All assume ioMutex_ is already held by the caller.
    bool openSerialLocked();
    void closeSerialLocked();
    // Sends one request and returns the first well-formed reply, if one
    // comes within `timeout`.
    std::optional<Reply> exchangeLocked(const Frame& request, std::chrono::milliseconds timeout);
    // Says nothing for longer than the firmware waits on a partial frame, so
    // that it drops whatever it was holding, then discards what came back.
    // Without this, one lost byte would leave every later frame misaligned
    // for as long as input keeps flowing.
    void resyncLocked();
    // A status check, counted in the passive health window.
    bool probeAliveLocked();
    // Looks for the board at every other standard rate, restoring the
    // configured one afterwards. Returns the rate it answered at, if any.
    std::optional<int> scanBaudLocked();

    // Sends an input command, up to `attempts` times until it's done, as
    // `copies` separate commands, and counts it as dropped only if none was
    // done. Only commands the firmware can apply twice to the same effect may
    // have more than one attempt or copy (a key or button state, a position,
    // a clear — never a scroll step).
    void send(const Frame& request, int attempts, int copies = 1);
    // One copy. A board that has stopped answering is marked not ready and
    // the watchdog asked to check it now, and commands are dropped at once
    // until it answers again, rather than each waiting out its timeouts.
    bool sendOnce(const Frame& request, int attempts);
    std::atomic<bool> probeRequested_{false};
};

} // namespace houston_kvm
