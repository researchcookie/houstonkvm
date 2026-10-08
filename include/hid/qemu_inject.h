#pragma once

#include "hid/input_backend.h"
#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

namespace houston_kvm {

class QemuInject final : public InputBackend {
public:
    explicit QemuInject(const std::string& socketPath);
    ~QemuInject() override;

    QemuInject(const QemuInject&) = delete;
    QemuInject& operator=(const QemuInject&) = delete;

    bool isReady() const override;
    const char* kind() const override { return "qemu"; }
    InputHealth health() const override {
        InputHealth h = InputBackend::health();
        if (h.state == HealthState::Down)
            h.summary = "Not connected to the VM's QMP socket: check the VM is running and "
                        "the socket path is right.";
        return h;
    }

    void mouseMoveAbsolute(double x, double y) override;
    void mouseButton(int button, bool pressed) override;
    void mouseScroll(int delta) override;
    void keyEvent(const std::string& jsCode, bool pressed) override;
    void releaseAll() override;

private:
    // Guards fd_ and all I/O on it, same pattern as Ch9329Inject's ioMutex_.
    mutable std::mutex ioMutex_;
    int fd_ = -1;
    // Mirrors fd_ >= 0 for isReady(), which the event loop polls: a write to
    // a QEMU that has stopped reading can block with ioMutex_ held.
    std::atomic<bool> connected_{false};

    // QMP input events are edges, not state snapshots, so unlike the other
    // backends there is nothing to zero: we have to remember what we
    // pressed in order to release it. Only touched from InputQueue's worker
    // thread (keyEvent/mouseButton/releaseAll are all called from there), so
    // no lock of its own.
    std::set<std::string> heldKeys_;
    std::set<std::string> heldButtons_;

    bool sendCmd(const std::string& json);
    void drainResponse();
    static const char* jsCodeToQcode(const std::string& jsCode);
};

} 
