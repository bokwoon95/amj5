#pragma once

#include "errors/errors.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <type_traits>

struct JSContext;

namespace amj::javascript {
namespace chrono = std::chrono;
namespace errors = amj::errors;

enum class ErrorCode : std::uint8_t {
    Stalled = 1,
    JSException,
};

[[nodiscard]] std::error_code make_error_code(ErrorCode errorCode);

// All methods except markReady() require the constructing thread. Callbacks,
// cleanups, and destruction of their captures happen on that thread outside the
// mutex. Workers need checked access (e.g. weak_ptr); the final owner must release
// the scheduler on its constructing thread. The borrowed context must outlive it.
//
// A returned JSException means JS_GetException(context) retrieves the pending JS
// exception. Other failures do not promise an associated JS exception. Supplied
// functions return zero-code Error for success; pending JS exceptions are also
// detected. Unexpected C++ exceptions from supplied functions become GenericError.
// Allocation/synchronization failures in scheduler operations may still throw.
class Scheduler {
  public:
    explicit Scheduler(JSContext* jsContext);
    ~Scheduler();
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;

    // nullopt means readiness is explicit. An elapsed callAt or markReady() makes the
    // callback runnable. IDs are never reused, including across clear().
    [[nodiscard]] std::expected<int64_t, errors::Error> registerCallback(std::function<errors::Error()> callback, std::optional<chrono::steady_clock::time_point> callAtOption);

    // Removes the callback and its readiness timestamp/deadline; invokes no cleanup.
    // Unknown IDs are ignored.
    [[nodiscard]] errors::Error unregisterCallback(int64_t callbackID);

    // Thread-safe. Records when first marked ready and wakes wait(). Repeated
    // calls coalesce; unknown/removed IDs are ignored. Never executes JS or
    // accesses the JS context.
    [[nodiscard]] errors::Error markReady(int64_t callbackID);

    [[nodiscard]] std::expected<int64_t, errors::Error> registerCleanup(std::function<errors::Error()> cleanup);

    // Removes before invoking, so repeat/reentrant calls are no-ops. Does not
    // unregister callbacks: modules manage those independently.
    [[nodiscard]] errors::Error cleanup(int64_t cleanupID);

    // Dispatches a snapshot ordered by earliest readiness timestamp/elapsed deadline,
    // then ID. Removes callbacks before invocation; yields to pending JS jobs.
    [[nodiscard]] errors::Error dispatch();

    // Executes no callbacks/JS. Returns for pending jobs or ready callbacks;
    // otherwise waits for explicit readiness or a deadline. No callbacks means Stalled.
    [[nodiscard]] errors::Error wait();

    // Removes all callbacks/cleanups, attempts every cleanup, returns the first
    // failure, preserving its JS exception if applicable. Remains reusable.
    // Destructor calls clear() and terminates if it fails or throws.
    [[nodiscard]] errors::Error clear();

  private:
    JSContext* jsContext = nullptr;
    std::thread::id ownerThreadID = {};
    std::mutex mutex = {};
    std::condition_variable conditionVariable = {};
    // The mutex protects callback/deadline/readiness maps shared with markReady().
    std::map<int64_t, std::function<errors::Error()>> callbacks = {};
    std::map<int64_t, chrono::steady_clock::time_point> callAt = {};
    std::map<int64_t, chrono::steady_clock::time_point> markedReadyAt = {};
    std::map<int64_t, std::function<errors::Error()>> cleanups = {};
    int64_t lastCallbackID = 0;
    int64_t lastCleanupID = 0;
    bool clearing = false;
};

} // namespace amj::javascript

namespace std {

template <>
struct is_error_code_enum<amj::javascript::ErrorCode> : true_type {};

} // namespace std
