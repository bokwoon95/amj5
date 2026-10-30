#include "callback_scheduler.hpp"

#include <quickjs.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace amj::javascript {
namespace chrono = std::chrono;

struct CallbackScheduler::Impl {
    JSContext* jsContext = nullptr;
    std::thread::id ownerThread = {};
    std::mutex mutex = {};
    std::condition_variable conditionVariable = {};
    // Both maps are protected by mutex. Callbacks and their captures must be
    // executed/destroyed on the owner thread, outside the mutex.
    std::map<int64_t, CallbackRecord> records = {};
    std::map<int64_t, chrono::steady_clock::time_point> notifiedAt = {};
    int64_t lastCallbackRecordID = 0;
    bool cancelling = false;
};

CallbackScheduler::CallbackScheduler(JSContext* jsContext) : impl(std::make_unique<Impl>()) {
    if (jsContext == nullptr) {
        throw std::invalid_argument("CallbackScheduler requires a JS context");
    }
    impl->jsContext = jsContext;
    impl->ownerThread = std::this_thread::get_id();
}

CallbackScheduler::~CallbackScheduler() {
    try {
        cancelAll();
    } catch (...) {
        // Do not let failed teardown silently leave native workers running.
        std::terminate();
    }
}

int64_t CallbackScheduler::schedule(CallbackRecord callbackRecord) {
    if (std::this_thread::get_id() != impl->ownerThread || impl->cancelling) {
        throw std::logic_error("Invalid CallbackScheduler registration");
    }
    if ((!callbackRecord.callback && !callbackRecord.cancel) || (!callbackRecord.callback && callbackRecord.deadline.has_value())) {
        throw std::invalid_argument("Registration requires a callback or cancellation-only work without a deadline");
    }
    const std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->records.size() >= 4096 || impl->lastCallbackRecordID >= 9007199254740991) {
        throw std::length_error("Pending callback limit or callback IDs exhausted");
    }
    const int64_t callbackRecordID = impl->lastCallbackRecordID + 1;
    impl->records.emplace(callbackRecordID, std::move(callbackRecord));
    impl->lastCallbackRecordID = callbackRecordID;
    return callbackRecordID;
}

void CallbackScheduler::unschedule(int64_t callbackRecordID) {
    if (std::this_thread::get_id() != impl->ownerThread) {
        throw std::logic_error("CallbackScheduler accessed from a non-owning thread");
    }
    std::map<int64_t, CallbackRecord>::node_type removed;
    {
        const std::lock_guard<std::mutex> lock(impl->mutex);
        removed = impl->records.extract(callbackRecordID);
        impl->notifiedAt.erase(callbackRecordID);
    }
    // Release callback captures outside the mutex, including for reentrant JS
    // finalizers. An empty node makes unknown IDs a no-op.
}

void CallbackScheduler::notify(int64_t callbackRecordID) {
    {
        const std::lock_guard<std::mutex> lock(impl->mutex);
        const std::map<int64_t, CallbackRecord>::const_iterator record = impl->records.find(callbackRecordID);
        if (record == impl->records.end() || !record->second.callback) {
            return;
        }
        // Repeated notifications coalesce and preserve the first timestamp.
        const bool inserted = impl->notifiedAt.try_emplace(callbackRecordID, chrono::steady_clock::now()).second;
        if (!inserted) {
            return;
        }
    }
    impl->conditionVariable.notify_one();
}

void CallbackScheduler::dispatch() {
    if (std::this_thread::get_id() != impl->ownerThread || impl->cancelling) {
        throw std::logic_error("Invalid CallbackScheduler dispatch");
    }
    // Promise continuations must get a chance to clear timers between tasks.
    if (static_cast<int>(JS_IsJobPending(JS_GetRuntime(impl->jsContext))) != 0) {
        return;
    }
    const chrono::steady_clock::time_point now = chrono::steady_clock::now();
    std::vector<std::pair<chrono::steady_clock::time_point, int64_t>> ready;
    {
        const std::lock_guard<std::mutex> lock(impl->mutex);
        ready.reserve(impl->records.size());
        for (const std::pair<const int64_t, CallbackRecord>& record : impl->records) {
            if (!record.second.callback) {
                continue;
            }
            std::optional<chrono::steady_clock::time_point> readyAtOption;
            const std::map<int64_t, chrono::steady_clock::time_point>::const_iterator notification = impl->notifiedAt.find(record.first);
            if (notification != impl->notifiedAt.end()) {
                readyAtOption = notification->second;
            }
            if (record.second.deadline.has_value() && record.second.deadline.value() <= now &&
                (!readyAtOption.has_value() || record.second.deadline.value() < readyAtOption.value())) {
                readyAtOption = record.second.deadline;
            }
            if (readyAtOption.has_value()) {
                ready.emplace_back(readyAtOption.value(), record.first);
            }
        }
    }
    std::ranges::sort(ready);
    for (const std::pair<chrono::steady_clock::time_point, int64_t>& readyRecord : ready) {
        std::map<int64_t, CallbackRecord>::node_type pending;
        {
            const std::lock_guard<std::mutex> lock(impl->mutex);
            pending = impl->records.extract(readyRecord.second);
            impl->notifiedAt.erase(readyRecord.second);
        }
        if (pending.empty()) {
            continue; // Earlier callbacks may remove other registrations.
        }
        pending.mapped().callback();
        if (static_cast<int>(JS_HasException(impl->jsContext)) != 0) {
            throw std::runtime_error("Callback record left an unhandled JS exception");
        }
        if (static_cast<int>(JS_IsJobPending(JS_GetRuntime(impl->jsContext))) != 0) {
            return;
        }
    }
}

void CallbackScheduler::wait() {
    if (std::this_thread::get_id() != impl->ownerThread || impl->cancelling) {
        throw std::logic_error("Invalid CallbackScheduler wait");
    }
    // Only the owner thread can enqueue JS jobs; workers only call notify().
    if (static_cast<int>(JS_IsJobPending(JS_GetRuntime(impl->jsContext))) != 0) {
        return;
    }
    std::unique_lock<std::mutex> lock(impl->mutex);
    while (true) {
        const chrono::steady_clock::time_point now = chrono::steady_clock::now();
        std::optional<chrono::steady_clock::time_point> nearestDeadlineOption;
        bool canProgress = false;
        for (const std::pair<const int64_t, CallbackRecord>& record : impl->records) {
            if (!record.second.callback) {
                continue;
            }
            canProgress = true;
            if (impl->notifiedAt.contains(record.first)) {
                return;
            }
            if (record.second.deadline.has_value()) {
                if (record.second.deadline.value() <= now) {
                    return;
                }
                if (!nearestDeadlineOption.has_value() || record.second.deadline.value() < nearestDeadlineOption.value()) {
                    nearestDeadlineOption = record.second.deadline;
                }
            }
        }
        if (!canProgress) {
            throw std::logic_error("No pending callback can make progress; invocation is stalled");
        }
        // Atomically release the predicate mutex and sleep, then reacquire it.
        // Recheck manually after notifications, deadlines, and spurious wakeups.
        if (nearestDeadlineOption.has_value()) {
            impl->conditionVariable.wait_until(lock, nearestDeadlineOption.value());
        } else {
            impl->conditionVariable.wait(lock);
        }
    }
}

void CallbackScheduler::cancelAll() {
    if (std::this_thread::get_id() != impl->ownerThread) {
        throw std::logic_error("CallbackScheduler accessed from a non-owning thread");
    }
    if (impl->cancelling) {
        return;
    }
    impl->cancelling = true;
    std::map<int64_t, CallbackRecord> pending;
    {
        const std::lock_guard<std::mutex> lock(impl->mutex);
        pending.swap(impl->records);
        impl->notifiedAt.clear();
    }
    // Late worker notifications now find no record. Do not hold the mutex while
    // stopping/joining workers, since those workers may be inside notify().
    std::exception_ptr failure;
    std::optional<JSValue> firstJSExceptionOption;
    for (const std::pair<const int64_t, CallbackRecord>& record : pending) {
        if (!record.second.cancel) {
            continue;
        }
        try {
            record.second.cancel();
            if (static_cast<int>(JS_HasException(impl->jsContext)) != 0) {
                throw std::runtime_error("Cancellation hook left an unhandled JS exception");
            }
        } catch (...) {
            const bool firstFailure = failure == nullptr;
            if (firstFailure) {
                failure = std::current_exception();
            }
            // Clear JS exceptions so the remaining cancellation hooks can run.
            if (static_cast<int>(JS_HasException(impl->jsContext)) != 0) {
                const JSValue exception = JS_GetException(impl->jsContext);
                if (firstFailure) {
                    firstJSExceptionOption = exception;
                } else {
                    JS_FreeValue(impl->jsContext, exception);
                }
            }
        }
    }
    pending.clear();
    impl->cancelling = false;
    if (failure != nullptr) {
        if (firstJSExceptionOption.has_value()) {
            JS_Throw(impl->jsContext, firstJSExceptionOption.value());
        }
        std::rethrow_exception(failure);
    }
}

} // namespace amj::javascript
