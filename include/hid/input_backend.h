#pragma once
#include "hid/input_health.h"

#include <chrono>
#include <functional>
#include <string>

namespace houston_kvm {

class InputBackend {
public:
    virtual ~InputBackend() = default;

    virtual bool isReady() const = 0;
    // x, y are normalized to the video frame: [0.0, 1.0] on each axis,
    // (0,0) = top-left. Absolute-only — there is no relative-motion path;
    // every backend maps this into its own device coordinate space.
    virtual void mouseMoveAbsolute(double x, double y) = 0;
    virtual void mouseButton(int button, bool pressed) = 0;
    virtual void mouseScroll(int delta) = 0;
    virtual void keyEvent(const std::string& jsCode, bool pressed) = 0;
    // Puts the target's keyboard and mouse back to "nothing pressed", for
    // when whoever was driving stops without having released everything
    // (control released or taken over, the browser tab closed or lost its
    // connection, the target being stopped). The HID chip and the gadget
    // keep their last report until told otherwise, so without this a key
    // held at that moment stays held on the target indefinitely.
    //
    // Must be safe to call when nothing is held — callers send it as a
    // precaution — and must never move the cursor. Like every other method
    // it is only called from InputQueue's worker thread.
    virtual void releaseAll() = 0;

    // How long to leave between the key reports of text being typed (see
    // InputQueue::pushText()), so each press and release reaches the target
    // as its own report instead of piling up in a link that can't keep
    // pace. Called from InputQueue's worker thread.
    virtual std::chrono::milliseconds typingInterval() const {
        return std::chrono::milliseconds(10);
    }

    // Health and self-test are called from the uWS event-loop thread, so
    // implementations must never block on device I/O here.
    virtual const char* kind() const = 0;
    // Backends that can't talk back only know whether the device is open.
    virtual InputHealth health() const {
        InputHealth h;
        h.kind    = kind();
        h.state   = isReady() ? HealthState::Good : HealthState::Down;
        h.summary = isReady() ? "Input device connected."
                              : "The input device isn't available.";
        return h;
    }
    // Starts an asynchronous self-test. False if this backend has none, or
    // one is already running.
    virtual bool startSelfTest() { return false; }
    virtual bool hasSelfTest() const { return false; }
    virtual InputSelfTest selfTest() const { return {}; }

    // Starts an asynchronous change of the adapter's line speed, rewriting
    // its own stored configuration so the change survives a power cycle.
    // `onSwitched` runs on the backend's own thread once the adapter answers
    // reliably at the new rate, so the caller can save it. False if this
    // backend can't, or a change is already running.
    virtual bool startBaudChange(int /*baud*/, std::function<void(int)> /*onSwitched*/) { return false; }
    virtual bool canChangeBaud() const { return false; }
    virtual InputBaudChange baudChange() const { return {}; }
};

} // namespace houston_kvm
