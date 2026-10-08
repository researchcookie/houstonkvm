#include "hid/input_inject.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <unordered_map>

namespace houston_kvm {

namespace {
constexpr const char* KBD_DEV   = "/dev/hidg0";
constexpr const char* MOUSE_DEV = "/dev/hidg1";

// A HID report here is a handful of bytes, normally well within the
// gadget's report queue — but the fd is O_NONBLOCK, so a write can still
// come back EAGAIN/short if the target hasn't drained the previous report
// yet. Unlike a stale mouse-move sample, a dropped button/key edge has no
// self-correction, so retry — but only for local queue space to free up
// (poll for POLLOUT), never for the target to actually consume/ack the
// report. Bounded to ~30ms total so a stuck write can't stall the worker
// thread InputQueue runs this on.
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
} // namespace

InputInject::InputInject() {
    kbd_fd_ = open(KBD_DEV, O_WRONLY | O_NONBLOCK);
    if (kbd_fd_ < 0) {
        std::cerr << "InputInject: cannot open " << KBD_DEV << ": " << strerror(errno)
                  << " (run sudo scripts/setup-hid-gadget.sh)\n";
        return;
    }

    mouse_fd_ = open(MOUSE_DEV, O_WRONLY | O_NONBLOCK);
    if (mouse_fd_ < 0) {
        std::cerr << "InputInject: cannot open " << MOUSE_DEV << ": " << strerror(errno) << "\n";
        close(kbd_fd_);
        kbd_fd_ = -1;
        return;
    }

    std::cout << "InputInject: HID gadget keyboard+mouse ready\n";
}

InputInject::~InputInject() {
    if (kbd_fd_ >= 0)   close(kbd_fd_);
    if (mouse_fd_ >= 0) close(mouse_fd_);
}

void InputInject::sendKeyReport() {
    // Standard 8-byte boot-protocol keyboard report.
    uint8_t report[8] = {};
    report[0] = modifiers_;
    for (int i = 0; i < 6; ++i) report[2 + i] = keys_[i];
    if (!writeAllRetrying(kbd_fd_, report, sizeof(report)))
        std::cerr << "InputInject: keyboard report write failed: " << strerror(errno) << "\n";
}

namespace {
// Matches the Logical Maximum (0x7FFF) for X/Y in the absolute hid.mouse
// report descriptor in setup-hid-gadget.sh.
constexpr uint16_t kGadgetAbsMax = 32767;
}

void InputInject::sendMouseReport(int8_t wheel) {
    // 6-byte absolute report: buttons(1) + X(2 LE) + Y(2 LE) + wheel(1).
    uint8_t report[6] = {
        mouse_buttons_,
        static_cast<uint8_t>(mouse_x_ & 0xFF),
        static_cast<uint8_t>((mouse_x_ >> 8) & 0xFF),
        static_cast<uint8_t>(mouse_y_ & 0xFF),
        static_cast<uint8_t>((mouse_y_ >> 8) & 0xFF),
        static_cast<uint8_t>(wheel)
    };
    if (!writeAllRetrying(mouse_fd_, report, sizeof(report)))
        std::cerr << "InputInject: mouse report write failed: " << strerror(errno) << "\n";
}

void InputInject::mouseMoveAbsolute(double x, double y) {
    if (mouse_fd_ < 0) return;
    x = std::clamp(x, 0.0, 1.0);
    y = std::clamp(y, 0.0, 1.0);
    mouse_x_ = static_cast<uint16_t>(x * kGadgetAbsMax + 0.5);
    mouse_y_ = static_cast<uint16_t>(y * kGadgetAbsMax + 0.5);
    sendMouseReport(0);
}

void InputInject::mouseButton(int button, bool pressed) {
    if (mouse_fd_ < 0 || button < 0 || button > 2) return;
    if (pressed) mouse_buttons_ |=  static_cast<uint8_t>(1u << button);
    else         mouse_buttons_ &= ~static_cast<uint8_t>(1u << button);
    sendMouseReport(0);
}

void InputInject::mouseScroll(int delta) {
    if (mouse_fd_ < 0 || delta == 0) return;
    sendMouseReport(static_cast<int8_t>(std::clamp(delta, -127, 127)));
}

void InputInject::keyEvent(const std::string& jsCode, bool pressed) {
    if (kbd_fd_ < 0) return;
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

void InputInject::releaseAll() {
    modifiers_ = 0;
    keys_.fill(0);
    if (kbd_fd_ >= 0) sendKeyReport();
    // The gadget's mouse report is absolute-only, so unlike the keyboard a
    // mouse report can't be sent "just in case" — it would reposition the
    // cursor to our last-sent coordinates. Only send one if a button was
    // actually down.
    bool hadButtons = mouse_buttons_ != 0;
    mouse_buttons_  = 0;
    if (hadButtons && mouse_fd_ >= 0) sendMouseReport(0);
}

uint8_t InputInject::jsCodeToHid(const std::string& code, bool& isModifier, uint8_t& modBit) {
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

} // namespace houston_kvm
