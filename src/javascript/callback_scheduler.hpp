#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

struct JSContext;

namespace amj::javascript {
namespace chrono = std::chrono;

struct CallbackRecord {
    std::function<void()> callback = {};
    // Only invoked by cancelAll(), not by dispatch() or unschedule().
    std::function<void()> cancel = {};
    std::optional<chrono::steady_clock::time_point> deadline = {};
};

// All operations except notify() require the constructing thread. Workers must
// retain checked access (e.g. weak_ptr) rather than a dangling scheduler pointer.
// The borrowed context must outlive this scheduler, including its destruction.
// Its final owner must release it on the constructing thread.
class CallbackScheduler {
  public:
    explicit CallbackScheduler(JSContext* jsContext);
    ~CallbackScheduler();
    CallbackScheduler(const CallbackScheduler&) = delete;
    CallbackScheduler& operator=(const CallbackScheduler&) = delete;
    CallbackScheduler(CallbackScheduler&&) = delete;
    CallbackScheduler& operator=(CallbackScheduler&&) = delete;

    // Returns a unique ID, never reused. Callback-only, timer, and cancel-only
    // records are allowed; cancel-only records cannot have a deadline.
    int64_t schedule(CallbackRecord callbackRecord);
    // Unknown IDs are ignored. Does not invoke the cancellation hook.
    void unschedule(int64_t callbackRecordID);
    // Thread-safe. Records the first notification time and wakes wait().
    // Unknown, removed, or cancel-only IDs are ignored.
    void notify(int64_t callbackRecordID);
    // Dispatches a snapshot ordered by earliest notification/elapsed deadline,
    // then ID. Removes records before calling them; yields to pending JS jobs.
    void dispatch();
    // Executes no callbacks or JS. Returns for pending JS jobs or ready records;
    // otherwise waits for a notification or deadline. Throws if only cancel-only
    // records (or none) remain. Does not guarantee external workers will progress.
    void wait();
    // Removes all records, attempts every cancellation hook, then rethrows the
    // first failure. Hooks run outside the mutex. Remains reusable. Destructor
    // calls this too and terminates if cancellation fails.
    void cancelAll();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl = {};
};

} // namespace amj::javascript
