#include "auth/rate_limiter.h"

namespace houston_kvm {

namespace {
constexpr int                     kMaxFailures  = 5;
constexpr std::chrono::minutes    kWindow{5};
constexpr std::chrono::minutes    kLockout{5};
constexpr size_t                  kSweepThreshold = 5000;
} // namespace

int LoginRateLimiter::checkLocked(const std::string& ip) {
    auto it = entries_.find(ip);
    if (it == entries_.end()) return 0;

    auto now = std::chrono::steady_clock::now();
    if (now < it->second.lockedUntil) {
        return static_cast<int>(
            std::chrono::duration_cast<std::chrono::seconds>(it->second.lockedUntil - now).count()) + 1;
    }
    return 0;
}

int LoginRateLimiter::beginAttempt(const std::string& ip) {
    if (int wait = checkLocked(ip)) return wait;
    auto& entry = entries_[ip];
    auto now = std::chrono::steady_clock::now();
    int failures = now - entry.windowStart > kWindow ? 0 : entry.failures;
    // Already enough guesses in flight to lock this address out if they
    // all fail: wait for those to be decided.
    if (failures + entry.inFlight >= kMaxFailures) return 1;
    ++entry.inFlight;
    return 0;
}

void LoginRateLimiter::endAttempt(Entry& entry) {
    if (entry.inFlight > 0) --entry.inFlight;
}

void LoginRateLimiter::cancelAttempt(const std::string& ip) {
    auto it = entries_.find(ip);
    if (it == entries_.end()) return;
    endAttempt(it->second);
    if (it->second.inFlight == 0 && it->second.failures == 0) entries_.erase(it);
}

void LoginRateLimiter::recordFailure(const std::string& ip) {
    auto now = std::chrono::steady_clock::now();
    auto& entry = entries_[ip];
    endAttempt(entry);

    if (entry.failures == 0 || now - entry.windowStart > kWindow) {
        entry.failures    = 0;
        entry.windowStart = now;
    }
    ++entry.failures;

    if (entry.failures >= kMaxFailures)
        entry.lockedUntil = now + kLockout;

    sweepIfLarge();
}

void LoginRateLimiter::recordSuccess(const std::string& ip) {
    auto it = entries_.find(ip);
    if (it == entries_.end()) return;
    endAttempt(it->second);
    // Other attempts from this address may still be in flight; keep their
    // count so they can't slip past the limit once this one clears it.
    if (it->second.inFlight > 0) {
        it->second.failures = 0;
        it->second.lockedUntil = {};
    } else {
        entries_.erase(it);
    }
}

void LoginRateLimiter::sweepIfLarge() {
    if (entries_.size() <= kSweepThreshold) return;

    auto now = std::chrono::steady_clock::now();
    for (auto it = entries_.begin(); it != entries_.end();) {
        bool lockExpired   = now >= it->second.lockedUntil;
        bool windowExpired = now - it->second.windowStart > kWindow;
        if (lockExpired && windowExpired && it->second.inFlight == 0)
            it = entries_.erase(it);
        else
            ++it;
    }
}

} // namespace houston_kvm
