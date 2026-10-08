#pragma once

#include "hid/input_backend.h"

#include <array>
#include <cstdint>
#include <string>

namespace houston_kvm {

// Injects keyboard/mouse events into the target PC via USB HID gadget.
// Requires /dev/hidg0 (keyboard) and /dev/hidg1 (mouse) — run setup-hid-gadget.sh first.
class InputInject final : public InputBackend {
public:
    InputInject();
    ~InputInject() override;

    InputInject(const InputInject&) = delete;
    InputInject& operator=(const InputInject&) = delete;

    bool isReady() const override { return kbd_fd_ >= 0 && mouse_fd_ >= 0; }
    const char* kind() const override { return "usb-gadget"; }
    InputHealth health() const override {
        InputHealth h = InputBackend::health();
        if (h.state == HealthState::Down)
            h.summary = "/dev/hidg0 or /dev/hidg1 isn't available: this server needs a USB "
                        "gadget port, set up by houstonkvm-setup-hid (houstonkvm-hid.service).";
        return h;
    }

    void mouseMoveAbsolute(double x, double y) override;
    void mouseButton(int button, bool pressed) override;
    void mouseScroll(int delta) override;
    void keyEvent(const std::string& jsCode, bool pressed) override;
    void releaseAll() override;

    // Returns the USB HID usage ID; sets isModifier+modBit for modifier keys.
    static uint8_t jsCodeToHid(const std::string& code, bool& isModifier, uint8_t& modBit);

private:
    int kbd_fd_   = -1;
    int mouse_fd_ = -1;

    uint8_t modifiers_     = 0;
    std::array<uint8_t, 6> keys_{};
    uint8_t mouse_buttons_ = 0;
    // Last absolute position sent, in the gadget's 0-32767 HID report
    // space. The report is stateful-per-write, not a delta, so a
    // button/scroll-only report must resend the last known coordinates or
    // the target's cursor snaps back to wherever it was before.
    uint16_t mouse_x_ = 16384;
    uint16_t mouse_y_ = 16384;

    void sendKeyReport();
    void sendMouseReport(int8_t wheel);
};

} // namespace houston_kvm
