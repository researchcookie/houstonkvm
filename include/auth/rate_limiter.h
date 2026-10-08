#pragma once

#include <chrono>
#include <string>
#include <unordered_map>

namespace houston_kvm {

// Per-IP lockout after repeated auth failures, to slow down automated
// credential guessing on /api/login and /api/account/password. This is not
// a substitute for slow password hashing (already handled by libsodium's
// pwhash in Auth) — it just raises the cost of scripted brute-forcing.
//
// Password checks finish later, on PasswordHasher's threads, so attempts
// still being checked count toward the limit too: otherwise a burst sent
// all at once would get every guess checked before the first failure lands.
//
// Only ever touched from the uWS event-loop thread (same as the rest of
// routes_auth.cpp), so no internal locking is needed.
class LoginRateLimiter {
public:
    // Returns seconds remaining until the given IP may try again, or 0 if
    // the request may proceed now.
    int checkLocked(const std::string& ip);

    // checkLocked(), and if the attempt may proceed, counts it as in
    // flight. Every attempt that begins must end in exactly one of
    // recordFailure(), recordSuccess() or cancelAttempt().
    int beginAttempt(const std::string& ip);
    void cancelAttempt(const std::string& ip);

    void recordFailure(const std::string& ip);
    void recordSuccess(const std::string& ip);

private:
    struct Entry {
        int failures = 0;
        int inFlight = 0;
        std::chrono::steady_clock::time_point windowStart;
        std::chrono::steady_clock::time_point lockedUntil;
    };

    void endAttempt(Entry& entry);
    void sweepIfLarge();

    std::unordered_map<std::string, Entry> entries_;
};

} // namespace houston_kvm
