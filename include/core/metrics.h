#pragma once

#include "auth/auth.h"
#include "core/database.h"
#include "core/events.h"

#include <App.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace houston_kvm {

class TargetManager;

// What GET /metrics reports, in Prometheus's text format:
//  - every EventBus event, counted by type and outcome (the same stream the
//    audit log records, so sign-ins, failures and control changes are
//    counted without instrumenting a route twice);
//  - how late the event loop runs (a stall here stalls every stream and
//    request, so it's the first thing to alert on);
//  - each target's video, input-link and viewer state;
//  - the usual process_* figures.
// Labels are only ever fixed lists or target ids and names, never anything a
// client sent, so the number of series stays bounded.
//
// Event-loop thread only, like the EventBus.
class Metrics {
public:
    explicit Metrics(uWS::Loop* loop);
    ~Metrics();

    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;

    void count(const Event& e);

    // The lag probe: a timer that should fire every kLagIntervalMs, and how
    // much later than that it did.
    void start();
    void stop();

    std::string render(Database& db, TargetManager& targets) const;

    static constexpr int kLagIntervalMs = 100;
    static constexpr std::array<double, 9> kLagBuckets{
        0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5};

private:
    static void onTimer(struct us_timer_t* timer);
    void observeLag(double seconds);

    uWS::Loop*            loop_;
    struct us_timer_t*    timer_ = nullptr;
    std::chrono::steady_clock::time_point lastTick_;
    const std::chrono::system_clock::time_point started_;

    std::map<std::pair<std::string, std::string>, uint64_t> events_;
    std::array<uint64_t, kLagBuckets.size()> lagBuckets_{};  // not cumulative
    uint64_t lagCount_ = 0;
    double   lagSum_   = 0;
};

// GET /metrics. Readable by an Owner (browser session or full token) and by
// a metrics-scoped token an Owner made, which can read nothing else.
template <bool SSL>
void registerMetricsRoutes(uWS::TemplatedApp<SSL>& app, Auth& auth, Database& db,
                           TargetManager& targets, const Metrics& metrics);

} // namespace houston_kvm
