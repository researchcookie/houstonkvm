#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <semaphore>
#include <string>
#include <string_view>
#include <thread>

namespace houston_kvm {

class StreamManager;

// A discrete input event — a button/scroll/key edge, never a mousemove
// (that's handled separately, see InputQueue's moveX_/moveY_ below). Every
// field is POD, so events live directly in InputQueue's ring buffer with
// zero heap allocation per event.
struct InputEvent {
    enum class Type : uint8_t { MouseButton, MouseScroll, Key, ReleaseAll } type;
    int     button = 0;      // MouseButton
    int     delta = 0;       // MouseScroll
    bool    pressed = false; // MouseButton, Key
    char    keyCode[24] = {}; // Key — JS `code` strings (e.g. "ControlLeft")
    uint8_t keyCodeLen = 0;   //   are always well under 24 bytes; longer
                              //   ones are silently truncated (see .cpp).
};

// Runs a target's input on a worker thread, so a slow backend never blocks
// the HTTP or WebSocket handler that queued it.
//
// Two kinds of input, two guarantees:
//  - Buttons, scrolls and keys are edges with no self-correction (a lost
//    keyup is a stuck key). They go through a lock-free SPSC ring
//    (kCapacity; dropped only in an extreme flood, see push()), in order.
//  - Mouse position is an absolute overwrite, so only the latest matters.
//    Moves coalesce into one slot (moveX_/moveY_/moveValid_), so a flood of
//    them (a drag) can never crowd out or delay a click.
//
// Single producer, single consumer: every push*() comes from the event-loop
// thread (routes_input.cpp) and only the worker consumes. Never push from
// another thread.
class InputQueue {
public:
    explicit InputQueue(StreamManager& streamManager);
    ~InputQueue();

    InputQueue(const InputQueue&) = delete;
    InputQueue& operator=(const InputQueue&) = delete;

    void start();

    // Never blocks the caller — the caller is always the uWS event-loop
    // thread, and blocking it would stall the whole server.
    void pushMouseMove(double x, double y); // always delivered (coalesced)
    bool pushMouseButton(int button, bool pressed); // false = queue full, dropped
    bool pushMouseScroll(int delta);                //   (see push())
    bool pushKey(std::string_view jsCode, bool pressed);

    // Releases every key and button the backend is holding (see
    // InputBackend::releaseAll()). Unlike the pushes above this can't report
    // "queue full": a dropped release is a stuck key, so it rides the ring
    // in strict order with the events before and after it when there's room,
    // and otherwise falls back to a flag the worker honours right after its
    // next drain (releaseAllPending_) — which can at worst also release a
    // key pressed a moment later, never leave one held.
    void pushReleaseAll();

    // Text to type on the target, one key at a time, already checked by
    // prepareTextForTyping(). Appended to whatever is still being typed.
    // False if that would put more than kMaxPendingText characters in
    // waiting: at 9600 baud that's already over an hour of typing.
    //
    // Typing is paced by the backend (InputBackend::typingInterval()) and
    // runs between the driver's own events, which are never held up behind
    // it. It stops, with nothing left held, on cancelText() and on
    // pushReleaseAll() (control ending in any way), and on its own if the
    // input link goes down, rather than typing the rest into a gap.
    bool pushText(std::string_view text);
    void cancelText();
    // Characters not yet typed. Safe from any thread.
    size_t textRemaining() const { return textRemaining_.load(std::memory_order_relaxed); }

    static constexpr size_t kMaxPendingText = 65536;

private:
    static constexpr size_t kCapacity = 256; // power of two

    bool push(const InputEvent& ev);
    void notify();
    void workerLoop();
    void dispatch(const InputEvent& ev);
    void releaseAllNow();
    bool typingActive() const;
    bool typeStep();

    StreamManager&    streamManager_;
    std::thread       worker_;
    std::atomic<bool> running_{false};

    InputEvent ring_[kCapacity]{};
    // head_/tail_ each live on their own cache line: head_ is only ever
    // written by the producer (uWS thread) and tail_ only by the consumer
    // (worker thread) — without the padding, both indices would share a
    // cache line and every push/pop would force a cross-core line bounce
    // regardless of the lock-free algorithm being otherwise contention-free.
    alignas(64) std::atomic<size_t> head_{0}; // next slot to write
    alignas(64) std::atomic<size_t> tail_{0}; // next slot to read

    // Latest-value coalescing slot for mousemove — see the class comment.
    // A plain mutex here (not lock-free) is fine: pushes happen at at most
    // ~60/s (rAF-throttled client-side), so it's effectively uncontended;
    // the point of keeping it separate from the ring isn't raw speed, it's
    // making the ring's "never dropped in practice" guarantee actually
    // true for discrete events.
    std::mutex moveMutex_;
    double     moveX_ = 0, moveY_ = 0;
    bool       moveValid_ = false;

    // Wakes the consumer — signaled by every push (ring or coalesced
    // move) via notify(), never released() directly. The worker drains
    // everything available (the whole ring, then the move slot, see
    // workerLoop()) each time it wakes, so it doesn't need one signal per
    // push — but release() on an already-signaled binary_semaphore is
    // undefined behavior (the standard's precondition is counter+update
    // <= max, and max is 1), so signaled_ gates it down to at most one
    // outstanding release, cleared right before the worker starts
    // draining. A push that lands mid-drain re-signals for the *next*
    // wake, which is exactly the harmless extra empty cycle this design
    // already tolerates.
    std::atomic<bool>     signaled_{false};
    std::atomic<bool>     releaseAllPending_{false}; // fallback for a full ring, see pushReleaseAll()
    std::binary_semaphore wake_{0};

    // Text waiting to be typed: text_ from textPos_ on. Written by the
    // producer (pushText/cancelText) and read by the worker one character at
    // a time, hence the mutex; textRemaining_ mirrors its length so status
    // reads never take it.
    std::mutex          textMutex_;
    std::string         text_;
    size_t              textPos_ = 0;
    std::atomic<size_t> textRemaining_{0};

    // Worker-only: where typing is within the current character. A key is
    // pressed on one step and released on the next, and Shift stays down
    // across a run of shifted characters rather than being tapped for each.
    const char* typingKey_  = nullptr; // pressed, not yet released
    bool        shiftHeld_  = false;
};

} // namespace houston_kvm
