#include "hid/ch9329_inject.h"

#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace houston_kvm {

namespace {

// CH9329 command codes (from WCH's CH9329 UART communication protocol).
constexpr uint8_t CMD_GET_INFO             = 0x01;
constexpr uint8_t CMD_SEND_KB_GENERAL_DATA = 0x02;
constexpr uint8_t CMD_SEND_MS_ABS_DATA     = 0x04;
constexpr uint8_t CMD_SEND_MS_REL_DATA     = 0x05;
constexpr uint8_t CMD_GET_PARA_CFG         = 0x08;
constexpr uint8_t CMD_SET_PARA_CFG         = 0x09;
constexpr uint8_t CMD_RESET                = 0x0F;
constexpr uint8_t CMD_RESPONSE_BIT         = 0x80;

// CH9329 absolute-mouse coordinate space is 12-bit (0-4095) per axis,
// scaled proportionally across whatever resolution the target believes its
// screen is.
constexpr uint16_t kCh9329AbsMax = 4095;

constexpr auto kHealthyCheckInterval = std::chrono::milliseconds(5000);
constexpr auto kInitialBackoff       = std::chrono::milliseconds(1000);
constexpr auto kMaxBackoff           = std::chrono::milliseconds(20000);
constexpr auto kProbeTimeout         = std::chrono::milliseconds(1500);

// Health: the passive window, how many failed checks in it make a link
// "degraded" (one is a blip; three in an hour is a cable worth a look), and
// the size of the on-demand test's burst.
constexpr auto kHealthWindow          = std::chrono::hours(1);
constexpr int  kDegradedAfterFailures = 3;
constexpr int  kSelfTestProbes        = 20;

struct LinkFacts {
    bool                deviceOpen;
    bool                measured;
    bool                lastOk;
    std::optional<bool> targetEnumerated;
    std::optional<int>  answersAtBaud;
    int                 configuredBaud;
    int                 ok, failed, reconnects;
    int                 degradedAfter;
};

// One place that turns what we measured into a verdict and which cable to
// look at, shared by the passive health view and the on-demand test.
std::pair<HealthState, std::string> classifyLink(const LinkFacts& f) {
    if (!f.deviceOpen)
        return {HealthState::Down,
                "Can't open the serial device: the USB-serial adapter is unplugged, "
                "or the device path is wrong."};
    if (!f.measured)
        return {HealthState::Unknown, "Not checked yet."};
    if (!f.lastOk && f.answersAtBaud)
        return {HealthState::Down,
                "The adapter answers at " + std::to_string(*f.answersAtBaud) +
                " baud, but this target is set to " + std::to_string(f.configuredBaud) +
                ": change the target's serial baud rate to match."};
    if (!f.lastOk)
        return {HealthState::Down,
                "The adapter isn't answering: check the USB cable between this server "
                "and the adapter, and the adapter itself."};
    if (f.targetEnumerated == false)
        return {HealthState::Down,
                "The adapter answers, but the target machine hasn't recognised it as a "
                "keyboard and mouse: check the USB cable to the target, and that the "
                "target is powered on."};
    if (f.failed >= f.degradedAfter || f.reconnects >= 2) {
        std::string s = std::to_string(f.failed) + " of " + std::to_string(f.ok + f.failed) +
                        " checks failed";
        if (f.reconnects) s += " (" + std::to_string(f.reconnects) + " reconnects)";
        return {HealthState::Degraded,
                s + ": likely a loose or failing cable or connector. Reseat it, or swap "
                    "it to confirm."};
    }
    return {HealthState::Good, "Adapter and target link OK."};
}

// A CH9329 packet is a handful of bytes — normally well within the kernel
// tty output buffer even under load. But clicks and keys go out as
// back-to-back redundant reports, and fd_ is O_NONBLOCK, so at low baud
// rates a write can still come back EAGAIN/short. This
// retries only for local buffer space to free up (poll for POLLOUT) — never
// for the chip/target to respond, so it doesn't reintroduce the round-trip
// ACK-wait latency sendCommand() deliberately avoids (see its declaration).
// Bounded to ~30ms total so one stuck write can't stall the worker thread.
bool writeAllRetrying(int fd, const uint8_t* data, size_t len) {
    size_t sent     = 0;
    auto   deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
    while (sent < len) {
        ssize_t n = write(fd, data + sent, len - sent);
        if (n > 0) {
            sent += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            auto remaining = deadline - std::chrono::steady_clock::now();
            auto remainingMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
            if (remainingMs <= 0) return false;
            pollfd pfd{fd, POLLOUT, 0};
            if (poll(&pfd, 1, static_cast<int>(remainingMs)) <= 0) return false;
            continue;
        }
        return false; // real error (device gone, etc.) — not retryable
    }
    return true;
}

speed_t baudToSpeed(int baudRate) {
    switch (baudRate) {
        case 1200:   return B1200;
        case 2400:   return B2400;
        case 4800:   return B4800;
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        default:
            std::cerr << "Ch9329Inject: unsupported baud rate " << baudRate
                      << ", defaulting to 9600\n";
            return B9600;
    }
}

} // namespace

Ch9329Inject::Ch9329Inject(const std::string& serialDevice, int baudRate)
    : device_(serialDevice), baudRate_(baudRate) {
    {
        std::lock_guard<std::mutex> lock(ioMutex_);
        if (openSerialLocked()) {
            if (!probeAliveLocked()) {
                std::cerr << "Ch9329Inject: " << device_
                          << " opened but did not answer GET_INFO — check"
                             " wiring/baud rate; will keep retrying in the"
                             " background\n";
            } else {
                std::cout << "Ch9329Inject: " << device_ << " self-test OK\n";
                logParaCfgOnceBestEffort();
            }
        }
    }

    running_  = true;
    watchdog_ = std::thread([this] { watchdogLoop(); });
}

Ch9329Inject::~Ch9329Inject() {
    running_ = false;
    cv_.notify_all();
    if (watchdog_.joinable()) watchdog_.join();

    std::lock_guard<std::mutex> lock(ioMutex_);
    closeSerialLocked();
}

bool Ch9329Inject::isReady() const {
    return deviceOpen_.load() && lastProbeOk_.load();
}

bool Ch9329Inject::setSpeedLocked(int baud) {
    termios tty{};
    if (tcgetattr(fd_, &tty) != 0) return false;
    speed_t speed = baudToSpeed(baud);
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);
    return tcsetattr(fd_, TCSANOW, &tty) == 0;
}

bool Ch9329Inject::openSerialLocked() {
    fd_ = open(device_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
        std::cerr << "Ch9329Inject: cannot open " << device_ << ": "
                  << strerror(errno) << "\n";
        return false;
    }

    termios tty{};
    if (tcgetattr(fd_, &tty) != 0) {
        std::cerr << "Ch9329Inject: tcgetattr failed: " << strerror(errno) << "\n";
        close(fd_); fd_ = -1; return false;
    }

    speed_t speed = baudToSpeed(baudRate_);
    cfsetispeed(&tty, speed);
    cfsetospeed(&tty, speed);

    cfmakeraw(&tty);
    tty.c_cflag |= (CLOCAL | CREAD);
    tty.c_cflag &= ~PARENB;   // no parity
    tty.c_cflag &= ~CSTOPB;   // 1 stop bit
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;       // 8 data bits
    tty.c_cflag &= ~CRTSCTS;  // no hardware flow control
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 0;

    if (tcsetattr(fd_, TCSANOW, &tty) != 0) {
        std::cerr << "Ch9329Inject: tcsetattr failed: " << strerror(errno) << "\n";
        close(fd_); fd_ = -1; return false;
    }
    tcflush(fd_, TCIOFLUSH);

    loggedDropWhileDown_ = false;
    deviceOpen_ = true;
    // Quiet while the link is down, so a chip that never answers doesn't
    // log a line per reconnect attempt.
    if (!linkDownLogged_)
        std::cout << "Ch9329Inject: connected to " << device_
                  << " @ " << baudRate_ << " baud\n";
    return true;
}

void Ch9329Inject::closeSerialLocked() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
    deviceOpen_  = false;
    lastProbeOk_ = false;
}

// Frame: 0x57 0xAB ADDR CMD LEN <LEN bytes> SUM. The chip acknowledges
// every command, including the fire-and-forget input reports, so an ack for
// a click sent just before a probe can arrive ahead of the probe's own
// reply. Those frames, and any bytes that don't start a frame, are skipped;
// only a well-formed reply to expectedCmd counts.
std::optional<std::vector<uint8_t>> Ch9329Inject::readFrameLocked(
        uint8_t expectedCmd, std::chrono::milliseconds timeout) {
    std::vector<uint8_t> buf;
    auto deadline = std::chrono::steady_clock::now() + timeout;

    for (;;) {
        // Consume whatever complete frames are buffered.
        for (;;) {
            size_t start = 0;
            while (start + 1 < buf.size() && !(buf[start] == 0x57 && buf[start + 1] == 0xAB))
                ++start;
            buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(start));
            if (buf.size() < 5) break;
            size_t frameLen = 5 + static_cast<size_t>(buf[4]) + 1;
            if (buf.size() < frameLen) break;
            uint32_t sum = 0;
            for (size_t i = 0; i + 1 < frameLen; ++i) sum += buf[i];
            if (static_cast<uint8_t>(sum & 0xFF) != buf[frameLen - 1]) {
                buf.erase(buf.begin());   // not a frame after all; resync
                continue;
            }
            if (buf[3] == (expectedCmd | CMD_RESPONSE_BIT))
                return std::vector<uint8_t>(buf.begin() + 5, buf.begin() + static_cast<std::ptrdiff_t>(frameLen - 1));
            buf.erase(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(frameLen));
        }

        auto remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remainingMs <= 0) return std::nullopt;
        pollfd pfd{fd_, POLLIN, 0};
        if (poll(&pfd, 1, static_cast<int>(remainingMs)) <= 0) return std::nullopt;
        uint8_t chunk[128];
        ssize_t n = read(fd_, chunk, sizeof(chunk));
        if (n <= 0) return std::nullopt;
        buf.insert(buf.end(), chunk, chunk + n);
    }
}

std::optional<std::vector<uint8_t>> Ch9329Inject::requestLocked(
        uint8_t cmd, std::chrono::milliseconds timeout, const uint8_t* data, uint8_t len) {
    if (fd_ < 0) return std::nullopt;
    tcflush(fd_, TCIFLUSH);
    auto pkt = buildPacket(cmd, data, len);
    if (write(fd_, pkt.data(), pkt.size()) != static_cast<ssize_t>(pkt.size()))
        return std::nullopt;
    return readFrameLocked(cmd, timeout);
}

// Startup diagnostic (see header). Also where the chip's stored baud rate
// comes from for health().
void Ch9329Inject::logParaCfgOnceBestEffort() {
    if (fd_ < 0 || paraCfgAttemptsLeft_ <= 0) return;
    --paraCfgAttemptsLeft_;
    auto cfg = requestLocked(CMD_GET_PARA_CFG, kProbeTimeout);
    if (!cfg) {
        std::cerr << "Ch9329Inject: GET_PARA_CFG got no valid response within "
                   << kProbeTimeout.count() << "ms (" << paraCfgAttemptsLeft_
                   << " retries left)\n";
        return;
    }
    if (cfg->size() < 7) {
        std::cerr << "Ch9329Inject: GET_PARA_CFG short response (len="
                   << cfg->size() << ")\n";
        return;
    }
    const auto& c = *cfg;
    // Bytes 3-6 are the chip's baud rate, big-endian.
    int chipBaud = (c[3] << 24) | (c[4] << 16) | (c[5] << 8) | c[6];
    {
        std::lock_guard<std::mutex> lock(healthMutex_);
        chipBaud_ = chipBaud;
    }
    std::ostringstream hex;
    for (uint8_t b : c)
        hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b) << ' ';
    std::cerr << "Ch9329Inject: chip persistent config — work_mode=0x"
               << std::hex << static_cast<int>(c[0])
               << " (0x00/0x80=kbd+mouse combined, 0x01/0x81=kbd only,"
                  " 0x02/0x82=mouse only, 0x03/0x83=custom HID)"
               << " serial_mode=0x" << static_cast<int>(c[1])
               << std::dec << " baud=" << chipBaud
               << "\n  full config bytes: " << hex.str() << "\n";
    paraCfgAttemptsLeft_ = 0; // got it — stop retrying
}

bool Ch9329Inject::probeAliveLocked(bool record) {
    auto start = std::chrono::steady_clock::now();
    auto info  = requestLocked(CMD_GET_INFO, kProbeTimeout);
    double ms  = std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - start).count();
    lastProbeOk_ = info.has_value();
    if (!record) return info.has_value();

    {
        std::lock_guard<std::mutex> lock(healthMutex_);
        probedOnce_ = true;
        // GET_INFO data: firmware version, then USB status (1 = the target
        // has enumerated the chip as a keyboard and mouse).
        if (info && info->size() >= 2) {
            firmwareVersion_  = (*info)[0];
            targetEnumerated_ = (*info)[1] == 0x01;
        }
        if (info) answersAtBaud_.reset();
    }
    recordEvent(info ? HealthEvent::Kind::CheckOk : HealthEvent::Kind::CheckFailed, ms);
    return info.has_value();
}

std::optional<int> Ch9329Inject::scanBaudLocked() {
    std::optional<int> found;
    for (int baud : {9600, 115200, 57600, 38400, 19200, 4800, 2400, 1200}) {
        if (baud == baudRate_ || !setSpeedLocked(baud)) continue;
        if (requestLocked(CMD_GET_INFO, std::chrono::milliseconds(300))) {
            found = baud;
            break;
        }
    }
    setSpeedLocked(baudRate_);
    tcflush(fd_, TCIOFLUSH);
    return found;
}

void Ch9329Inject::watchdogLoop() {
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
                        std::cerr << "Ch9329Inject: " << device_
                                  << " stopped responding to GET_INFO — reconnecting"
                                     " (target reboot/replug?)\n";
                    linkDownLogged_ = true;
                    recordEvent(HealthEvent::Kind::Reconnect);
                    closeSerialLocked();
                    alive = openSerialLocked() && probeAliveLocked();
                }
            }
            // Still silent on an open port: find out whether it's the baud
            // rate, at most every few minutes (a scan blocks input for ~2s,
            // though nothing gets through while it's this broken anyway).
            auto now = std::chrono::steady_clock::now();
            if (!alive && fd_ >= 0 &&
                (!lastBaudScan_ || now - *lastBaudScan_ > std::chrono::minutes(5))) {
                lastBaudScan_ = now;
                if (auto rate = scanBaudLocked()) {
                    std::cerr << "Ch9329Inject: " << device_ << " answers at " << *rate
                               << " baud, but this target is set to " << baudRate_
                               << " — change the target's serial baud rate\n";
                    std::lock_guard<std::mutex> hl(healthMutex_);
                    answersAtBaud_ = *rate;
                }
            }
            if (alive && linkDownLogged_) {
                std::cerr << "Ch9329Inject: " << device_ << " answering again @ "
                           << baudRate_ << " baud\n";
                linkDownLogged_ = false;
            }
            if (alive) logParaCfgOnceBestEffort();
        }

        if (testRequested_.exchange(false)) runSelfTest();
        if (baudRequested_.exchange(false)) runBaudChange();

        auto wait = alive ? kHealthyCheckInterval : backoff;
        backoff   = alive ? kInitialBackoff : std::min(backoff * 2, kMaxBackoff);

        std::unique_lock<std::mutex> lock(cvMutex_);
        cv_.wait_for(lock, wait, [this] {
            return !running_.load() || testRequested_.load() || baudRequested_.load();
        });
    }
}

// ── Health ───────────────────────────────────────────────────────────────────

void Ch9329Inject::recordEvent(HealthEvent::Kind kind, double ms) {
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

void Ch9329Inject::pruneEventsLocked() const {
    auto cutoff = std::chrono::steady_clock::now() - kHealthWindow;
    while (!events_.empty() && events_.front().at < cutoff) events_.pop_front();
}

InputHealth Ch9329Inject::health() const {
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
            case HealthEvent::Kind::CheckFailed: ++h.checksFailed;           break;
            case HealthEvent::Kind::Reconnect:   ++h.reconnects;             break;
            case HealthEvent::Kind::Dropped:     h.droppedCommands += e.count; break;
        }
    }
    if (h.checksOk) h.replyMsAvg = msSum / h.checksOk;
    h.targetEnumerated = targetEnumerated_;
    h.firmwareVersion  = firmwareVersion_;
    h.chipBaud         = chipBaud_;
    h.answersAtBaud    = answersAtBaud_;

    auto [state, summary] = classifyLink({*h.deviceOpen, probedOnce_, lastProbeOk_.load(),
                                          targetEnumerated_, answersAtBaud_, baudRate_,
                                          h.checksOk, h.checksFailed, h.reconnects,
                                          kDegradedAfterFailures});
    h.state   = state;
    h.summary = std::move(summary);
    return h;
}

bool Ch9329Inject::startSelfTest() {
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

InputSelfTest Ch9329Inject::selfTest() const {
    std::lock_guard<std::mutex> lock(healthMutex_);
    return test_;
}

// A burst of probes with the datasheet's own 500 ms budget, then the chip's
// stored configuration. Each probe takes ioMutex_ on its own, so input
// queued meanwhile isn't held up for the whole burst.
void Ch9329Inject::runSelfTest() {
    InputSelfTest t;
    t.phase = InputSelfTest::Phase::Done;
    double msSum = 0;
    bool open = false;
    for (int i = 0; i < kSelfTestProbes && running_.load(); ++i) {
        {
            std::lock_guard<std::mutex> lock(ioMutex_);
            open = fd_ >= 0;
            if (!open) break;
            ++t.probes;
            auto start = std::chrono::steady_clock::now();
            auto info  = requestLocked(CMD_GET_INFO, std::chrono::milliseconds(500));
            double ms  = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - start).count();
            if (info) {
                ++t.ok;
                msSum += ms;
                t.replyMsMax = std::max(t.replyMsMax, ms);
                if (info->size() >= 2) {
                    t.firmwareVersion  = (*info)[0];
                    t.targetEnumerated = (*info)[1] == 0x01;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (t.ok) t.replyMsAvg = msSum / t.ok;

    std::optional<int> answersAt;
    {
        std::lock_guard<std::mutex> lock(ioMutex_);
        if (fd_ >= 0 && t.ok) {
            if (auto cfg = requestLocked(CMD_GET_PARA_CFG, std::chrono::milliseconds(500));
                cfg && cfg->size() >= 7) {
                const auto& c = *cfg;
                t.workMode   = c[0];
                t.serialMode = c[1];
                t.chipBaud   = (c[3] << 24) | (c[4] << 16) | (c[5] << 8) | c[6];
            }
        } else if (fd_ >= 0) {
            answersAt = scanBaudLocked();
        }
    }

    auto [state, verdict] = classifyLink({open, true, t.ok > 0, t.targetEnumerated, answersAt,
                                          baudRate_, t.ok, t.probes - t.ok, 0, 1});
    t.verdictState   = state;
    t.verdict        = std::move(verdict);
    t.finishedAtUnix = static_cast<int64_t>(std::time(nullptr));

    std::lock_guard<std::mutex> lock(healthMutex_);
    test_ = std::move(t);
}

bool Ch9329Inject::startBaudChange(int baud, std::function<void(int)> onSwitched) {
    {
        std::lock_guard<std::mutex> lock(healthMutex_);
        if (baudChange_.phase == InputBaudChange::Phase::Running) return false;
        baudChange_          = {};
        baudChange_.phase    = InputBaudChange::Phase::Running;
        baudChange_.fromBaud = baudRate_;
        baudChange_.toBaud   = baud;
        requestedBaud_       = baud;
        onBaudSwitched_      = std::move(onSwitched);
    }
    {
        std::lock_guard<std::mutex> lock(cvMutex_);
        baudRequested_ = true;
    }
    cv_.notify_all();
    return true;
}

InputBaudChange Ch9329Inject::baudChange() const {
    std::lock_guard<std::mutex> lock(healthMutex_);
    return baudChange_;
}

bool Ch9329Inject::writeConfigAndSwitchLocked(const std::vector<uint8_t>& cfg, int from, int to) {
    const auto answers = [this] {
        return requestLocked(CMD_GET_INFO, std::chrono::milliseconds(500)).has_value();
    };
    setSpeedLocked(from);
    auto status = requestLocked(CMD_SET_PARA_CFG, std::chrono::milliseconds(500),
                                cfg.data(), static_cast<uint8_t>(cfg.size()));
    if (!status || status->empty() || (*status)[0] != 0x00) return false;
    if (from == to) return true;

    // Some firmware switches rate at once, some only after a reset.
    setSpeedLocked(to);
    if (answers()) return true;
    setSpeedLocked(from);
    requestLocked(CMD_RESET, std::chrono::milliseconds(500));
    std::this_thread::sleep_for(std::chrono::seconds(1));
    setSpeedLocked(to);
    if (answers()) return true;
    setSpeedLocked(from);
    return false;
}

// Holds ioMutex_ throughout, so input waits for it: a few seconds at most,
// and the route refuses to start one while anyone is driving.
void Ch9329Inject::runBaudChange() {
    InputBaudChange r;
    std::function<void(int)> onSwitched;
    {
        std::lock_guard<std::mutex> lock(healthMutex_);
        r = baudChange_;
        onSwitched = std::move(onBaudSwitched_);
    }
    const int from = r.fromBaud, to = r.toBaud;
    const auto rate = [](int b) { return std::to_string(b) + " baud"; };

    {
        std::lock_guard<std::mutex> lock(ioMutex_);
        std::optional<std::vector<uint8_t>> cfg;
        if (fd_ >= 0 && probeAliveLocked(false))
            cfg = requestLocked(CMD_GET_PARA_CFG, std::chrono::milliseconds(500));

        if (!cfg) {
            r.message = "The adapter isn't answering at " + rate(from) +
                        ", so its speed can't be changed. Nothing was changed.";
        } else if (cfg->size() != 50) {
            r.message = "The adapter's configuration didn't read back correctly. "
                        "Nothing was changed.";
        } else {
            std::vector<uint8_t> newCfg = *cfg;
            newCfg[3] = static_cast<uint8_t>(to >> 24);
            newCfg[4] = static_cast<uint8_t>(to >> 16);
            newCfg[5] = static_cast<uint8_t>(to >> 8);
            newCfg[6] = static_cast<uint8_t>(to);

            if (!writeConfigAndSwitchLocked(newCfg, from, to)) {
                // Put the stored setting back, so it still matches the rate
                // the chip answers at; harmless if it was never written.
                bool restored = writeConfigAndSwitchLocked(*cfg, from, from);
                r.message = restored
                    ? "The adapter stayed at " + rate(from) + ". Some boards fix their "
                      "speed with configuration pins, so it can't be changed in software."
                    : "The adapter stopped answering during the change. Unplug it and "
                      "plug it back in, then run the input test.";
            } else {
                int ok = 0;
                for (int i = 0; i < 10; ++i)
                    if (requestLocked(CMD_GET_INFO, std::chrono::milliseconds(500))) ++ok;
                if (ok < 10) {
                    bool back = writeConfigAndSwitchLocked(*cfg, to, from);
                    r.message = "The link wasn't reliable at " + rate(to) + " (" +
                                std::to_string(ok) + " of 10 replies), so " +
                                (back ? "it went back to " + rate(from) + "."
                                      : "it tried to go back to " + rate(from) +
                                        ", but the adapter stopped answering. Unplug it "
                                        "and plug it back in.");
                } else {
                    baudRate_ = to;
                    r.ok      = true;
                    r.message = "The adapter now runs at " + rate(to) + ".";
                }
            }
            tcflush(fd_, TCIOFLUSH);
        }
    }
    std::cerr << "Ch9329Inject: " << device_ << " baud change " << from << " -> " << to
              << ": " << r.message << "\n";

    r.phase          = InputBaudChange::Phase::Done;
    r.finishedAtUnix = static_cast<int64_t>(std::time(nullptr));
    {
        std::lock_guard<std::mutex> lock(healthMutex_);
        if (r.ok) {
            chipBaud_ = to;
            answersAtBaud_.reset();
        }
        baudChange_ = r;
    }
    if (r.ok && onSwitched) onSwitched(to);
}

std::vector<uint8_t> Ch9329Inject::buildPacket(uint8_t cmd, const uint8_t* data, uint8_t len) {
    std::vector<uint8_t> pkt;
    pkt.reserve(static_cast<size_t>(5) + len + 1);
    pkt.push_back(0x57);
    pkt.push_back(0xAB);
    pkt.push_back(0x00); // device address
    pkt.push_back(cmd);
    pkt.push_back(len);
    pkt.insert(pkt.end(), data, data + len);

    uint32_t sum = 0;
    for (uint8_t b : pkt) sum += b;
    pkt.push_back(static_cast<uint8_t>(sum & 0xFF));
    return pkt;
}

void Ch9329Inject::sendCommand(uint8_t cmd, const uint8_t* data, uint8_t len) {
    std::lock_guard<std::mutex> lock(ioMutex_);
    if (fd_ < 0) {
        // Logged so a click/key landing in a reconnect window (see
        // watchdogLoop()) doesn't vanish without a trace. Once per
        // down-stretch, not once per dropped command, so this
        // can't spam under a mousemove flood while the link is down.
        recordEvent(HealthEvent::Kind::Dropped);
        if (!loggedDropWhileDown_) {
            loggedDropWhileDown_ = true;
            std::cerr << "Ch9329Inject: dropping cmd=0x" << std::hex
                       << static_cast<int>(cmd) << std::dec
                       << " — serial link down (reconnecting)\n";
        }
        return;
    }

    auto pkt = buildPacket(cmd, data, len);
    if (!writeAllRetrying(fd_, pkt.data(), pkt.size())) {
        std::cerr << "Ch9329Inject: command write failed: " << strerror(errno) << "\n";
        return;
    }
    // Drain whatever the chip sends back, if anything — fd_ is O_NONBLOCK so
    // this never waits. Not verified against `cmd`; see the "fire-and-forget"
    // rationale on the declaration. This just keeps stray bytes from piling
    // up in the kernel's read buffer between calls.
    uint8_t buf[64];
    while (read(fd_, buf, sizeof(buf)) > 0) {}
}

void Ch9329Inject::sendKeyReport() {
    // Standard 8-byte boot-protocol keyboard report, same layout InputInject
    // writes to /dev/hidg0 — CH9329 forwards it to the target verbatim.
    uint8_t data[8] = {};
    data[0] = modifiers_;
    for (int i = 0; i < 6; ++i) data[2 + i] = keys_[i];
    // Sent twice: a full state snapshot, so the repeat is a no-op if the
    // first landed — and a missed keydown/keyup (esp. a modifier) is worse
    // than a missed mouse-move sample, so we don't rely on the next report
    // to self-correct here the way movement does.
    sendCommand(CMD_SEND_KB_GENERAL_DATA, data, sizeof(data));
    sendCommand(CMD_SEND_KB_GENERAL_DATA, data, sizeof(data));
}

void Ch9329Inject::sendMouseReport(int8_t wheel, bool redundant) {
    uint8_t data[7] = {
        0x02, // report mode: always 2 for absolute mode per CH9329 protocol
        mouse_buttons_,
        static_cast<uint8_t>(mouse_x_ & 0xFF),
        static_cast<uint8_t>((mouse_x_ >> 8) & 0xFF),
        static_cast<uint8_t>(mouse_y_ & 0xFF),
        static_cast<uint8_t>((mouse_y_ >> 8) & 0xFF),
        static_cast<uint8_t>(wheel)
    };
    sendCommand(CMD_SEND_MS_ABS_DATA, data, sizeof(data));
    if (redundant) sendCommand(CMD_SEND_MS_ABS_DATA, data, sizeof(data));
}

void Ch9329Inject::sendRelativeButtonReport() {
    uint8_t data[5] = {
        0x01, // report mode: always 1 for relative mode per CH9329 protocol
              // — likely the actual USB HID Report ID; see header comment.
        mouse_buttons_,
        0x00, // no X delta
        0x00, // no Y delta
        0x00, // no wheel
    };
    sendCommand(CMD_SEND_MS_REL_DATA, data, sizeof(data));
}

void Ch9329Inject::mouseMoveAbsolute(double x, double y) {
    // No fd_ check here: sendCommand() safely no-ops while disconnected,
    // and reading fd_ without ioMutex_ would race the watchdog.
    x = std::clamp(x, 0.0, 1.0);
    y = std::clamp(y, 0.0, 1.0);
    mouse_x_ = static_cast<uint16_t>(x * kCh9329AbsMax + 0.5);
    mouse_y_ = static_cast<uint16_t>(y * kCh9329AbsMax + 0.5);
    sendMouseReport(0);
}

void Ch9329Inject::mouseButton(int button, bool pressed) {
    if (button < 0 || button > 2) return;
    // DOM MouseEvent.button is 0=left,1=middle,2=right; the CH9329 report
    // byte is bit0=left,bit1=right,bit2=middle — middle/right don't share a
    // bit position, so translate rather than shifting `button` directly.
    static constexpr uint8_t kBitForButton[3] = {0x01, 0x04, 0x02};
    uint8_t bit = kBitForButton[button];
    if (pressed) mouse_buttons_ |=  bit;
    else         mouse_buttons_ &= ~bit;
    sendMouseReport(0, /*redundant=*/true);
    sendRelativeButtonReport(); // workaround — see header comment
}

void Ch9329Inject::mouseScroll(int delta) {
    if (delta == 0) return;
    int8_t wheel = static_cast<int8_t>(std::clamp(delta, -127, 127));
    sendMouseReport(wheel);
}

void Ch9329Inject::keyEvent(const std::string& jsCode, bool pressed) {
    bool    isModifier = false;
    uint8_t modBit     = 0;
    uint8_t hid        = jsCodeToHid(jsCode, isModifier, modBit);
    if (!isModifier && hid == 0) return;

    if (isModifier) {
        if (pressed) modifiers_ |=  modBit;
        else         modifiers_ &= ~modBit;
    } else if (pressed) {
        for (const auto& k : keys_) if (k == hid) return; // ignore key-repeat
        for (auto& k : keys_) { if (k == 0) { k = hid; break; } }
    } else {
        for (auto& k : keys_) { if (k == hid) { k = 0; break; } }
    }
    sendKeyReport();
}

void Ch9329Inject::releaseAll() {
    modifiers_     = 0;
    keys_.fill(0);
    mouse_buttons_ = 0;
    // Always sent, even if our own state already read zero: a key-up that
    // was lost on the serial link (the common failure on these chips) leaves
    // the target holding a key our state has long since forgotten, and a
    // full-state snapshot of "nothing pressed" is the only thing that clears
    // it. sendKeyReport() already sends twice.
    sendKeyReport();
    // Buttons are cleared with a zero-delta *relative* report, not an
    // absolute one: it's the report type buttons actually register on (see
    // sendRelativeButtonReport()), and unlike an absolute report it can't
    // move the cursor — which would snap to our last-sent position (the
    // screen centre, if it never moved) on every release.
    sendRelativeButtonReport();
    sendRelativeButtonReport();
}

std::chrono::milliseconds Ch9329Inject::typingInterval() const {
    // One keyEvent() is two 14-byte keyboard packets (sendKeyReport() sends
    // each twice) at 10 bits a byte on the wire. Leave a quarter again on
    // top so the link drains between reports rather than queueing in the
    // tty, and never go below the default: the chip still has to hand each
    // report to the target's USB poll. About 37 ms at 9600 baud, 10 ms at
    // 115200.
    int baud = std::max(1, baudRate_.load(std::memory_order_relaxed));
    auto wireMs = (2 * 14 * 10 * 1000 * 5 / 4 + baud - 1) / baud;
    return std::max(InputBackend::typingInterval(), std::chrono::milliseconds(wireMs));
}

uint8_t Ch9329Inject::jsCodeToHid(const std::string& code, bool& isModifier, uint8_t& modBit) {
    isModifier = false;
    modBit     = 0;

    static const std::unordered_map<std::string, uint8_t> mods = {
        {"ControlLeft",0x01},{"ShiftLeft",  0x02},
        {"AltLeft",    0x04},{"MetaLeft",   0x08},
        {"ControlRight",0x10},{"ShiftRight",0x20},
        {"AltRight",   0x40},{"MetaRight",  0x80},
    };
    if (auto it = mods.find(code); it != mods.end()) {
        isModifier = true;
        modBit     = it->second;
        return 0;
    }

    static const std::unordered_map<std::string, uint8_t> table = {
        {"KeyA",0x04},{"KeyB",0x05},{"KeyC",0x06},{"KeyD",0x07},
        {"KeyE",0x08},{"KeyF",0x09},{"KeyG",0x0A},{"KeyH",0x0B},
        {"KeyI",0x0C},{"KeyJ",0x0D},{"KeyK",0x0E},{"KeyL",0x0F},
        {"KeyM",0x10},{"KeyN",0x11},{"KeyO",0x12},{"KeyP",0x13},
        {"KeyQ",0x14},{"KeyR",0x15},{"KeyS",0x16},{"KeyT",0x17},
        {"KeyU",0x18},{"KeyV",0x19},{"KeyW",0x1A},{"KeyX",0x1B},
        {"KeyY",0x1C},{"KeyZ",0x1D},
        {"Digit1",0x1E},{"Digit2",0x1F},{"Digit3",0x20},{"Digit4",0x21},
        {"Digit5",0x22},{"Digit6",0x23},{"Digit7",0x24},{"Digit8",0x25},
        {"Digit9",0x26},{"Digit0",0x27},
        {"Enter",0x28},{"Escape",0x29},{"Backspace",0x2A},{"Tab",0x2B},
        {"Space",0x2C},{"Minus",0x2D},{"Equal",0x2E},
        {"BracketLeft",0x2F},{"BracketRight",0x30},{"Backslash",0x31},
        {"Semicolon",0x33},{"Quote",0x34},{"Backquote",0x35},
        {"Comma",0x36},{"Period",0x37},{"Slash",0x38},
        {"CapsLock",0x39},
        {"F1",0x3A},{"F2",0x3B},{"F3",0x3C},{"F4",0x3D},
        {"F5",0x3E},{"F6",0x3F},{"F7",0x40},{"F8",0x41},
        {"F9",0x42},{"F10",0x43},{"F11",0x44},{"F12",0x45},
        {"PrintScreen",0x46},{"ScrollLock",0x47},{"Pause",0x48},
        {"Insert",0x49},{"Home",0x4A},{"PageUp",0x4B},
        {"Delete",0x4C},{"End",0x4D},{"PageDown",0x4E},
        {"ArrowRight",0x4F},{"ArrowLeft",0x50},{"ArrowDown",0x51},{"ArrowUp",0x52},
        {"NumLock",0x53},
        {"NumpadDivide",0x54},{"NumpadMultiply",0x55},{"NumpadSubtract",0x56},
        {"NumpadAdd",0x57},{"NumpadEnter",0x58},
        {"Numpad1",0x59},{"Numpad2",0x5A},{"Numpad3",0x5B},{"Numpad4",0x5C},
        {"Numpad5",0x5D},{"Numpad6",0x5E},{"Numpad7",0x5F},{"Numpad8",0x60},
        {"Numpad9",0x61},{"Numpad0",0x62},{"NumpadDecimal",0x63},
        {"IntlBackslash",0x64},
    };
    auto it = table.find(code);
    return it != table.end() ? it->second : 0;
}

}
