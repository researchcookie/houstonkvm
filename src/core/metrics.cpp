#include "core/metrics.h"

#include "core/http_common.h"
#include "core/target_manager.h"
#include "hid/input_backend.h"
#include "hid/input_health.h"
#include "video/audio_capture.h"
#include "video/video_health.h"

#include <libusockets.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace houston_kvm {

namespace {

// Prometheus label values: backslash, double quote and newline escaped.
std::string escapeLabel(std::string_view v) {
    std::string out;
    out.reserve(v.size());
    for (char c : v) {
        if (c == '\\')      out += "\\\\";
        else if (c == '"')  out += "\\\"";
        else if (c == '\n') out += "\\n";
        else                out += c;
    }
    return out;
}

std::string number(double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

void header(std::ostringstream& out, const char* name, const char* type, const char* help) {
    out << "# HELP " << name << ' ' << help << "\n# TYPE " << name << ' ' << type << '\n';
}

// From /proc/self: CPU seconds, resident bytes, open descriptors.
struct ProcessStats {
    double cpuSeconds = 0;
    double residentBytes = 0;
    int    openFds = 0;
};

ProcessStats processStats() {
    ProcessStats p;
    std::ifstream stat("/proc/self/stat");
    std::string line;
    if (std::getline(stat, line)) {
        // Fields after the command name, which is in parentheses and may
        // hold spaces: state is field 3, utime 14, stime 15.
        auto close = line.rfind(')');
        if (close != std::string::npos) {
            std::istringstream rest(line.substr(close + 2));
            std::string field;
            unsigned long long utime = 0, stime = 0;
            for (int i = 3; i <= 15 && rest >> field; ++i) {
                if (i == 14) utime = std::stoull(field);
                if (i == 15) stime = std::stoull(field);
            }
            p.cpuSeconds = static_cast<double>(utime + stime) / static_cast<double>(sysconf(_SC_CLK_TCK));
        }
    }
    std::ifstream statm("/proc/self/statm");
    unsigned long long size = 0, resident = 0;
    if (statm >> size >> resident)
        p.residentBytes = static_cast<double>(resident) * static_cast<double>(sysconf(_SC_PAGESIZE));
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator("/proc/self/fd", ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec))
        ++p.openFds;
    return p;
}

constexpr VideoState kVideoStates[] = {VideoState::Off, VideoState::Starting, VideoState::Good,
                                       VideoState::NoSignal, VideoState::Reconnecting,
                                       VideoState::Absent};
constexpr HealthState kInputStates[] = {HealthState::Unknown, HealthState::Good,
                                        HealthState::Degraded, HealthState::Down};
constexpr AudioState kAudioStates[] = {AudioState::Off, AudioState::Unsupported, AudioState::Idle,
                                       AudioState::Good, AudioState::Absent};

} // namespace

Metrics::Metrics(uWS::Loop* loop) : loop_(loop), started_(std::chrono::system_clock::now()) {}

Metrics::~Metrics() { stop(); }

void Metrics::count(const Event& e) {
    ++events_[{std::string(e.type), std::string(e.outcome)}];
}

void Metrics::start() {
    // fallthrough 0 and an explicit close in stop(): see
    // TargetManager::startDriverRevalidation for why.
    timer_ = us_create_timer(reinterpret_cast<us_loop_t*>(loop_), 0, sizeof(Metrics*));
    *static_cast<Metrics**>(us_timer_ext(timer_)) = this;
    lastTick_ = std::chrono::steady_clock::now();
    us_timer_set(timer_, &Metrics::onTimer, kLagIntervalMs, kLagIntervalMs);
}

void Metrics::stop() {
    if (!timer_) return;
    us_timer_close(timer_);
    timer_ = nullptr;
}

void Metrics::onTimer(struct us_timer_t* timer) {
    auto* self = *static_cast<Metrics**>(us_timer_ext(timer));
    auto now = std::chrono::steady_clock::now();
    double late = std::chrono::duration<double>(now - self->lastTick_).count() -
                  kLagIntervalMs / 1000.0;
    self->lastTick_ = now;
    self->observeLag(late > 0 ? late : 0);
}

void Metrics::observeLag(double seconds) {
    ++lagCount_;
    lagSum_ += seconds;
    for (size_t i = 0; i < kLagBuckets.size(); ++i) {
        if (seconds <= kLagBuckets[i]) {
            ++lagBuckets_[i];
            break;
        }
    }
}

std::string Metrics::render(Database& db, TargetManager& targets) const {
    std::ostringstream out;

    header(out, "houstonkvm_build_info", "gauge", "The running version.");
    out << "houstonkvm_build_info{version=\"" << escapeLabel(HOUSTONKVM_VERSION) << "\"} 1\n";

    header(out, "houstonkvm_events_total", "counter",
           "Audited actions since the server started, by type and outcome.");
    for (const auto& [key, n] : events_)
        out << "houstonkvm_events_total{type=\"" << escapeLabel(key.first) << "\",outcome=\""
            << escapeLabel(key.second) << "\"} " << n << '\n';

    header(out, "houstonkvm_event_loop_lag_seconds", "histogram",
           "How late the event loop ran a timer due every 100 ms. Every request and stream "
           "waits while the loop is late.");
    uint64_t cumulative = 0;
    for (size_t i = 0; i < kLagBuckets.size(); ++i) {
        cumulative += lagBuckets_[i];
        out << "houstonkvm_event_loop_lag_seconds_bucket{le=\"" << number(kLagBuckets[i]) << "\"} "
            << cumulative << '\n';
    }
    out << "houstonkvm_event_loop_lag_seconds_bucket{le=\"+Inf\"} " << lagCount_ << '\n'
        << "houstonkvm_event_loop_lag_seconds_sum " << number(lagSum_) << '\n'
        << "houstonkvm_event_loop_lag_seconds_count " << lagCount_ << '\n';

    // Per target. Each family's lines are gathered separately, since the
    // format wants a family's samples together.
    std::ostringstream enabled, running, video, videoReconnects, input, inputFailed, inputReplyMax,
        inputReconnects, inputDropped, audio, viewers, controlled;
    for (const auto& t : db.listTargets()) {
        std::string labels = "target_id=\"" + std::to_string(t.id) + "\",target=\"" +
                             escapeLabel(t.name) + "\"";
        enabled << "houstonkvm_target_enabled{" << labels << "} " << (t.enabled ? 1 : 0) << '\n';
        auto* rt = targets.find(t.id);
        running << "houstonkvm_target_running{" << labels << "} " << (rt ? 1 : 0) << '\n';
        if (!rt) continue;

        auto vh = rt->streamManager.videoHealth();
        for (auto s : kVideoStates)
            video << "houstonkvm_target_video_state{" << labels << ",state=\"" << videoStateName(s)
                  << "\"} " << (vh.state == s ? 1 : 0) << '\n';
        videoReconnects << "houstonkvm_target_video_reconnects_total{" << labels << "} "
                        << vh.reconnects << '\n';

        auto ih = rt->streamManager.withInput(
            [](InputBackend* b) { return b ? b->health() : InputHealth{}; });
        for (auto s : kInputStates)
            input << "houstonkvm_target_input_health{" << labels << ",state=\"" << healthStateName(s)
                  << "\"} " << (ih.state == s ? 1 : 0) << '\n';
        inputFailed << "houstonkvm_target_input_checks_failed{" << labels << "} "
                    << ih.checksFailed << '\n';
        inputReplyMax << "houstonkvm_target_input_reply_seconds_max{" << labels << "} "
                      << number(ih.replyMsMax / 1000.0) << '\n';
        inputReconnects << "houstonkvm_target_input_reconnects{" << labels << "} "
                        << ih.reconnects << '\n';
        inputDropped << "houstonkvm_target_input_dropped_commands{" << labels << "} "
                     << ih.droppedCommands << '\n';

        auto ah = rt->streamManager.audioHealth();
        for (auto s : kAudioStates)
            audio << "houstonkvm_target_audio_state{" << labels << ",state=\"" << audioStateName(s)
                  << "\"} " << (ah.state == s ? 1 : 0) << '\n';

        viewers << "houstonkvm_target_viewers{" << labels << ",transport=\"mjpeg\"} "
                << rt->broadcaster->viewers() << '\n'
                << "houstonkvm_target_viewers{" << labels << ",transport=\"webrtc\"} "
                << rt->streamManager.webrtcViewers() << '\n';
        controlled << "houstonkvm_target_controlled{" << labels << "} "
                   << (rt->driverToken.empty() ? 0 : 1) << '\n';
    }
    auto family = [&out](const char* name, const char* type, const char* help,
                         const std::ostringstream& lines) {
        header(out, name, type, help);
        out << lines.str();
    };
    family("houstonkvm_target_enabled", "gauge", "Whether the target is enabled.", enabled);
    family("houstonkvm_target_running", "gauge",
           "Whether the target's capture, input and streaming are running.", running);
    family("houstonkvm_target_video_state", "gauge", "The target's video state; 1 for the current one.",
           video);
    family("houstonkvm_target_video_reconnects_total", "counter",
           "Times the video came back by itself since the target started.", videoReconnects);
    family("houstonkvm_target_input_health", "gauge",
           "The target's input link health; 1 for the current state.", input);
    family("houstonkvm_target_input_checks_failed", "gauge",
           "Input link checks that got no answer in the last hour.", inputFailed);
    family("houstonkvm_target_input_reply_seconds_max", "gauge",
           "Slowest input link check reply in the last hour.", inputReplyMax);
    family("houstonkvm_target_input_reconnects", "gauge",
           "Times the input adapter was reopened in the last hour.", inputReconnects);
    family("houstonkvm_target_input_dropped_commands", "gauge",
           "Input commands dropped in the last hour because the link was down.", inputDropped);
    family("houstonkvm_target_audio_state", "gauge", "The target's sound state; 1 for the current one.",
           audio);
    family("houstonkvm_target_viewers", "gauge", "Viewers watching the target, by transport.", viewers);
    family("houstonkvm_target_controlled", "gauge", "Whether someone is driving the target.",
           controlled);

    auto p = processStats();
    header(out, "process_cpu_seconds_total", "counter", "User and system CPU time spent.");
    out << "process_cpu_seconds_total " << number(p.cpuSeconds) << '\n';
    header(out, "process_resident_memory_bytes", "gauge", "Resident memory size.");
    out << "process_resident_memory_bytes " << number(p.residentBytes) << '\n';
    header(out, "process_open_fds", "gauge", "Open file descriptors.");
    out << "process_open_fds " << p.openFds << '\n';
    header(out, "process_start_time_seconds", "gauge", "When the process started, in Unix time.");
    out << "process_start_time_seconds "
        << number(std::chrono::duration<double>(started_.time_since_epoch()).count()) << '\n';
    return out.str();
}

template <bool SSL>
void registerMetricsRoutes(uWS::TemplatedApp<SSL>& app, Auth& auth, Database& db,
                           TargetManager& targets, const Metrics& metrics) {
    app.get("/metrics", [&auth, &db, &targets, &metrics](auto* res, auto* req) {
        auto who = authenticate(auth, db, res, req, /*metricsRoute=*/true);
        if (!who) return;
        // A metrics token keeps its account's role, so it stops working if
        // that account is no longer an Owner.
        if (who->role != Role::Owner) {
            res->writeStatus("403 Forbidden")->end("Insufficient permissions");
            return;
        }
        res->writeHeader("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
           ->writeHeader("Cache-Control", "no-store")
           ->end(metrics.render(db, targets));
    });
}

template void registerMetricsRoutes<false>(uWS::App&, Auth&, Database&, TargetManager&,
                                           const Metrics&);
template void registerMetricsRoutes<true>(uWS::SSLApp&, Auth&, Database&, TargetManager&,
                                          const Metrics&);

} // namespace houston_kvm
