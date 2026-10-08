#include "hid/qemu_inject.h"

#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <unordered_map>

using json = nlohmann::json;

namespace houston_kvm {

// ── Construction / Destruction ───────────────────────────────────────────────

QemuInject::QemuInject(const std::string& socketPath) {
    fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
        std::cerr << "QemuInject: socket() failed: " << strerror(errno) << "\n";
        return;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socketPath.size() >= sizeof(addr.sun_path)) {
        std::cerr << "QemuInject: socket path too long\n";
        close(fd_); fd_ = -1; return;
    }
    std::strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "QemuInject: cannot connect to " << socketPath << ": "
                  << strerror(errno) << "\n"
                  << "             Start QEMU with: -qmp unix:"
                  << socketPath << ",server,nowait\n";
        close(fd_); fd_ = -1; return;
    }

    // Read the QMP greeting then negotiate capabilities.
    char buf[4096];
    if (read(fd_, buf, sizeof(buf) - 1) <= 0) {
        std::cerr << "QemuInject: no QMP greeting\n";
        close(fd_); fd_ = -1; return;
    }
    if (!sendCmd(R"({"execute":"qmp_capabilities"})")) return;
    if (read(fd_, buf, sizeof(buf) - 1) <= 0) {
        std::cerr << "QemuInject: qmp_capabilities handshake failed\n";
        close(fd_); fd_ = -1; return;
    }

    connected_.store(true, std::memory_order_relaxed);
    std::cout << "QemuInject: connected to QEMU QMP socket " << socketPath << "\n";
}

QemuInject::~QemuInject() {
    std::lock_guard<std::mutex> lock(ioMutex_);
    if (fd_ >= 0) close(fd_);
}

bool QemuInject::isReady() const {
    return connected_.load(std::memory_order_relaxed);
}

// ── Internal helpers ─────────────────────────────────────────────────────────

bool QemuInject::sendCmd(const std::string& payload) {
    std::lock_guard<std::mutex> lock(ioMutex_);
    if (fd_ < 0) return false;
    std::string msg = payload + "\n";
    ssize_t n = write(fd_, msg.c_str(), msg.size());
    if (n != static_cast<ssize_t>(msg.size())) {
        std::cerr << "QemuInject: write failed: " << strerror(errno) << "\n";
        close(fd_); fd_ = -1;
        connected_.store(false, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void QemuInject::drainResponse() {
    std::lock_guard<std::mutex> lock(ioMutex_);
    if (fd_ < 0) return;
    char buf[4096];
    while (recv(fd_, buf, sizeof(buf), MSG_DONTWAIT) > 0) {}
}

// ── Public API ───────────────────────────────────────────────────────────────

namespace {
// QMP absolute-axis events range 0..0x7FFF, proportional to whatever
// resolution the guest display currently is — QEMU rescales internally.
constexpr int kQmpAbsMax = 0x7FFF;
}

void QemuInject::mouseMoveAbsolute(double x, double y) {
    if (!isReady()) return;
    x = std::clamp(x, 0.0, 1.0);
    y = std::clamp(y, 0.0, 1.0);
    int vx = static_cast<int>(x * kQmpAbsMax + 0.5);
    int vy = static_cast<int>(y * kQmpAbsMax + 0.5);
    json events = json::array({
        {{"type","abs"},{"data",{{"axis","x"},{"value",vx}}}},
        {{"type","abs"},{"data",{{"axis","y"},{"value",vy}}}},
        {{"type","syn"},{"data",json::object()}}
    });
    sendCmd(json{{"execute","input-send-event"},{"arguments",{{"events",events}}}}.dump());
    drainResponse();
}

void QemuInject::mouseButton(int button, bool pressed) {
    if (!isReady() || button < 0 || button > 2) return;
    static constexpr const char* names[] = {"left","right","middle"};
    if (pressed) heldButtons_.insert(names[button]);
    else         heldButtons_.erase(names[button]);
    json events = json::array({
        {{"type","btn"},{"data",{{"down",pressed},{"button",names[button]}}}},
        {{"type","syn"},{"data",json::object()}}
    });
    sendCmd(json{{"execute","input-send-event"},{"arguments",{{"events",events}}}}.dump());
    drainResponse();
}

void QemuInject::mouseScroll(int delta) {
    if (!isReady() || delta == 0) return;
    const char* btn = delta > 0 ? "wheel-up" : "wheel-down";
    int steps = std::abs(delta);
    json events = json::array();
    for (int i = 0; i < steps; ++i) {
        events.push_back({{"type","btn"},{"data",{{"down",true }, {"button",btn}}}});
        events.push_back({{"type","syn"},{"data",json::object()}});
        events.push_back({{"type","btn"},{"data",{{"down",false}, {"button",btn}}}});
        events.push_back({{"type","syn"},{"data",json::object()}});
    }
    sendCmd(json{{"execute","input-send-event"},{"arguments",{{"events",events}}}}.dump());
    drainResponse();
}

void QemuInject::keyEvent(const std::string& jsCode, bool pressed) {
    if (!isReady()) return;
    const char* qcode = jsCodeToQcode(jsCode);
    if (!qcode) return;
    if (pressed) heldKeys_.insert(qcode);
    else         heldKeys_.erase(qcode);
    json events = json::array({
        {{"type","key"},{"data",{{"down",pressed},{"key",{{"type","qcode"},{"data",qcode}}}}}},
        {{"type","syn"},{"data",json::object()}}
    });
    sendCmd(json{{"execute","input-send-event"},{"arguments",{{"events",events}}}}.dump());
    drainResponse();
}

void QemuInject::releaseAll() {
    if (heldKeys_.empty() && heldButtons_.empty()) return;
    // Link down: keep what we're holding so a later call can still release
    // it, rather than forgetting keys the guest still thinks are pressed.
    if (!isReady()) return;
    json events = json::array();
    for (const auto& key : heldKeys_) {
        events.push_back({{"type","key"},{"data",{{"down",false},{"key",{{"type","qcode"},{"data",key}}}}}});
    }
    for (const auto& button : heldButtons_) {
        events.push_back({{"type","btn"},{"data",{{"down",false},{"button",button}}}});
    }
    events.push_back({{"type","syn"},{"data",json::object()}});
    sendCmd(json{{"execute","input-send-event"},{"arguments",{{"events",events}}}}.dump());
    drainResponse();
    heldKeys_.clear();
    heldButtons_.clear();
}

// ── Key mapping (JS KeyboardEvent.code → QEMU qcode) ────────────────────────

const char* QemuInject::jsCodeToQcode(const std::string& code) {
    // QEMU qcode names are defined in qapi/ui.json QKeyCode enum.
    static const std::unordered_map<std::string, const char*> table = {
        {"KeyA","a"},{"KeyB","b"},{"KeyC","c"},{"KeyD","d"},
        {"KeyE","e"},{"KeyF","f"},{"KeyG","g"},{"KeyH","h"},
        {"KeyI","i"},{"KeyJ","j"},{"KeyK","k"},{"KeyL","l"},
        {"KeyM","m"},{"KeyN","n"},{"KeyO","o"},{"KeyP","p"},
        {"KeyQ","q"},{"KeyR","r"},{"KeyS","s"},{"KeyT","t"},
        {"KeyU","u"},{"KeyV","v"},{"KeyW","w"},{"KeyX","x"},
        {"KeyY","y"},{"KeyZ","z"},
        {"Digit1","1"},{"Digit2","2"},{"Digit3","3"},{"Digit4","4"},
        {"Digit5","5"},{"Digit6","6"},{"Digit7","7"},{"Digit8","8"},
        {"Digit9","9"},{"Digit0","0"},
        {"Enter","ret"},{"Escape","esc"},{"Backspace","backspace"},
        {"Tab","tab"},{"Space","spc"},
        {"Minus","minus"},{"Equal","equal"},
        {"BracketLeft","bracket_left"},{"BracketRight","bracket_right"},
        {"Backslash","backslash"},{"Semicolon","semicolon"},
        {"Quote","apostrophe"},{"Backquote","grave_accent"},
        {"Comma","comma"},{"Period","dot"},{"Slash","slash"},
        {"CapsLock","caps_lock"},
        {"F1","f1"},{"F2","f2"},{"F3","f3"},{"F4","f4"},
        {"F5","f5"},{"F6","f6"},{"F7","f7"},{"F8","f8"},
        {"F9","f9"},{"F10","f10"},{"F11","f11"},{"F12","f12"},
        {"PrintScreen","print"},{"ScrollLock","scroll_lock"},{"Pause","pause"},
        {"Insert","insert"},{"Home","home"},{"PageUp","pgup"},
        {"Delete","delete"},{"End","end"},{"PageDown","pgdn"},
        {"ArrowRight","right"},{"ArrowLeft","left"},
        {"ArrowDown","down"},{"ArrowUp","up"},
        {"NumLock","num_lock"},
        {"NumpadDivide","kp_divide"},{"NumpadMultiply","kp_multiply"},
        {"NumpadSubtract","kp_subtract"},{"NumpadAdd","kp_add"},
        {"NumpadEnter","kp_enter"},
        {"Numpad1","kp_1"},{"Numpad2","kp_2"},{"Numpad3","kp_3"},
        {"Numpad4","kp_4"},{"Numpad5","kp_5"},{"Numpad6","kp_6"},
        {"Numpad7","kp_7"},{"Numpad8","kp_8"},{"Numpad9","kp_9"},
        {"Numpad0","kp_0"},{"NumpadDecimal","kp_decimal"},
        {"ShiftLeft","shift"},  {"ShiftRight","shift_r"},
        {"ControlLeft","ctrl"}, {"ControlRight","ctrl_r"},
        {"AltLeft","alt"},      {"AltRight","altgr"},
        {"MetaLeft","meta_l"},  {"MetaRight","meta_r"},
        {"IntlBackslash","102nd"},
    };
    auto it = table.find(code);
    return it != table.end() ? it->second : nullptr;
}

} // namespace houston_kvm
