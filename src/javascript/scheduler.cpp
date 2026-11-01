#include "scheduler.hpp"

#include "errors/errors.hpp"

#include <limits>
#include <quickjs.h>

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

std::expected<int64_t, errors::Error> Scheduler::registerCallback(std::function<errors::Error()> callback, std::optional<chrono::steady_clock::time_point> triggerTime) {
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
    callbackMap.insert_or_assign(callbackID, callback);
    if (triggerTime.has_value()) {
        triggerTimeMap.insert_or_assign(callbackID, triggerTime.value());
    }
    return callbackID;
}

errors::Error Scheduler::unregisterCallback(int64_t callbackID) {
    if (this_thread::get_id() != ownerThreadID) {
        return errors::message("called unregisterCallback() outside owner thread");
    }
    const std::lock_guard<std::mutex> lock(mutex);
    callbackMap.erase(callbackID);
    triggerTimeMap.erase(callbackID);
    readyTimeMap.erase(callbackID);
    return {};
}

errors::Error Scheduler::markReady(int64_t callbackID) {
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!callbackMap.contains(callbackID)) {
            return {};
        }
        if (readyTimeMap.contains(callbackID)) {
            return {};
        }
        readyTimeMap.insert_or_assign(callbackID, chrono::steady_clock::now());
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
    cleanupMap.insert_or_assign(cleanupID, cleanup);
    return cleanupID;
}

errors::Error Scheduler::cleanup(int64_t cleanupID) {
    if (this_thread::get_id() != ownerThreadID) {
        return errors::message("called cleanup() outside owner thread");
    }
    if (!cleanupMap.contains(cleanupID)) {
        return {};
    }
    const std::function<errors::Error()> cleanup = cleanupMap.at(cleanupID);
    cleanupMap.erase(cleanupID);
    errors::Error error;
    try {
        error = cleanup();
    } catch (const std::exception& exception) {
        error = errors::message(std::string("Cleanup threw a C++ exception: ") + exception.what());
    } catch (...) {
        error = errors::message("Cleanup threw an unknown C++ exception");
    }
    if (JS_HasException(jsContext)) {
        error.errorCode = ErrorCode::JSException;
    }
    return error;
}

errors::Error Scheduler::runNext() {
    if (this_thread::get_id() != ownerThreadID) {
        return errors::message("called runNext() outside owner thread");
    }
    if (clearing) {
        return errors::message("called runNext() inside a clear()");
    }
    if (JS_HasException(jsContext)) {
        return errors::message("cannot run next callback with a pending JavaScript exception");
    }
    std::function<errors::Error()> callback = {};
    {
        std::unique_lock<std::mutex> lock(mutex);
        while (true) {
            if (callbackMap.empty()) {
                return errors::Error{
                    .errorCode = ErrorCode::Stalled,
                    .errorMessage = "No scheduled callback can make progress; invocation is stalled",
                };
            }
            int64_t callbackID = 0;
            std::optional<chrono::steady_clock::time_point> timePoint = {};
            for (const std::pair<const int64_t, std::function<errors::Error()>>& entry : callbackMap) {
                // Check the readyTimeMap.
                if (readyTimeMap.contains(entry.first)) {
                    const chrono::steady_clock::time_point readyTime = readyTimeMap.at(entry.first);
                    if (!timePoint.has_value() || readyTime < timePoint.value()) {
                        callbackID = entry.first;
                        timePoint = readyTime;
                    }
                }
                // Check the triggerTimeMap.
                if (triggerTimeMap.contains(entry.first)) {
                    const chrono::steady_clock::time_point triggerTime = triggerTimeMap.at(entry.first);
                    if (!timePoint.has_value() || triggerTime < timePoint.value()) {
                        callbackID = entry.first;
                        timePoint = triggerTime;
                    }
                }
            }
            if (timePoint.has_value()) {
                if (timePoint.value() <= chrono::steady_clock::now()) {
                    callback = callbackMap.at(callbackID);
                    callbackMap.erase(callbackID);
                    triggerTimeMap.erase(callbackID);
                    readyTimeMap.erase(callbackID);
                    break;
                }
                conditionVariable.wait_until(lock, timePoint.value());
            } else {
                conditionVariable.wait(lock);
            }
        }
    }
    errors::Error error = {};
    try {
        error = callback();
    } catch (const std::exception& exception) {
        error = errors::message(std::string("Callback threw a C++ exception: ") + exception.what());
    } catch (...) {
        error = errors::message("Callback threw an unknown C++ exception");
    }
    if (JS_HasException(jsContext)) {
        error.errorCode = ErrorCode::JSException;
    }
    return error;
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
        pendingCallbacks.swap(callbackMap);
        pendingCleanups.swap(cleanupMap);
        triggerTimeMap.clear();
        readyTimeMap.clear();
    }
    errors::Error firstError;
    std::optional<JSValue> firstJSException;
    // Preserve an exception already pending on entry while allowing cleanups to
    // call QuickJS with no exception pending.
    if (JS_HasException(jsContext)) {
        firstJSException = JS_GetException(jsContext);
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
        if (JS_HasException(jsContext)) {
            const JSValue exception = JS_GetException(jsContext);
            error.errorCode = ErrorCode::JSException;
            if (error.errorMessage.empty()) {
                error.errorMessage = "Cleanup left a pending JavaScript exception";
            }
            if (!firstError.errorCode) {
                firstJSException = exception;
            } else {
                JS_FreeValue(jsContext, exception);
            }
        }
        if (!firstError.errorCode && error.errorCode) {
            firstError = error;
        }
    }
    pendingCallbacks.clear();
    pendingCleanups.clear();
    clearing = false;
    if (firstJSException.has_value()) {
        JS_Throw(jsContext, firstJSException.value());
    }
    return firstError;
}

} // namespace amj::javascript
