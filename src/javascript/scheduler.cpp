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
    std::optional<chrono::steady_clock::time_point> callAtOption
) {
    if (this_thread::get_id() != ownerThreadID) {
        return std::unexpected(errors::generic("called registerCallback() outside owner thread"));
    }
    if (clearing) {
        return std::unexpected(errors::generic("called registerCallback() inside a clear()"));
    }
    if (!callback) {
        return std::unexpected(errors::Error{
            .errorCode = std::make_error_code(std::errc::invalid_argument),
            .errorMessage = "Cannot register an empty callback",
        });
    }
    const std::lock_guard<std::mutex> lock(mutex);
    if (callbacks.size() >= 4096 || lastCallbackID >= 9007199254740991) {
        return std::unexpected(errors::generic("Pending callback limit or callback IDs exhausted"));
    }
    const int64_t callbackID = lastCallbackID + 1;
    if (callAtOption.has_value()) {
        callAt.emplace(callbackID, callAtOption.value());
    }
    try {
        callbacks.emplace(callbackID, std::move(callback));
    } catch (...) {
        callAt.erase(callbackID);
        throw;
    }
    lastCallbackID = callbackID;
    return callbackID;
}

errors::Error Scheduler::unregisterCallback(int64_t callbackID) {
    if (this_thread::get_id() != ownerThreadID) {
        return errors::generic("called unregisterCallback() outside owner thread");
    }
    std::map<int64_t, std::function<errors::Error()>>::node_type removed;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        removed = callbacks.extract(callbackID);
        callAt.erase(callbackID);
        markedReadyAt.erase(callbackID);
    }
    // Destroy callback captures outside the mutex, on the owner thread.
    return {};
}

errors::Error Scheduler::markReady(int64_t callbackID) {
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!callbacks.contains(callbackID)) {
            return {};
        }
        const bool inserted = markedReadyAt.try_emplace(callbackID, chrono::steady_clock::now()).second;
        if (!inserted) {
            return {};
        }
    }
    conditionVariable.notify_one();
    return {};
}

std::expected<int64_t, errors::Error> Scheduler::registerCleanup(std::function<errors::Error()> cleanup) {
    if (this_thread::get_id() != ownerThreadID) {
        return std::unexpected(errors::generic("called registerCleanup() outside owner thread"));
    }
    if (clearing) {
        return std::unexpected(errors::generic("called registerCleanup() inside a clear()"));
    }
    if (cleanup == nullptr) {
        return std::unexpected(errors::generic("registered a nullptr cleanup function"));
    }
    if (lastCleanupID >= std::numeric_limits<int64_t>::max()) {
        return std::unexpected(errors::generic("cleanup IDs exhausted"));
    }
    const int64_t cleanupID = lastCleanupID + 1;
    cleanups.emplace(cleanupID, std::move(cleanup));
    lastCleanupID = cleanupID;
    return cleanupID;
}

errors::Error Scheduler::cleanup(int64_t cleanupID) {
    if (this_thread::get_id() != ownerThreadID) {
        return errors::generic("Cleanup execution requires the owner thread");
    }
    const std::map<int64_t, std::function<errors::Error()>>::node_type pending = cleanups.extract(cleanupID);
    if (pending.empty()) {
        return {};
    }
    errors::Error error;
    try {
        error = pending.mapped()();
    } catch (const std::exception& exception) {
        error = errors::generic(std::string("Cleanup threw a C++ exception: ") + exception.what());
    } catch (...) {
        error = errors::generic("Cleanup threw an unknown C++ exception");
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
    if (this_thread::get_id() != ownerThreadID || clearing) {
        return errors::generic("Dispatch requires the owner thread outside clear()");
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
            std::optional<chrono::steady_clock::time_point> readyAtOption;
            const std::map<int64_t, chrono::steady_clock::time_point>::const_iterator readiness = markedReadyAt.find(callback.first);
            if (readiness != markedReadyAt.end()) {
                readyAtOption = readiness->second;
            }
            const std::map<int64_t, chrono::steady_clock::time_point>::const_iterator deadline = callAt.find(callback.first);
            if (deadline != callAt.end() && deadline->second <= now && (!readyAtOption.has_value() || deadline->second < readyAtOption.value())) {
                readyAtOption = deadline->second;
            }
            if (readyAtOption.has_value()) {
                ready.emplace_back(readyAtOption.value(), callback.first);
            }
        }
    }
    std::ranges::sort(ready);
    for (const std::pair<chrono::steady_clock::time_point, int64_t>& readyCallback : ready) {
        std::map<int64_t, std::function<errors::Error()>>::node_type pending;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            pending = callbacks.extract(readyCallback.second);
            callAt.erase(readyCallback.second);
            markedReadyAt.erase(readyCallback.second);
        }
        if (pending.empty()) {
            continue; // Earlier callbacks may unregister other callbacks.
        }
        errors::Error error;
        try {
            error = pending.mapped()();
        } catch (const std::exception& exception) {
            error = errors::generic(std::string("Callback threw a C++ exception: ") + exception.what());
        } catch (...) {
            error = errors::generic("Callback threw an unknown C++ exception");
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
        return errors::generic("Wait requires the owner thread outside clear()");
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
        if (!markedReadyAt.empty()) {
            return {};
        }
        const chrono::steady_clock::time_point now = chrono::steady_clock::now();
        std::optional<chrono::steady_clock::time_point> nearestDeadlineOption;
        for (const std::pair<const int64_t, chrono::steady_clock::time_point>& deadline : callAt) {
            if (deadline.second <= now) {
                return {};
            }
            if (!nearestDeadlineOption.has_value() || deadline.second < nearestDeadlineOption.value()) {
                nearestDeadlineOption = deadline.second;
            }
        }
        // Check the predicate manually after notification, timeout, or spurious
        // wakeup. The shared mutex prevents a lost check-to-sleep notification.
        if (nearestDeadlineOption.has_value()) {
            conditionVariable.wait_until(lock, nearestDeadlineOption.value());
        } else {
            conditionVariable.wait(lock);
        }
    }
}

errors::Error Scheduler::clear() {
    if (this_thread::get_id() != ownerThreadID) {
        return errors::generic("Clear requires the owner thread");
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
        callAt.clear();
        markedReadyAt.clear();
    }
    errors::Error firstError;
    std::optional<JSValue> firstJSExceptionOption;
    // Preserve an exception already pending on entry while allowing cleanups to
    // call QuickJS with no exception pending.
    if (static_cast<int>(JS_HasException(jsContext)) != 0) {
        firstJSExceptionOption = JS_GetException(jsContext);
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
            error = errors::generic(std::string("Cleanup threw a C++ exception: ") + exception.what());
        } catch (...) {
            error = errors::generic("Cleanup threw an unknown C++ exception");
        }
        if (static_cast<int>(JS_HasException(jsContext)) != 0) {
            const JSValue exception = JS_GetException(jsContext);
            error.errorCode = ErrorCode::JSException;
            if (error.errorMessage.empty()) {
                error.errorMessage = "Cleanup left a pending JavaScript exception";
            }
            if (!firstError.errorCode) {
                firstJSExceptionOption = exception;
            } else {
                JS_FreeValue(jsContext, exception);
            }
        } else if (error.errorCode == ErrorCode::JSException) {
            error.errorCode = errors::ErrorCode::GenericError;
            error.errorMessage = "Cleanup returned JSException without a pending JavaScript exception";
        }
        if (!firstError.errorCode && error.errorCode) {
            firstError = std::move(error);
        }
    }
    pendingCallbacks.clear();
    pendingCleanups.clear();
    clearing = false;
    if (firstJSExceptionOption.has_value()) {
        JS_Throw(jsContext, firstJSExceptionOption.value());
    }
    return firstError;
}

} // namespace amj::javascript
