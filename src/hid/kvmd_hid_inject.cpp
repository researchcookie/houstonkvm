#include "hid/kvmd_hid_inject.h"

#include "hid/input_inject.h"
#include "hid/serial_link.h"

#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <ctime>
#include <iostream>
#include <vector>

namespace houston_kvm {

namespace {

// The firmware's protocol, as its source defines it (hid/pico/src/ph_proto.h
// and hid/arduino/src/proto.h in the kvmd repository, as of kvmd 4.227).
constexpr uint8_t kRequestMagic = 0x33;
constexpr uint8_t kReplyMagic   = 0x34;

constexpr uint8_t CMD_PING         = 0x01;
constexpr uint8_t CMD_CLEAR_HID    = 0x10;
constexpr uint8_t CMD_KEY          = 0x11;
constexpr uint8_t CMD_MOUSE_ABS    = 0x12;
constexpr uint8_t CMD_MOUSE_BUTTON = 0x13;
constexpr uint8_t CMD_MOUSE_WHEEL  = 0x14;

// Reply codes without bit 7: the request wasn't carried out.
constexpr uint8_t RESP_INVALID = 0x45;   // a command this firmware doesn't know

// Status flags in a done reply's code byte.
constexpr uint8_t PONG_KBD_OFFLINE   = 0x08;
constexpr uint8_t PONG_MOUSE_OFFLINE = 0x10;

// The active modes, in a done reply's outputs byte.
constexpr uint8_t OUT_DYNAMIC    = 0x80;   // the board's modes can be switched
constexpr uint8_t OUT_KBD_MASK   = 0x07;
constexpr uint8_t OUT_MOUSE_MASK = 0x38;
constexpr uint8_t OUT_MOUSE_ABS  = 0x08;
constexpr uint8_t OUT_MOUSE_W98  = 0x20;   // absolute too, scaled for Windows 98

// The firmware drops a partial frame after 100 ms without a byte, so a
// little longer than that in silence lines the next request up.
constexpr auto kResyncPause  = std::chrono::milliseconds(120);
// The firmware answers within a couple of milliseconds; these are for
// replies that aren't coming.
constexpr auto kReplyMargin  = std::chrono::milliseconds(100);
constexpr auto kProbeTimeout = std::chrono::milliseconds(500);
constexpr auto kHealthyCheckInterval = std::chrono::milliseconds(5000);
constexpr auto kInitialBackoff       = std::chrono::milliseconds(1000);
constexpr auto kMaxBackoff           = std::chrono::milliseconds(20000);

// Health: the same window and thresholds as Ch9329Inject, so the two read
// alike in the directory.
constexpr auto kHealthWindow          = std::chrono::hours(1);
constexpr int  kDegradedAfterFailures = 3;
constexpr int  kSelfTestProbes        = 20;

struct LinkFacts {
    bool                   deviceOpen;
    bool                   measured;
    bool                   lastOk;
    std::optional<bool>    targetEnumerated;
    std::optional<uint8_t> outputs;
    std::optional<int>     answersAtBaud;
    int                    configuredBaud;
    int                    ok, failed, reconnects;
    int                    degradedAfter;
};

// Turns what was measured into a verdict and which part to look at, shared
// by the passive health view and the on-demand test.
std::pair<HealthState, std::string> classifyLink(const LinkFacts& f) {
    if (!f.deviceOpen)
        return {HealthState::Down,
                "Can't open the serial device: the USB-serial adapter is unplugged, "
                "or the device path is wrong."};
    if (!f.measured)
        return {HealthState::Unknown, "Not checked yet."};
    if (!f.lastOk && f.answersAtBaud)
        return {HealthState::Down,
                "The board answers at " + std::to_string(*f.answersAtBaud) +
                " baud, but this target is set to " + std::to_string(f.configuredBaud) +
                ": change the target's serial baud rate to match."};
    if (!f.lastOk)
        return {HealthState::Down,
                "The board isn't answering: check that the target machine is on (it powers "
                "the board), the wires from the USB-serial adapter (its TX to the board's RX, "
                "RX to TX, and ground), and that the board runs kvmd's HID firmware on its "
                "serial pins (on a Pico, GP22 wired to ground)."};
    if (f.targetEnumerated == false)
        return {HealthState::Down,
                "The board answers, but the target machine hasn't recognised it as a "
                "keyboard and mouse: check the USB cable to the target, and that the "
                "target is powered on."};
    // A board built with switchable modes sets OUT_DYNAMIC; one built for
    // fixed modes reports them without it, and an old firmware reports none.
    if (f.outputs && (*f.outputs & (OUT_DYNAMIC | OUT_KBD_MASK | OUT_MOUSE_MASK))) {
        const uint8_t mouse = *f.outputs & OUT_MOUSE_MASK;
        if ((*f.outputs & OUT_KBD_MASK) == 0 || mouse == 0)
            return {HealthState::Down,
                    "The board answers, but its keyboard or mouse is switched off in the "
                    "firmware: switch both back on (on a Pico, nothing wired to GP2 to GP9), "
                    "then unplug the board and plug it back in."};
        if (mouse != OUT_MOUSE_ABS && mouse != OUT_MOUSE_W98)
            return {HealthState::Down,
                    "The board answers, but its mouse is in relative or PS/2 mode, and "
                    "HoustonKVM can only place the pointer absolutely: set it back to an "
                    "absolute USB mouse (on a Pico, nothing wired to GP2 to GP9), then "
                    "unplug the board and plug it back in."};
    }
    if (f.failed >= f.degradedAfter || f.reconnects >= 2) {
        std::string s = std::to_string(f.failed) + " of " + std::to_string(f.ok + f.failed) +
                        " checks failed";
        if (f.reconnects) s += " (" + std::to_string(f.reconnects) + " reconnects)";
        return {HealthState::Degraded,
                s + ": likely a loose wire or a failing cable or connector. Reseat it, or "
                    "swap it to confirm."};
    }
    return {HealthState::Good, "Board and target link OK."};
}

bool enumeratedFrom(uint8_t code) {
    return !(code & (PONG_KBD_OFFLINE | PONG_MOUSE_OFFLINE));
}

// The firmware numbers keys in its own order (kvmd's keymap.csv), not by USB
// usage. This is the inverse of its table, for every usage the browser key
// table can produce, in runs where both orders agree.
uint8_t keyNumberForUsage(uint8_t usage) {
    if (usage >= 0x04 && usage <= 0x31) return usage - 3;   // KeyA … Backslash
    if (usage >= 0x33 && usage <= 0x46) return usage - 4;   // Semicolon … PrintScreen
    if (usage >= 0x49 && usage <= 0x52) return usage - 6;   // Insert … ArrowUp
    if (usage >= 0x54 && usage <= 0x63) return usage + 5;   // NumpadDivide … NumpadDecimal
    switch (usage) {
        case 0x47: return 86;    // ScrollLock
        case 0x48: return 85;    // Pause
        case 0x53: return 87;    // NumLock
        case 0x64: return 106;   // IntlBackslash
    }
    return 0;
}

} // namespace

KvmdHidInject::KvmdHidInject(const std::string& serialDevice, int baudRate)
    : device_(serialDevice), baudRate_(baudRate),
      replyTimeout_(kReplyMargin +
                    std::chrono::milliseconds(16 * 10 * 1000 / std::max(1, baudRate))) {
    {
        std::lock_guard<std::mutex> lock(ioMutex_);
        if (openSerialLocked()) {
            if (!probeAliveLocked())
                std::cerr << "KvmdHidInject: " << device_
                          << " opened but the board did not answer — check wiring/baud"
                             " rate; will keep retrying in the background\n";
            else
                std::cout << "KvmdHidInject: " << device_ << " self-test OK\n";
        }
    }

    running_  = true;
    watchdog_ = std::thread([this] { watchdogLoop(); });
}

KvmdHidInject::~KvmdHidInject() {
    running_ = false;
    cv_.notify_all();
    if (watchdog_.joinable()) watchdog_.join();

    std::lock_guard<std::mutex> lock(ioMutex_);
    closeSerialLocked();
}

bool KvmdHidInject::isReady() const {
    return deviceOpen_.load() && lastProbeOk_.load();
}

bool KvmdHidInject::openSerialLocked() {
    std::string error;
    fd_ = openRawSerial(device_, baudRate_, error, /*lowLatency=*/true);
    if (fd_ < 0) {
        std::cerr << "KvmdHidInject: " << error << "\n";
        return false;
    }
    loggedDropWhileDown_ = false;
    deviceOpen_ = true;
    if (!linkDownLogged_)
        std::cout << "KvmdHidInject: connected to " << device_ << " @ " << baudRate_ << " baud\n";
    return true;
}

void KvmdHidInject::closeSerialLocked() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    deviceOpen_  = false;
    lastProbeOk_ = false;
}

KvmdHidInject::Frame KvmdHidInject::buildRequest(uint8_t cmd, uint8_t a0, uint8_t a1, uint8_t a2,
                                             uint8_t a3) {
    Frame f{kRequestMagic, cmd, a0, a1, a2, a3, 0, 0};
    uint16_t crc = crc16(f.data(), 6);
    f[6] = static_cast<uint8_t>(crc >> 8);
    f[7] = static_cast<uint8_t>(crc & 0xFF);
    return f;
}

uint16_t KvmdHidInject::crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001)
                            : static_cast<uint16_t>(crc >> 1);
    }
    return crc;
}

// A reply is 0x34 CODE OUT1 OUT2 0 0 CRC CRC. The firmware sends one per
// frame it receives, and one more if it gives up on a partial frame, so
// stale input is flushed before the request goes out and anything that
// isn't a well-formed reply is skipped.
std::optional<KvmdHidInject::Reply> KvmdHidInject::exchangeLocked(const Frame& request,
                                                             std::chrono::milliseconds timeout) {
    if (fd_ < 0) return std::nullopt;
    tcflush(fd_, TCIFLUSH);
    if (!writeAllRetrying(fd_, request.data(), request.size())) {
        if (!loggedDropWhileDown_) {
            loggedDropWhileDown_ = true;
            std::cerr << "KvmdHidInject: write to " << device_ << " failed: " << strerror(errno)
                      << "\n";
        }
        return std::nullopt;
    }

    std::vector<uint8_t> buf;
    auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        for (;;) {
            auto start = std::find(buf.begin(), buf.end(), kReplyMagic);
            buf.erase(buf.begin(), start);
            if (buf.size() < 8) break;
            if (crc16(buf.data(), 6) == ((buf[6] << 8) | buf[7]))
                return Reply{buf[1], buf[2]};
            buf.erase(buf.begin());   // not a reply after all; resync
        }

        auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remainingMs <= 0) return std::nullopt;
        pollfd pfd{fd_, POLLIN, 0};
        if (poll(&pfd, 1, static_cast<int>(remainingMs)) <= 0) return std::nullopt;
        uint8_t chunk[64];
        ssize_t n = read(fd_, chunk, sizeof(chunk));
        if (n <= 0) return std::nullopt;
        buf.insert(buf.end(), chunk, chunk + n);
    }
}

void KvmdHidInject::resyncLocked() {
    std::this_thread::sleep_for(kResyncPause);
    if (fd_ >= 0) tcflush(fd_, TCIFLUSH);
}

bool KvmdHidInject::probeAliveLocked() {
    auto start = std::chrono::steady_clock::now();
    auto reply = exchangeLocked(buildRequest(CMD_PING), kProbeTimeout);
    double ms  = std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - start).count();
    bool ok = reply && reply->ok();
    if (!ok) resyncLocked();
    lastProbeOk_ = ok;
    if (ok) loggedDropWhileDown_ = false;

    {
        std::lock_guard<std::mutex> lock(healthMutex_);
        probedOnce_ = true;
        if (ok) answersAtBaud_.reset();
    }
    if (ok) noteStatus(*reply);
    recordEvent(ok ? HealthEvent::Kind::CheckOk : HealthEvent::Kind::CheckFailed, ms);
    return ok;
}

std::optional<int> KvmdHidInject::scanBaudLocked() {
    std::optional<int> found;
    for (int baud : {115200, 9600, 57600, 38400, 19200, 4800, 2400, 1200}) {
        if (!running_.load()) break;   // being closed: don't hold it up
        if (baud == baudRate_ || !setSerialSpeed(fd_, baud)) continue;
        // What went out at the last rate may have left the firmware holding
        // part of a frame.
        resyncLocked();
        auto reply = exchangeLocked(buildRequest(CMD_PING), std::chrono::milliseconds(200));
        if (reply && reply->ok()) {
            found = baud;
            break;
        }
    }
    setSerialSpeed(fd_, baudRate_);
    resyncLocked();
    return found;
}

void KvmdHidInject::watchdogLoop() {
    auto backoff = kInitialBackoff;

    while (running_.load()) {
        bool alive;
        {
            std::lock_guard<std::mutex> lock(ioMutex_);
            if (fd_ < 0) {
                alive = openSerialLocked() && probeAliveLocked();
            } else {
                alive = probeAliveLocked();
                if (!alive) {
                    if (!linkDownLogged_)
                        std::cerr << "KvmdHidInject: " << device_
                                  << " stopped answering — reconnecting (target off or"
                                     " rebooting?)\n";
                    linkDownLogged_ = true;
                    recordEvent(HealthEvent::Kind::Reconnect);
                    closeSerialLocked();
                    alive = openSerialLocked() && probeAliveLocked();
                }
            }
            // Still silent on an open port: find out whether it's the baud
            // rate, at most every few minutes (a scan holds input up for
            // about two seconds, though none gets through like this anyway).
            auto now = std::chrono::steady_clock::now();
            if (!alive && fd_ >= 0 &&
                (!lastBaudScan_ || now - *lastBaudScan_ > std::chrono::minutes(5))) {
                lastBaudScan_ = now;
                if (auto rate = scanBaudLocked()) {
                    std::cerr << "KvmdHidInject: " << device_ << " answers at " << *rate
                              << " baud, but this target is set to " << baudRate_
                              << " — change the target's serial baud rate\n";
                    std::lock_guard<std::mutex> hl(healthMutex_);
                    answersAtBaud_ = *rate;
                }
            }
            if (alive && linkDownLogged_) {
                std::cerr << "KvmdHidInject: " << device_ << " answering again @ " << baudRate_
                          << " baud\n";
                linkDownLogged_ = false;
            }
        }

        if (testRequested_.exchange(false)) runSelfTest();

        auto wait = alive ? kHealthyCheckInterval : backoff;
        backoff   = alive ? kInitialBackoff : std::min(backoff * 2, kMaxBackoff);

        std::unique_lock<std::mutex> lock(cvMutex_);
        cv_.wait_for(lock, wait, [this] {
            return !running_.load() || testRequested_.load() || probeRequested_.load();
        });
        probeRequested_ = false;
    }
}

// ── Health ───────────────────────────────────────────────────────────────────

void KvmdHidInject::recordEvent(HealthEvent::Kind kind, double ms) {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(healthMutex_);
    if (kind == HealthEvent::Kind::Dropped && !events_.empty() &&
        events_.back().kind == kind && now - events_.back().at < std::chrono::seconds(1)) {
        ++events_.back().count;
    } else {
        events_.push_back({now, kind, ms, 1});
    }
    pruneEventsLocked();
}

void KvmdHidInject::pruneEventsLocked() const {
    auto cutoff = std::chrono::steady_clock::now() - kHealthWindow;
    while (!events_.empty() && events_.front().at < cutoff) events_.pop_front();
}

void KvmdHidInject::noteStatus(const Reply& r) {
    std::lock_guard<std::mutex> lock(healthMutex_);
    targetEnumerated_ = enumeratedFrom(r.code);
    outputs_          = r.outputs;
}

InputHealth KvmdHidInject::health() const {
    InputHealth h;
    h.kind           = kind();
    h.configuredBaud = baudRate_;
    h.deviceOpen     = deviceOpen_.load();

    std::lock_guard<std::mutex> lock(healthMutex_);
    pruneEventsLocked();
    double msSum = 0;
    for (const auto& e : events_) {
        switch (e.kind) {
            case HealthEvent::Kind::CheckOk:
                ++h.checksOk;
                msSum += e.ms;
                h.replyMsMax = std::max(h.replyMsMax, e.ms);
                break;
            case HealthEvent::Kind::CheckFailed: ++h.checksFailed;             break;
            case HealthEvent::Kind::Reconnect:   ++h.reconnects;               break;
            case HealthEvent::Kind::Dropped:     h.droppedCommands += e.count; break;
        }
    }
    if (h.checksOk) h.replyMsAvg = msSum / h.checksOk;
    h.targetEnumerated = targetEnumerated_;
    h.answersAtBaud    = answersAtBaud_;

    auto [state, summary] = classifyLink({*h.deviceOpen, probedOnce_, lastProbeOk_.load(),
                                          targetEnumerated_, outputs_, answersAtBaud_, baudRate_,
                                          h.checksOk, h.checksFailed, h.reconnects,
                                          kDegradedAfterFailures});
    h.state   = state;
    h.summary = std::move(summary);
    return h;
}

bool KvmdHidInject::startSelfTest() {
    {
        std::lock_guard<std::mutex> lock(healthMutex_);
        if (test_.phase == InputSelfTest::Phase::Running) return false;
        test_       = {};
        test_.phase = InputSelfTest::Phase::Running;
    }
    {
        std::lock_guard<std::mutex> lock(cvMutex_);
        testRequested_ = true;
    }
    cv_.notify_all();
    return true;
}

InputSelfTest KvmdHidInject::selfTest() const {
    std::lock_guard<std::mutex> lock(healthMutex_);
    return test_;
}

// A burst of probes, each taking ioMutex_ on its own so input queued
// meanwhile isn't held up for the whole burst. The firmware has no stored
// configuration to read back, unlike a CH9329; its modes come with every
// reply.
void KvmdHidInject::runSelfTest() {
    InputSelfTest t;
    t.phase = InputSelfTest::Phase::Done;
    double msSum = 0;
    bool open = false;
    std::optional<uint8_t> outputs;
    for (int i = 0; i < kSelfTestProbes && running_.load(); ++i) {
        {
            std::lock_guard<std::mutex> lock(ioMutex_);
            open = fd_ >= 0;
            if (!open) break;
            ++t.probes;
            auto start = std::chrono::steady_clock::now();
            auto reply = exchangeLocked(buildRequest(CMD_PING), kProbeTimeout);
            double ms  = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - start).count();
            if (reply && reply->ok()) {
                ++t.ok;
                msSum += ms;
                t.replyMsMax       = std::max(t.replyMsMax, ms);
                t.targetEnumerated = enumeratedFrom(reply->code);
                outputs            = reply->outputs;
            } else {
                resyncLocked();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (t.ok) t.replyMsAvg = msSum / t.ok;

    std::optional<int> answersAt;
    if (open && !t.ok) {
        std::lock_guard<std::mutex> lock(ioMutex_);
        if (fd_ >= 0) answersAt = scanBaudLocked();
    }

    auto [state, verdict] = classifyLink({open, true, t.ok > 0, t.targetEnumerated, outputs,
                                          answersAt, baudRate_, t.ok, t.probes - t.ok, 0, 1});
    t.verdictState   = state;
    t.verdict        = std::move(verdict);
    t.finishedAtUnix = static_cast<int64_t>(std::time(nullptr));

    std::lock_guard<std::mutex> lock(healthMutex_);
    test_ = std::move(t);
}

// ── Input ────────────────────────────────────────────────────────────────────

void KvmdHidInject::send(const Frame& request, int attempts, int copies) {
    bool done = false;
    for (int i = 0; i < copies; ++i) done |= sendOnce(request, attempts);
    if (!done) recordEvent(HealthEvent::Kind::Dropped);
}

bool KvmdHidInject::sendOnce(const Frame& request, int attempts) {
    bool done = false, quiet = false;
    {
        std::lock_guard<std::mutex> lock(ioMutex_);
        if (fd_ < 0 || !lastProbeOk_) {
            if (!loggedDropWhileDown_) {
                loggedDropWhileDown_ = true;
                std::cerr << "KvmdHidInject: dropping cmd=0x" << std::hex
                          << static_cast<int>(request[1]) << std::dec
                          << " — the board isn't answering (checking it again)\n";
            }
            return false;
        }
        bool invalid = false;
        for (int i = 0; i < attempts && !done && !invalid; ++i) {
            auto reply = exchangeLocked(request, replyTimeout_);
            if (reply && reply->ok()) {
                noteStatus(*reply);
                done = true;
            } else if (reply && reply->code == RESP_INVALID) {
                invalid = true;   // the firmware doesn't know it: sending it again won't help
            } else {
                resyncLocked();
            }
        }
        if (!done && !invalid) {
            lastProbeOk_ = false;
            quiet = true;
        }
    }
    if (quiet) {
        {
            std::lock_guard<std::mutex> lock(cvMutex_);
            probeRequested_ = true;
        }
        cv_.notify_all();
    }
    return done;
}

void KvmdHidInject::mouseMoveAbsolute(double x, double y) {
    // The firmware takes each axis as -32768 … 32767 and scales it onto its
    // own report. One attempt: a lost position is corrected by the next.
    auto axis = [](double v) {
        return static_cast<int16_t>(std::lround(std::clamp(v, 0.0, 1.0) * 65535.0) - 32768);
    };
    auto ax = static_cast<uint16_t>(axis(x));
    auto ay = static_cast<uint16_t>(axis(y));
    send(buildRequest(CMD_MOUSE_ABS, ax >> 8, ax & 0xFF, ay >> 8, ay & 0xFF), 1);
}

void KvmdHidInject::mouseButton(int button, bool pressed) {
    if (button < 0 || button > 2) return;
    // DOM MouseEvent.button is 0=left, 1=middle, 2=right. The firmware takes
    // a "this button" bit and a "pressed" bit for each, so one command
    // changes one button and leaves the others as they are.
    static constexpr uint8_t kSelect[3]  = {0x80, 0x20, 0x40};
    static constexpr uint8_t kPressed[3] = {0x08, 0x02, 0x04};
    auto bits    = static_cast<uint8_t>(kSelect[button] | (pressed ? kPressed[button] : 0));
    auto request = buildRequest(CMD_MOUSE_BUTTON, bits);
    // Sent twice. The reply only says the firmware took the command: a Pico
    // drops a mouse report it can't hand to the target's USB poll at that
    // moment, and a lost click, unlike a lost move, has no next report to
    // correct it. The repeat changes nothing if the first one landed.
    send(request, 2, /*copies=*/2);
}

void KvmdHidInject::mouseScroll(int delta) {
    if (delta == 0) return;
    auto wheel = static_cast<int8_t>(std::clamp(delta, -127, 127));
    // Horizontal first, which the firmware ignores. One attempt only: a
    // repeated step would scroll twice.
    send(buildRequest(CMD_MOUSE_WHEEL, 0, static_cast<uint8_t>(wheel)), 1);
}

void KvmdHidInject::keyEvent(const std::string& jsCode, bool pressed) {
    uint8_t key = keyNumber(jsCode);
    if (key == 0) return;
    // The firmware ignores a press of a key already down, so the browser's
    // key repeat is harmless.
    send(buildRequest(CMD_KEY, key, pressed ? 1 : 0), 2);
}

void KvmdHidInject::releaseAll() {
    // CLEAR lets go of every key and button. The firmware's own report for
    // it keeps the last position it was sent, so the pointer stays put.
    // Sent twice, for the same reason as a click (see mouseButton()), and
    // always, even when nothing should be held: a key-up lost on the way
    // leaves the target holding a key this side has forgotten.
    send(buildRequest(CMD_CLEAR_HID), 2, /*copies=*/2);
}

std::chrono::milliseconds KvmdHidInject::typingInterval() const {
    // One keyEvent() is an 8-byte request and an 8-byte reply, 10 bits a
    // byte on the wire. Leave a quarter again on top, and never go below
    // the default, which gives the target's USB poll time for each report.
    // Under 2 ms at 115200 baud, so the default it is.
    int baud    = std::max(1, baudRate_);
    auto wireMs = (16 * 10 * 1000 * 5 / 4 + baud - 1) / baud;
    return std::max(InputBackend::typingInterval(), std::chrono::milliseconds(wireMs));
}

uint8_t KvmdHidInject::keyNumber(const std::string& jsCode) {
    // The same browser-code-to-USB-usage table every backend uses, then the
    // firmware's numbering.
    bool    isModifier = false;
    uint8_t modBit     = 0;
    uint8_t usage      = InputInject::jsCodeToHid(jsCode, isModifier, modBit);
    if (isModifier)
        // Modifier bit n is usage 0xE0 + n, which the firmware numbers 77 + n.
        return static_cast<uint8_t>(77 + std::countr_zero(modBit));
    return keyNumberForUsage(usage);
}

} // namespace houston_kvm
