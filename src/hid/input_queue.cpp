#include "hid/input_queue.h"

#include "hid/input_backend.h"
#include "hid/text_typing.h"
#include "video/stream_manager.h"

#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>

namespace houston_kvm {

namespace {

// Best effort: neither may fail startup, since most installs won't have
// CAP_SYS_NICE (scripts/houstonkvm.service grants it).
//
// Returns whether SCHED_FIFO took effect. Only then is the thread pinned to
// a core: pinned at normal priority, it would wait whenever that core is
// busy (with video encoding, say) instead of moving to a free one.
bool applyRealtimeSchedulingBestEffort() {
    sched_param param{};
    param.sched_priority = 10; // low end of 1-99 (sched(7)): start low,
                                // raise only if measurement shows a
                                // problem. Deliberately not 99 — that
                                // shares the top band with kernel
                                // migration/watchdog threads.
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
        std::cerr << "InputQueue: SCHED_FIFO unavailable (needs CAP_SYS_NICE) "
                     "— input worker runs at normal scheduling priority, "
                     "not pinning CPU affinity either (see input_queue.cpp)\n";
        return false;
    }
    return true;
}

// The jiggler's step, as a share of the screen: a few pixels, enough for
// every OS to take it as movement, toward the middle so it stays on screen.
// It comes back after kJiggleReturn, so it doesn't build up or stay put.
constexpr double kJiggleStep   = 0.002;
constexpr auto   kJiggleReturn = std::chrono::milliseconds(100);

// An input test or a speed change under way: input would spoil the test's
// results, or go out at the wrong speed.
bool adapterBusy(const InputBackend& b) {
    return b.selfTest().phase == InputSelfTest::Phase::Running ||
           b.baudChange().phase == InputBaudChange::Phase::Running;
}

void applyCpuAffinityBestEffort() {
    unsigned n = std::thread::hardware_concurrency();
    if (n < 2) return; // nothing to gain by pinning on a single-core box
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(n - 1, &set); // last core, not core 0 — core 0 most commonly
                           // fields IRQs by default on ARM SBCs
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0) {
        std::cerr << "InputQueue: could not pin input worker's CPU affinity\n";
    }
}

} // namespace

InputQueue::InputQueue(StreamManager& streamManager) : streamManager_(streamManager) {}

InputQueue::~InputQueue() {
    running_.store(false, std::memory_order_relaxed);
    notify(); // wake a blocked acquire() so it can see running_==false
    if (worker_.joinable()) worker_.join();
}

void InputQueue::start() {
    running_.store(true, std::memory_order_relaxed);
    worker_ = std::thread(&InputQueue::workerLoop, this);
}

void InputQueue::notify() {
    if (!signaled_.exchange(true, std::memory_order_release)) wake_.release();
}

bool InputQueue::push(const InputEvent& ev) {
    size_t head = head_.load(std::memory_order_relaxed);
    size_t next = (head + 1) % kCapacity;
    if (next == tail_.load(std::memory_order_acquire)) {
        // Logged so a dropped click/key always leaves a trace server-side
        // (routes_input.cpp also reports it to the client as Overloaded).
        // This should only ever fire under a genuine flood; if it
        // shows up during ordinary single clicks, the ring is filling
        // for a reason worth investigating, not just "make it bigger".
        std::cerr << "InputQueue: ring buffer full, dropping event (type="
                   << static_cast<int>(ev.type) << ")\n";
        return false; // full
    }
    ring_[head] = ev;
    head_.store(next, std::memory_order_release);
    notify();
    return true;
}

void InputQueue::pushMouseMove(double x, double y) {
    {
        std::lock_guard<std::mutex> lock(moveMutex_);
        moveX_ = x;
        moveY_ = y;
        moveValid_ = true;
    }
    notify();
}

bool InputQueue::pushMouseButton(int button, bool pressed) {
    InputEvent ev{};
    ev.type = InputEvent::Type::MouseButton;
    ev.button = button;
    ev.pressed = pressed;
    return push(ev);
}

bool InputQueue::pushMouseScroll(int delta) {
    InputEvent ev{};
    ev.type = InputEvent::Type::MouseScroll;
    ev.delta = delta;
    return push(ev);
}

void InputQueue::pushReleaseAll() {
    // Whatever ended control ends the typing with it. Cleared here, not when
    // the worker reaches the release, so text pushed after this call is kept.
    cancelText();
    InputEvent ev{};
    ev.type = InputEvent::Type::ReleaseAll;
    if (!push(ev)) {
        releaseAllPending_.store(true, std::memory_order_release);
        notify();
    }
}

bool InputQueue::pushKey(std::string_view jsCode, bool pressed) {
    InputEvent ev{};
    ev.type = InputEvent::Type::Key;
    ev.pressed = pressed;
    size_t len = std::min(jsCode.size(), sizeof(ev.keyCode));
    std::memcpy(ev.keyCode, jsCode.data(), len);
    ev.keyCodeLen = static_cast<uint8_t>(len);
    return push(ev);
}

bool InputQueue::pushText(std::string_view text) {
    if (text.empty()) return true;
    {
        std::lock_guard<std::mutex> lock(textMutex_);
        size_t waiting = text_.size() - textPos_;
        if (waiting + text.size() > kMaxPendingText) return false;
        text_.erase(0, textPos_); // what's already been typed
        textPos_ = 0;
        text_.append(text);
        textRemaining_.store(text_.size(), std::memory_order_relaxed);
    }
    notify();
    return true;
}

void InputQueue::cancelText() {
    // A key the worker has down, and Shift, are still released by its next
    // steps (see typeStep()), so stopping mid-word leaves nothing held.
    std::lock_guard<std::mutex> lock(textMutex_);
    text_.clear();
    textPos_ = 0;
    textRemaining_.store(0, std::memory_order_relaxed);
}

void InputQueue::setJiggler(bool on, std::chrono::milliseconds every) {
    // TargetManager::apply() calls this on every change to a target, and
    // most aren't about the jiggler: those don't wake the worker.
    if (jiggler_.load(std::memory_order_relaxed) == on &&
        jiggleEveryMs_.load(std::memory_order_relaxed) == every.count())
        return;
    jiggleEveryMs_.store(every.count(), std::memory_order_relaxed);
    jiggler_.store(on, std::memory_order_relaxed);
    notify();   // so the worker works out its next wake again
}

std::optional<std::chrono::steady_clock::time_point> InputQueue::nextJiggleAt() const {
    if (jiggledOut_) return jiggleBackAt_;
    if (!jiggler_.load(std::memory_order_relaxed)) return std::nullopt;
    return quietSince_ + std::chrono::milliseconds(jiggleEveryMs_.load(std::memory_order_relaxed));
}

void InputQueue::returnFromJiggle() {
    if (!jiggledOut_) return;
    jiggledOut_ = false;
    if (auto b = streamManager_.inputBackend()) b->mouseMoveAbsolute(pointerX_, pointerY_);
}

// Out a step, then back. Not while the driver holds a mouse button down,
// which would make it a drag, nor while the adapter isn't ready or is busy
// (adapterBusy): then it tries again an interval later. A busy adapter holds
// the way back too, until it's done.
void InputQueue::jiggleStep(std::chrono::steady_clock::time_point now) {
    if (jiggledOut_) {
        if (now < jiggleBackAt_) return;
        if (auto b = streamManager_.inputBackend(); b && adapterBusy(*b)) {
            jiggleBackAt_ = now + kJiggleReturn;
            return;
        }
        returnFromJiggle();
        quietSince_ = now;
        return;
    }
    auto due = nextJiggleAt();
    if (!due || now < *due) return;
    quietSince_ = now;
    auto b = streamManager_.inputBackend();
    if (!b || !b->isReady() || adapterBusy(*b) || buttonsHeld_) return;
    b->mouseMoveAbsolute(pointerX_ + (pointerX_ < 0.5 ? kJiggleStep : -kJiggleStep), pointerY_);
    jiggledOut_   = true;
    jiggleBackAt_ = now + kJiggleReturn;
}

void InputQueue::releaseAllNow() {
    if (auto b = streamManager_.inputBackend()) b->releaseAll();
    buttonsHeld_ = 0;
    typingKey_ = nullptr; // released just now along with everything else
    shiftHeld_ = false;
}

bool InputQueue::typingActive() const {
    return textRemaining() > 0 || typingKey_ || shiftHeld_;
}

// One key report's worth of typing. False when there was nothing to do.
bool InputQueue::typeStep() {
    auto key = [this](const char* code, bool pressed) {
        if (auto b = streamManager_.inputBackend()) b->keyEvent(code, pressed);
    };

    if (typingKey_) {
        key(typingKey_, false);
        typingKey_ = nullptr;
        return true;
    }

    auto backend = streamManager_.inputBackend();
    if (!(backend && backend->isReady()) && textRemaining() > 0) {
        std::cerr << "InputQueue: input link went down, stopped typing with "
                  << textRemaining() << " characters left\n";
        cancelText();
    }

    std::optional<TypedKey> next;
    {
        std::lock_guard<std::mutex> lock(textMutex_);
        if (textPos_ < text_.size()) next = usKeyFor(text_[textPos_]);
    }
    if (!next) {
        if (!shiftHeld_) return false;
        key("ShiftLeft", false);
        shiftHeld_ = false;
        return true;
    }
    if (next->shift != shiftHeld_) {
        key("ShiftLeft", next->shift);
        shiftHeld_ = next->shift;
        return true;
    }
    {
        std::lock_guard<std::mutex> lock(textMutex_);
        // cancelText() may have emptied it since the look above; then the
        // next step lets go of Shift instead.
        if (textPos_ >= text_.size()) return true;
        ++textPos_;
        textRemaining_.store(text_.size() - textPos_, std::memory_order_relaxed);
    }
    key(next->code, true);
    typingKey_ = next->code;
    return true;
}

// Device I/O, so never through withInput(): see StreamManager.
void InputQueue::dispatch(const InputEvent& ev) {
    if (ev.type == InputEvent::Type::ReleaseAll) {
        releaseAllNow();
        return;
    }
    auto b = streamManager_.inputBackend();
    if (!b) return;
    switch (ev.type) {
        case InputEvent::Type::MouseButton:
            if (ev.button >= 0 && ev.button < 8) {   // for the jiggler, see jiggleStep()
                const auto bit = static_cast<uint8_t>(1u << ev.button);
                buttonsHeld_ = ev.pressed ? (buttonsHeld_ | bit) : (buttonsHeld_ & ~bit);
            }
            b->mouseButton(ev.button, ev.pressed);
            break;
        case InputEvent::Type::MouseScroll:
            b->mouseScroll(ev.delta);
            break;
        case InputEvent::Type::Key:
            b->keyEvent(std::string(ev.keyCode, ev.keyCodeLen), ev.pressed);
            break;
        case InputEvent::Type::ReleaseAll:
            break;
    }
}

void InputQueue::workerLoop() {
    if (applyRealtimeSchedulingBestEffort()) applyCpuAffinityBestEffort();

    auto nextTypeAt = std::chrono::steady_clock::now();
    quietSince_ = nextTypeAt;
    for (;;) {
        // While typing, wake for the next key report as well as for input,
        // and with the jiggler on, for its next move.
        // signaled_ is only cleared after a real acquire: on a timeout a
        // release may be pending, and clearing it then would let a second
        // release land on the semaphore (see signaled_).
        std::optional<std::chrono::steady_clock::time_point> wakeAt;
        if (typingActive()) wakeAt = nextTypeAt;
        if (auto jiggle = nextJiggleAt(); jiggle && (!wakeAt || *jiggle < *wakeAt)) wakeAt = jiggle;
        bool woken = true;
        if (wakeAt) woken = wake_.try_acquire_until(*wakeAt);
        else wake_.acquire();
        if (woken) signaled_.store(false, std::memory_order_relaxed);
        bool input = false;   // anything from the driver, or typing

        // Drain every pending discrete event first, strict FIFO — these
        // never coalesce and must never be skipped ahead of. A click lands
        // where the pointer was, not where the jiggler has it for a moment.
        for (;;) {
            size_t tail = tail_.load(std::memory_order_relaxed);
            size_t head = head_.load(std::memory_order_acquire);
            if (tail == head) break;
            InputEvent ev = ring_[tail];
            tail_.store((tail + 1) % kCapacity, std::memory_order_release);
            if (!input) returnFromJiggle();
            input = true;
            dispatch(ev);
        }

        // A release that couldn't get a ring slot (see pushReleaseAll()).
        // After the drain above so it can't overtake the events it's meant
        // to clean up after.
        if (releaseAllPending_.exchange(false, std::memory_order_acq_rel)) {
            releaseAllNow();
        }

        // Then the latest coalesced mouse position, if one landed.
        double x = 0, y = 0;
        bool hasMove;
        {
            std::lock_guard<std::mutex> lock(moveMutex_);
            hasMove = moveValid_;
            x = moveX_;
            y = moveY_;
            moveValid_ = false;
        }
        if (hasMove) {
            pointerX_   = x;
            pointerY_   = y;
            jiggledOut_ = false;   // this move puts it somewhere new anyway
            input       = true;
            if (auto b = streamManager_.inputBackend()) b->mouseMoveAbsolute(x, y);
        }

        // Then the next key report of any text being typed, once the
        // backend's interval since the last one has passed.
        auto now = std::chrono::steady_clock::now();
        if (typingActive() && now >= nextTypeAt && typeStep()) {
            input = true;
            auto b = streamManager_.inputBackend();
            nextTypeAt = now + (b ? b->typingInterval() : std::chrono::milliseconds(10));
        }

        // The jiggler waits for a quiet spell; any input starts it over.
        // Stopping, it starts nothing new.
        if (input) {
            returnFromJiggle();
            quietSince_ = now;
        } else if (running_.load(std::memory_order_relaxed)) {
            jiggleStep(now);
        }

        // Typing still under way at shutdown is abandoned: the target is
        // being stopped, which releases everything anyway. The pointer is
        // put back if the jiggler has it a step away.
        if (!running_.load(std::memory_order_relaxed)) {
            bool ringEmpty = tail_.load(std::memory_order_relaxed) == head_.load(std::memory_order_acquire);
            bool moveEmpty;
            { std::lock_guard<std::mutex> lock(moveMutex_); moveEmpty = !moveValid_; }
            if (ringEmpty && moveEmpty && !releaseAllPending_.load(std::memory_order_acquire)) {
                returnFromJiggle();
                return;
            }
        }
    }
}

} // namespace houston_kvm
