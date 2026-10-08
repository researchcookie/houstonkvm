#pragma once

#include <App.h>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace houston_kvm {

// Runs password hashing and verification off the uWS event loop. Argon2 is
// deliberately slow and memory-hungry (tens of milliseconds and ~64 MB per
// call at libsodium's INTERACTIVE limits), and the event loop also carries
// every MJPEG stream, input WebSocket and API call, so doing it inline let a
// burst of sign-ins stall every target's video and input.
//
// A few worker threads bound the memory in use at once; a bounded queue
// makes an overload fail fast (503) instead of piling up.
class PasswordHasher {
public:
    PasswordHasher(uWS::Loop* loop, int threads = 2, size_t maxQueued = 64);
    // Lets jobs already running finish, drops queued ones, and joins the
    // workers. A dropped job's `done` never runs; nor does the `done` of one
    // that finishes after the event loop has stopped.
    ~PasswordHasher();

    PasswordHasher(const PasswordHasher&) = delete;
    PasswordHasher& operator=(const PasswordHasher&) = delete;

    // Runs `work` on a worker thread, then `done` on the event loop. `work`
    // must not touch uWS or the database. Returns false, running neither,
    // when the queue is full. Call from the event-loop thread.
    bool submit(std::function<void()> work, std::function<void()> done);

private:
    struct Job {
        std::function<void()> work;
        std::function<void()> done;
    };

    void workerLoop();

    uWS::Loop*              loop_;
    const size_t            maxQueued_;
    std::mutex              mutex_;
    std::condition_variable cv_;
    std::deque<Job>         queue_;
    bool                    stopping_ = false;
    std::vector<std::thread> workers_;
};

} // namespace houston_kvm
