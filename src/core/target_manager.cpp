#include "core/target_manager.h"

#include <iostream>
#include <thread>
#include <vector>

namespace houston_kvm {

namespace {

std::shared_ptr<Broadcaster> makeBroadcaster(uWS::Loop* loop) {
    auto b = std::make_shared<Broadcaster>();
    b->loop = loop;
    return b;
}

} // namespace

TargetRuntime::TargetRuntime(int64_t id_, std::string roomId_, SelectiveForwardingUnit& sfu,
                             uWS::Loop* loop, const Database::TargetSettings& settings,
                             const ControlHooks& hooks)
    : id(id_), roomId(std::move(roomId_)),
      broadcaster(makeBroadcaster(loop)),
      streamManager(sfu, *broadcaster, roomId, kTargetPeerId),
      inputQueue(streamManager),
      appliedSettings(settings),
      hooks_(hooks) {
    streamManager.applySettings(settings);
    inputQueue.start();
}

void TargetRuntime::takeControl(const std::string& token, const Actor& who) {
    if (driverToken == token) return;
    inputQueue.pushReleaseAll();
    driverToken = token;
    driverName = who.username;
    driver = who;
    controlSession = hooks_.started ? hooks_.started(*this, who) : 0;
}

void TargetRuntime::releaseControl(std::string_view reason) {
    if (driverToken.empty()) return;
    if (hooks_.ended) hooks_.ended(*this, controlSession, reason);
    driverToken.clear();
    driverName.clear();
    driver = Actor{};
    controlSession = 0;
    inputQueue.pushReleaseAll();
}

void TargetRuntime::recordKeyPress() {
    if (controlSession && hooks_.keyPressed) hooks_.keyPressed(controlSession);
}

void TargetRuntime::recordMouse() {
    if (controlSession && hooks_.mouse) hooks_.mouse(controlSession);
}

TargetManager::TargetManager(SelectiveForwardingUnit& sfu, Database& db, uWS::Loop* loop)
    : sfu_(sfu), db_(db), loop_(loop) {}

TargetManager::~TargetManager() {
    {
        std::unique_lock<std::mutex> lock(jobsMutex_);
        jobsCv_.wait(lock, [this] { return jobs_ == 0; });
    }
    stopBackground();
    // Leave every target with nothing held down. Queued from here, on the
    // event-loop thread, because InputQueue is single-producer; each queue
    // drains its ring before its worker exits, so this lands before teardown
    // below closes the backend.
    for (auto& [id, runtime] : runtimes_) runtime->inputQueue.pushReleaseAll();
    // Each runtime's teardown can block for seconds (see the class comment);
    // do them in parallel so shutdown time is the slowest target's, not the
    // sum of all of them.
    std::vector<std::thread> teardown;
    for (auto& [id, runtime] : runtimes_)
        teardown.emplace_back([r = std::move(runtime)]() mutable { r.reset(); });
    for (auto& t : teardown) t.join();
}

void TargetManager::spawn(std::function<void()> job) {
    {
        std::lock_guard<std::mutex> lock(jobsMutex_);
        ++jobs_;
    }
    std::thread([this, job = std::move(job)]() mutable {
        job();
        job = nullptr; // release captures before signalling completion
        std::lock_guard<std::mutex> lock(jobsMutex_);
        --jobs_;
        jobsCv_.notify_all();
    }).detach();
}

void TargetManager::loadAll() {
    for (const auto& t : db_.listTargets()) apply(t);
}

TargetRuntime* TargetManager::find(int64_t id) {
    auto it = runtimes_.find(id);
    return it == runtimes_.end() ? nullptr : it->second.get();
}

void TargetManager::apply(const Database::Target& t) {
    if (t.isDefault) defaultId_ = t.id;

    auto it = runtimes_.find(t.id);
    if (!t.enabled) {
        if (it != runtimes_.end()) {
            auto runtime = std::move(it->second);
            runtimes_.erase(it);
            retire(std::move(runtime));
        }
        return;
    }

    if (it != runtimes_.end()) {
        if (!(it->second->appliedSettings == t.settings)) {
            it->second->streamManager.applySettings(t.settings);
            it->second->appliedSettings = t.settings;
        }
        return;
    }

    if (startPending_.count(t.id)) return; // the pending start re-reads the row when it fires
    bool busy;
    {
        std::lock_guard<std::mutex> lock(jobsMutex_);
        busy = retiring_ > 0;
    }
    if (busy) {
        startAfterRetiring(t.id);
        return;
    }
    // The generation makes the SFU room unique per runtime: a retiring
    // runtime's late removeParticipant() (see retire()) can then never
    // tear down the participant of a runtime re-created under the same id.
    std::string room = "target-" + std::to_string(t.id) + "-" + std::to_string(nextGeneration_++);
    runtimes_.emplace(t.id, std::make_unique<TargetRuntime>(t.id, std::move(room), sfu_, loop_,
                                                            t.settings, hooks_));
    std::cout << "Target " << t.id << " (" << t.name << "): started\n";
}

void TargetManager::remove(int64_t id) {
    if (defaultId_ == id) defaultId_ = 0;
    startPending_.erase(id);
    auto it = runtimes_.find(id);
    if (it == runtimes_.end()) return;
    auto runtime = std::move(it->second);
    runtimes_.erase(it);
    retire(std::move(runtime));
}

void TargetManager::retire(std::unique_ptr<TargetRuntime> runtime) {
    // Drop the driver lock and kick every viewer now, on this thread — both
    // touch uWS state that must only be used from the event loop. The slow
    // part (joining worker threads) happens off it.
    // Queued unconditionally, not just when the lock is held: the HID chip
    // keeps its last report after we stop talking to it, so a stopped target
    // must be left with nothing pressed. InputQueue drains its ring before
    // its worker exits, so this runs before the backend is destroyed.
    runtime->releaseControl("target_stopped");
    runtime->inputQueue.pushReleaseAll();
    runtime->broadcaster->closeAll();
    {
        std::lock_guard<std::mutex> lock(jobsMutex_);
        ++retiring_;
    }
    std::cout << "Target " << runtime->id << ": stopping\n";
    // std::function needs a copyable callable, so the runtime rides in a
    // shared holder and is destroyed explicitly (not by "last copy dies").
    auto holder = std::make_shared<std::unique_ptr<TargetRuntime>>(std::move(runtime));
    spawn([this, holder] {
        std::string room = (*holder)->roomId;
        holder->reset(); // joins capture/publisher/input threads, releases devices
        // Nothing removes a publisher from the SFU when it goes away (a
        // reconfigure just replaces it in place), so without this the room
        // and any of its subscribers would linger forever.
        sfu_.removeParticipant(room, kTargetPeerId);
        std::lock_guard<std::mutex> lock(jobsMutex_);
        --retiring_;
        jobsCv_.notify_all();
    });
}

void TargetManager::startAfterRetiring(int64_t id) {
    startPending_.insert(id);
    spawn([this, id] {
        {
            std::unique_lock<std::mutex> lock(jobsMutex_);
            jobsCv_.wait(lock, [this] { return retiring_ == 0; });
        }
        // Back on the event-loop thread, re-read the row: it may have been
        // edited, disabled, or deleted while we waited.
        loop_->defer([this, id] {
            startPending_.erase(id);
            if (auto t = db_.getTarget(id)) apply(*t);
        });
    });
}

void TargetManager::releaseControlFor(const std::string& token, std::string_view reason) {
    if (token.empty()) return;
    for (auto& [id, runtime] : runtimes_)
        if (runtime->driverToken == token) runtime->releaseControl(reason);
}

void TargetManager::onRevalidateTimer(struct us_timer_t* timer) {
    (*static_cast<TargetManager**>(us_timer_ext(timer)))->revalidateDrivers();
}

void TargetManager::revalidateDrivers() {
    for (auto& [id, runtime] : runtimes_) {
        if (runtime->driverToken.empty() || stillValid_(runtime->driverToken)) continue;
        std::cout << "Target " << id << ": driver's credentials are no longer valid — "
                  << "releasing control\n";
        runtime->releaseControl("credentials_revoked");
    }
}

void TargetManager::startDriverRevalidation(std::function<bool(const std::string&)> stillValid,
                                            int intervalMs) {
    stillValid_ = std::move(stillValid);
    // fallthrough must be 0. It looks like 1 ("don't keep the loop alive")
    // is the safe choice, but uSockets only counts a fallthrough poll when
    // it's created, while us_timer_close() uncounts it unconditionally: the
    // loop's poll count then ends up one short and `while (num_polls)` never
    // terminates, so shutdown hangs. Counted and closed explicitly instead
    // (see stopBackground(), which shutdown must call).
    revalidateTimer_ = us_create_timer(reinterpret_cast<us_loop_t*>(loop_), 0, sizeof(TargetManager*));
    *static_cast<TargetManager**>(us_timer_ext(revalidateTimer_)) = this;
    us_timer_set(revalidateTimer_, &TargetManager::onRevalidateTimer, intervalMs, intervalMs);
}

void TargetManager::stopBackground() {
    if (!revalidateTimer_) return;
    us_timer_close(revalidateTimer_);
    revalidateTimer_ = nullptr;
}

void TargetManager::closeAllStreams() {
    for (auto& [id, runtime] : runtimes_) runtime->broadcaster->closeAll();
}

} // namespace houston_kvm
