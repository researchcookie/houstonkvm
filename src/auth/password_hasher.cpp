#include "auth/password_hasher.h"

#include <utility>

namespace houston_kvm {

PasswordHasher::PasswordHasher(uWS::Loop* loop, int threads, size_t maxQueued)
    : loop_(loop), maxQueued_(maxQueued) {
    for (int i = 0; i < threads; ++i) workers_.emplace_back(&PasswordHasher::workerLoop, this);
}

PasswordHasher::~PasswordHasher() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    for (auto& t : workers_) t.join();
}

bool PasswordHasher::submit(std::function<void()> work, std::function<void()> done) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_ || queue_.size() >= maxQueued_) return false;
        queue_.push_back(Job{std::move(work), std::move(done)});
    }
    cv_.notify_one();
    return true;
}

void PasswordHasher::workerLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        job.work();
        loop_->defer(std::move(job.done));
    }
}

} // namespace houston_kvm
