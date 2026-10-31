#include "scheduler.hpp"

#include "errors/errors.hpp"

#include <limits>
#include <quickjs.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace amj::javascript {
namespace chrono = std::chrono;
namespace errors = amj::errors;
namespace this_thread = std::this_thread;
namespace {

class ErrorCategory final : public std::error_category {
  public:
    const char* name() const noexcept override {
        return "amj.javascript";
    }

    std::string message(int value) const override {
        switch (value) {
            case static_cast<int>(ErrorCode::Stalled):
                return "No scheduled callback can make progress";
            case static_cast<int>(ErrorCode::JSException):
                return "JavaScript exception pending in context";
            default:
                return "Unknown JavaScript error";
        }
    }
};

} // namespace

std::error_code make_error_code(ErrorCode errorCode) {
    static const ErrorCategory category;
    return std::error_code(static_cast<int>(errorCode), category);
}

Scheduler::Scheduler(JSContext* jsContext) {
    if (jsContext == nullptr) {
        throw std::invalid_argument("Scheduler requires a JS context");
    }
    this->jsContext = jsContext;
    this->ownerThreadID = this_thread::get_id();
}

Scheduler::~Scheduler() {
    try {
        const errors::Error error = clear();
        if (error.errorCode) {
            std::terminate();
        }
    } catch (...) {
        std::terminate();
    }
}

std::expected<int64_t, errors::Error> Scheduler::registerCallback(
    std::function<errors::Error()> callback,
    std::optional<chrono::steady_clock::time_point> optionalTriggerTime
) {
    if (this_thread::get_id() != ownerThreadID) {
        return std::unexpected(errors::message("called registerCallback() outside owner thread"));
    }
    if (clearing) {
        return std::unexpected(errors::message("called registerCallback() inside a clear()"));
    }
    if (callback == nullptr) {
        return std::unexpected(errors::message("registered a nullptr callback function"));
    }
    const std::lock_guard<std::mutex> lock(mutex);
    if (callbackIDCounter >= std::numeric_limits<int64_t>::max()) {
        return std::unexpected(errors::message("callbackIDs exhausted"));
    }
    callbackIDCounter++;
    const int64_t callbackID = callbackIDCounter;
    callbacks.insert_or_assign(callbackID, callback);
    if (optionalTriggerTime.has_value()) {
        triggerTimes.insert_or_assign(callbackID, optionalTriggerTime.value());
    }
    return callbackID;
}

errors::Error Scheduler::unregisterCallback(int64_t callbackID) {
    if (this_thread::get_id() != ownerThreadID) {
        return errors::message("called unregisterCallback() outside owner thread");
    }
    const std::lock_guard<std::mutex> lock(mutex);
    callbacks.erase(callbackID);
    triggerTimes.erase(callbackID);
    readyTimes.erase(callbackID);
    return {};
}

errors::Error Scheduler::markReady(int64_t callbackID) {
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!callbacks.contains(callbackID)) {
            return {};
        }
        if (readyTimes.contains(callbackID)) {
            return {};
        }
        readyTimes.insert_or_assign(callbackID, chrono::steady_clock::now());
    }
    conditionVariable.notify_one();
    return {};
}

std::expected<int64_t, errors::Error> Scheduler::registerCleanup(std::function<errors::Error()> cleanup) {
    if (this_thread::get_id() != ownerThreadID) {
        return std::unexpected(errors::message("called registerCleanup() outside owner thread"));
    }
    if (clearing) {
        return std::unexpected(errors::message("called registerCleanup() inside a clear()"));
    }
    if (cleanup == nullptr) {
        return std::unexpected(errors::message("registered a nullptr cleanup function"));
    }
    if (cleanupIDCounter >= std::numeric_limits<int64_t>::max()) {
        return std::unexpected(errors::message("cleanupIDs exhausted"));
    }
    cleanupIDCounter++;
    const int64_t cleanupID = cleanupIDCounter;
    cleanups.insert_or_assign(cleanupID, cleanup);
    return cleanupID;
}

errors::Error Scheduler::cleanup(int64_t cleanupID) {
    if (this_thread::get_id() != ownerThreadID) {
        return errors::message("called cleanup() outside owner thread");
    }
    if (!cleanups.contains(cleanupID)) {
        return {};
    }
    const std::function<errors::Error()> cleanup = cleanups.at(cleanupID);
    cleanups.erase(cleanupID);
    errors::Error error;
    try {
        error = cleanup();
    } catch (const std::exception& exception) {
        error = errors::message(std::string("Cleanup threw a C++ exception: ") + exception.what());
    } catch (...) {
        error = errors::message("Cleanup threw an unknown C++ exception");
    }
    if (static_cast<int>(JS_HasException(jsContext)) != 0) {
        error.errorCode = ErrorCode::JSException;
        if (error.errorMessage.empty()) {
            error.errorMessage = "Cleanup left a pending JavaScript exception";
        }
    } else if (error.errorCode == ErrorCode::JSException) {
        error.errorCode = errors::ErrorCode::GenericError;
        error.errorMessage = "Cleanup returned JSException without a pending JavaScript exception";
    }
    return error;
}

errors::Error Scheduler::dispatch() {
    if (this_thread::get_id() != ownerThreadID) {
        return errors::message("called dispatch() outside owner thread");
    }
    if (clearing) {
        return errors::message("called dispatch() inside a clear()");
    }
    if (static_cast<int>(JS_HasException(jsContext)) != 0) {
        return errors::Error{
            .errorCode = ErrorCode::JSException,
            .errorMessage = "Cannot dispatch with a pending JavaScript exception",
        };
    }
    // Promise continuations must get a chance to clear timers between callbacks.
    if (static_cast<int>(JS_IsJobPending(JS_GetRuntime(jsContext))) != 0) {
        return {};
    }
    const chrono::steady_clock::time_point now = chrono::steady_clock::now();
    std::vector<std::pair<chrono::steady_clock::time_point, int64_t>> ready;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        ready.reserve(callbacks.size());
        for (const std::pair<const int64_t, std::function<errors::Error()>>& callback : callbacks) {
            std::optional<chrono::steady_clock::time_point> optionalReadyAt;
            if (readyTimes.contains(callback.first)) {
                optionalReadyAt = readyTimes.at(callback.first);
            }
            if (triggerTimes.contains(callback.first)) {
                const chrono::steady_clock::time_point deadline = triggerTimes.at(callback.first);
                if (deadline <= now && (!optionalReadyAt.has_value() || deadline < optionalReadyAt.value())) {
                    optionalReadyAt = deadline;
                }
            }
            if (optionalReadyAt.has_value()) {
                ready.push_back(std::pair<chrono::steady_clock::time_point, int64_t>(optionalReadyAt.value(), callback.first));
            }
        }
    }
    std::ranges::sort(ready);
    for (const std::pair<chrono::steady_clock::time_point, int64_t>& readyCallback : ready) {
        std::function<errors::Error()> callback;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            if (callbacks.contains(readyCallback.second)) {
                callback = callbacks.at(readyCallback.second);
                callbacks.erase(readyCallback.second);
            }
            triggerTimes.erase(readyCallback.second);
            readyTimes.erase(readyCallback.second);
        }
        if (callback == nullptr) {
            continue; // Earlier callbacks may unregister other callbacks.
        }
        errors::Error error;
        try {
            error = callback();
        } catch (const std::exception& exception) {
            error = errors::message(std::string("Callback threw a C++ exception: ") + exception.what());
        } catch (...) {
            error = errors::message("Callback threw an unknown C++ exception");
        }
        if (static_cast<int>(JS_HasException(jsContext)) != 0) {
            error.errorCode = ErrorCode::JSException;
            if (error.errorMessage.empty()) {
                error.errorMessage = "Callback left a pending JavaScript exception";
            }
        } else if (error.errorCode == ErrorCode::JSException) {
            error.errorCode = errors::ErrorCode::GenericError;
            error.errorMessage = "Callback returned JSException without a pending JavaScript exception";
        }
        if (error.errorCode) {
            return error;
        }
        if (static_cast<int>(JS_IsJobPending(JS_GetRuntime(jsContext))) != 0) {
            return {};
        }
    }
    return {};
}

errors::Error Scheduler::wait() {
    if (this_thread::get_id() != ownerThreadID || clearing) {
        return errors::message("Wait requires the owner thread outside clear()");
    }
    if (static_cast<int>(JS_HasException(jsContext)) != 0) {
        return errors::Error{
            .errorCode = ErrorCode::JSException,
            .errorMessage = "Cannot wait with a pending JavaScript exception",
        };
    }
    // Only the owner thread can enqueue JS jobs; workers only call markReady().
    if (static_cast<int>(JS_IsJobPending(JS_GetRuntime(jsContext))) != 0) {
        return {};
    }
    std::unique_lock<std::mutex> lock(mutex);
    while (true) {
        if (callbacks.empty()) {
            return errors::Error{
                .errorCode = ErrorCode::Stalled,
                .errorMessage = "No scheduled callback can make progress; invocation is stalled",
            };
        }
        if (!readyTimes.empty()) {
            return {};
        }
        const chrono::steady_clock::time_point now = chrono::steady_clock::now();
        std::optional<chrono::steady_clock::time_point> optionalNearestDeadline;
        for (const std::pair<const int64_t, chrono::steady_clock::time_point>& deadline : triggerTimes) {
            if (deadline.second <= now) {
                return {};
            }
            if (!optionalNearestDeadline.has_value() || deadline.second < optionalNearestDeadline.value()) {
                optionalNearestDeadline = deadline.second;
            }
        }
        // Check the predicate manually after notification, timeout, or spurious
        // wakeup. The shared mutex prevents a lost check-to-sleep notification.
        if (optionalNearestDeadline.has_value()) {
            conditionVariable.wait_until(lock, optionalNearestDeadline.value());
        } else {
            conditionVariable.wait(lock);
        }
    }
}

errors::Error Scheduler::clear() {
    if (this_thread::get_id() != ownerThreadID) {
        return errors::message("Clear requires the owner thread");
    }
    if (clearing) {
        return {};
    }
    clearing = true;
    std::map<int64_t, std::function<errors::Error()>> pendingCallbacks;
    std::map<int64_t, std::function<errors::Error()>> pendingCleanups;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        pendingCallbacks.swap(callbacks);
        pendingCleanups.swap(cleanups);
        triggerTimes.clear();
        readyTimes.clear();
    }
    errors::Error firstError;
    std::optional<JSValue> optionalFirstJSException;
    // Preserve an exception already pending on entry while allowing cleanups to
    // call QuickJS with no exception pending.
    if (static_cast<int>(JS_HasException(jsContext)) != 0) {
        optionalFirstJSException = JS_GetException(jsContext);
        firstError = errors::Error{
            .errorCode = ErrorCode::JSException,
            .errorMessage = "JavaScript exception was pending before clear()",
        };
    }
    for (const std::pair<const int64_t, std::function<errors::Error()>>& pending : pendingCleanups) {
        errors::Error error;
        try {
            error = pending.second();
        } catch (const std::exception& exception) {
            error = errors::message(std::string("Cleanup threw a C++ exception: ") + exception.what());
        } catch (...) {
            error = errors::message("Cleanup threw an unknown C++ exception");
        }
        if (static_cast<int>(JS_HasException(jsContext)) != 0) {
            const JSValue exception = JS_GetException(jsContext);
            error.errorCode = ErrorCode::JSException;
            if (error.errorMessage.empty()) {
                error.errorMessage = "Cleanup left a pending JavaScript exception";
            }
            if (!firstError.errorCode) {
                optionalFirstJSException = exception;
            } else {
                JS_FreeValue(jsContext, exception);
            }
        } else if (error.errorCode == ErrorCode::JSException) {
            error.errorCode = errors::ErrorCode::GenericError;
            error.errorMessage = "Cleanup returned JSException without a pending JavaScript exception";
        }
        if (!firstError.errorCode && error.errorCode) {
            firstError = error;
        }
    }
    pendingCallbacks.clear();
    pendingCleanups.clear();
    clearing = false;
    if (optionalFirstJSException.has_value()) {
        JS_Throw(jsContext, optionalFirstJSException.value());
    }
    return firstError;
}

} // namespace amj::javascript
