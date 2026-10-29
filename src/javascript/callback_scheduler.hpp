#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

struct JSContext;

namespace amj::javascript {
namespace chrono = std::chrono;

// Native-only state: safe to retain and notify from a worker thread. Does not
// retain callbacks, JS values, a context, or the scheduler itself.
class Readiness {
  private:
    friend class CallbackScheduler;
    struct Wakeup;
    std::shared_ptr<Wakeup> wakeup = nullptr;
    bool active = false;
    bool used = false;
    std::optional<chrono::steady_clock::time_point> notifiedAt = {};

  public:
    // Wakeup is private; obtain instances through CallbackScheduler::makeSignal().
    explicit Readiness(std::shared_ptr<Wakeup> wakeup);
    Readiness(const Readiness&) = delete;
    Readiness& operator=(const Readiness&) = delete;
    Readiness(Readiness&&) = delete;
    Readiness& operator=(Readiness&&) = delete;

    // Remembers the first notification, including before schedule(). Subsequent
    // notifications coalesce; notifications after removal are ignored.
    void notify();
};

struct ScheduledCallback {
    std::function<void()> callback = {};
    // Only invoked by cancelAll(), not by dispatch() or unschedule().
    std::function<void()> cancel = {};
    std::optional<chrono::steady_clock::time_point> deadline = {};
    std::shared_ptr<Readiness> readySignal = nullptr;
};

// All operations except Readiness::notify() require the constructing thread.
// The borrowed context must outlive this scheduler, including its destruction.
// JS bindings can retain weak_ptr<CallbackScheduler> when the host uses shared
// ownership. This class does not manage the context/runtime lifetime.
class CallbackScheduler {
  public:
    explicit CallbackScheduler(JSContext* jsContext);
    ~CallbackScheduler();
    CallbackScheduler(const CallbackScheduler&) = delete;
    CallbackScheduler& operator=(const CallbackScheduler&) = delete;
    CallbackScheduler(CallbackScheduler&&) = delete;
    CallbackScheduler& operator=(CallbackScheduler&&) = delete;

    std::shared_ptr<Readiness> makeSignal();
    // Signals must originate from this scheduler and may be registered once.
    // Cancel-only entries are allowed, but cannot have a deadline or signal.
    int64_t schedule(ScheduledCallback scheduledCallback);
    // Unknown IDs are ignored. Does not invoke the cancellation hook.
    void unschedule(int64_t scheduledCallbackID);
    // Removes entries before calling them; yields whenever JS jobs are pending.
    void dispatch();
    // Executes no callbacks or JS. Throws if no registration can make progress.
    void wait();
    // Disables/removes all entries, attempts every cancellation hook, then
    // rethrows the first failure. Remains reusable. Destructor calls this too
    // and terminates if cancellation fails.
    void cancelAll();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl = {};
};

} // namespace amj::javascript
