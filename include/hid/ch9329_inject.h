#pragma once

#include "hid/input_backend.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace houston_kvm {

class Ch9329Inject final : public InputBackend {
public:
    explicit Ch9329Inject(const std::string& serialDevice, int baudRate = 9600);
    ~Ch9329Inject() override;

    Ch9329Inject(const Ch9329Inject&) = delete;
    Ch9329Inject& operator=(const Ch9329Inject&) = delete;

    bool isReady() const override;
    const char* kind() const override { return "ch9329"; }
    InputHealth health() const override;
    bool startSelfTest() override;
    bool hasSelfTest() const override { return true; }
    InputSelfTest selfTest() const override;
    bool startBaudChange(int baud, std::function<void(int)> onSwitched) override;
    bool canChangeBaud() const override { return true; }
    InputBaudChange baudChange() const override;

    void mouseMoveAbsolute(double x, double y) override;
    void mouseButton(int button, bool pressed) override;
    void mouseScroll(int delta) override;
    void keyEvent(const std::string& jsCode, bool pressed) override;
    void releaseAll() override;
    std::chrono::milliseconds typingInterval() const override;
    static std::vector<uint8_t> buildPacket(uint8_t cmd, const uint8_t* data, uint8_t len);

    static uint8_t jsCodeToHid(const std::string& code, bool& isModifier, uint8_t& modBit);

private:
    const std::string device_;
    // Changes only in runBaudChange(), with ioMutex_ held; atomic because
    // health() reads it without that lock.
    std::atomic<int>  baudRate_;

    // Guards fd_ and all I/O on it — sendCommand() (driven by InputQueue's
    // worker thread) and the watchdog thread below both touch the fd, since
    // the watchdog can close/reopen it out from under an in-flight command.
    mutable std::mutex ioMutex_;
    int fd_ = -1;
    // Mirrors of the link state for isReady(), which the event-loop thread
    // calls: it must not wait on ioMutex_, which a probe holds for up to
    // kProbeTimeout. Ready means the device is open AND the chip answered
    // the last probe — an open port at the wrong baud rate isn't ready.
    std::atomic<bool> deviceOpen_{false};
    std::atomic<bool> lastProbeOk_{false};
    // True once sendCommand() has already logged a dropped command for the
    // current down-stretch. Dropped clicks/keys must leave a trace (see
    // sendCommand()), but logging every drop unconditionally would spam once
    // per queued event (up to 60/s of mousemove) for as long as the link
    // stays down. Reset whenever openSerialLocked() succeeds, so the next
    // drop after a fresh disconnect logs again.
    bool loggedDropWhileDown_ = false;
    // Guards logParaCfgOnceBestEffort() — it's called both from the
    // constructor and from every watchdogLoop() cycle where the link is
    // alive (the GET_INFO handshake flakes often enough that a single
    // constructor-time attempt isn't reliable), so it retries a bounded
    // number of times rather than giving up (or spamming) on one failure.
    int paraCfgAttemptsLeft_ = 5;

    uint8_t modifiers_     = 0;
    std::array<uint8_t, 6> keys_{};
    uint8_t mouse_buttons_ = 0;
    // Last absolute position actually sent, in CH9329's 12-bit (0-4095)
    // device space. The chip's report is stateful-per-write, not a delta,
    // so a button/scroll-only report must resend the last known coordinates
    // or the target's cursor snaps back to wherever it was before.
    uint16_t mouse_x_ = 2048;
    uint16_t mouse_y_ = 2048;

    // Periodically confirms the chip is still answering GET_INFO and
    // transparently reopens the serial device if it stops responding (e.g.
    // the target PC rebooted and the bridge dropped off), instead of
    // requiring a physical replug. See std::atomic<bool> running_ / cv_ for
    // shutdown signaling.
    std::thread             watchdog_;
    std::atomic<bool>       running_{false};
    std::mutex              cvMutex_;
    std::condition_variable cv_;
    void watchdogLoop();
    // Watchdog thread only.
    bool linkDownLogged_ = false;
    std::chrono::steady_clock::time_point lastBaudScan_{};

    // ── Health (see health()) ──
    // Guarded by healthMutex_, never held while taking ioMutex_ (the order
    // is always ioMutex_ then healthMutex_), so health() never waits on I/O.
    struct HealthEvent {
        enum class Kind { CheckOk, CheckFailed, Reconnect, Dropped };
        std::chrono::steady_clock::time_point at;
        Kind   kind;
        double ms    = 0;   // CheckOk: reply time
        int    count = 1;   // Dropped: coalesced per second
    };
    mutable std::mutex              healthMutex_;
    mutable std::deque<HealthEvent> events_;   // last hour, oldest first
    bool                probedOnce_ = false;
    std::optional<bool> targetEnumerated_;
    std::optional<int>  firmwareVersion_;
    std::optional<int>  chipBaud_;
    std::optional<int>  answersAtBaud_;
    InputSelfTest       test_;
    std::atomic<bool>   testRequested_{false};
    void recordEvent(HealthEvent::Kind kind, double ms = 0);
    void pruneEventsLocked() const;
    void runSelfTest();   // watchdog thread
    // Guarded by healthMutex_, like test_.
    InputBaudChange            baudChange_;
    int                        requestedBaud_ = 0;
    std::function<void(int)>   onBaudSwitched_;
    std::atomic<bool>          baudRequested_{false};
    void runBaudChange();   // watchdog thread

    // All assume ioMutex_ is already held by the caller.
    bool openSerialLocked();
    void closeSerialLocked();
    bool setSpeedLocked(int baud);
    // `record` adds the result to the passive health window; the self-test
    // keeps its burst out of it.
    bool probeAliveLocked(bool record = true);
    // Flushes stale input, sends cmd, and returns the payload of the chip's
    // reply to it — skipping replies to other commands that arrive first.
    std::optional<std::vector<uint8_t>> requestLocked(uint8_t cmd, std::chrono::milliseconds timeout,
                                                      const uint8_t* data = nullptr, uint8_t len = 0);
    std::optional<std::vector<uint8_t>> readFrameLocked(uint8_t expectedCmd, std::chrono::milliseconds timeout);
    // Looks for the chip at every other standard rate, restoring baudRate_
    // afterwards. Returns the rate it answered at, if any.
    std::optional<int> scanBaudLocked();
    // Writes `cfg` (whose bytes 3-6 name `to`) while the chip answers at
    // `from`, then gets it answering at `to`. Leaves the port at `to` on
    // success and at `from` otherwise.
    bool writeConfigAndSwitchLocked(const std::vector<uint8_t>& cfg, int from, int to);
    // Startup diagnostic, retried until it succeeds once: reads the chip's
    // stored configuration (CMD_GET_PARA_CFG) and logs its working and serial
    // modes, which a board can ship set unexpectedly. The byte layout is in
    // ch9329_inject.cpp.
    void logParaCfgOnceBestEffort();

    void sendKeyReport();
    // absolute mode. `redundant` sends the identical report a second time —
    // safe only because these reports are full-state snapshots, not deltas,
    // so a repeat is a no-op on the target. Used for mouseButton() (a lost
    // click has no self-correction, unlike movement) but never for plain
    // cursor movement, which stays single-shot on purpose — see sendCommand().
    void sendMouseReport(int8_t wheel, bool redundant = false);
    // Buttons sent in the absolute report (CMD 0x04) never reach the target,
    // though absolute movement does: the chip appears to carry buttons only
    // in its relative report. So the button state also goes out as a
    // zero-delta relative report, alongside the absolute one.
    void sendRelativeButtonReport();
    // Fire-and-forget: writes cmd without waiting for the chip's ACK.
    // Waiting would queue every mouse move behind the input worker, and on a
    // flaky link that backlog becomes visible cursor lag. A lost move is
    // corrected by the next one; clicks and keys get their guarantee from
    // redundant sends instead, which cost a write, not a wait.
    void sendCommand(uint8_t cmd, const uint8_t* data, uint8_t len);
};

}
