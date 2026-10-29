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

struct Readiness::Wakeup {
    std::mutex mutex = {};
    std::condition_variable condition = {};
};

struct CallbackScheduler::Impl {
    JSContext* jsContext = nullptr;
    std::thread::id ownerThread = {};
    std::shared_ptr<Readiness::Wakeup> wakeup = std::make_shared<Readiness::Wakeup>();
    std::map<int64_t, ScheduledCallback> entries = {};
    int64_t lastCallbackID = 0;
    bool cancelling = false;
};

Readiness::Readiness(std::shared_ptr<Wakeup> wakeup) : wakeup(std::move(wakeup)), active(true) {
    if (this->wakeup == nullptr) {
        throw std::invalid_argument("Readiness requires scheduler wakeup state");
    }
}

void Readiness::notify() {
    {
        const std::lock_guard<std::mutex> lock(wakeup->mutex);
        if (!active || notifiedAt.has_value()) {
            return;
        }
        notifiedAt = chrono::steady_clock::now();
    }
    wakeup->condition.notify_one();
}

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

std::shared_ptr<Readiness> CallbackScheduler::makeSignal() {
    if (std::this_thread::get_id() != impl->ownerThread || impl->cancelling) {
        throw std::logic_error("Invalid CallbackScheduler signal creation");
    }
    return std::make_shared<Readiness>(impl->wakeup);
}

int64_t CallbackScheduler::schedule(ScheduledCallback scheduledCallback) {
    if (std::this_thread::get_id() != impl->ownerThread || impl->cancelling) {
        throw std::logic_error("Invalid CallbackScheduler registration");
    }
    if ((!scheduledCallback.callback && !scheduledCallback.cancel) ||
        (!scheduledCallback.callback && (scheduledCallback.deadline.has_value() || scheduledCallback.readySignal != nullptr)) ||
        (scheduledCallback.callback && !scheduledCallback.deadline.has_value() && scheduledCallback.readySignal == nullptr)) {
        throw std::invalid_argument("Registration requires runnable work with a deadline/signal, or cancellation-only work");
    }
    if (impl->entries.size() >= 4096 || impl->lastCallbackID >= 9007199254740991) {
        throw std::length_error("Pending callback limit or callback IDs exhausted");
    }
    const std::shared_ptr<Readiness> readySignal = scheduledCallback.readySignal;
    if (readySignal != nullptr) {
        if (readySignal->wakeup != impl->wakeup) {
            throw std::invalid_argument("Readiness belongs to a different scheduler");
        }
        const std::lock_guard<std::mutex> lock(impl->wakeup->mutex);
        if (readySignal->used) {
            throw std::invalid_argument("Readiness cannot be reused");
        }
    }
    const int64_t scheduledCallbackID = impl->lastCallbackID + 1;
    impl->entries.emplace(scheduledCallbackID, std::move(scheduledCallback));
    impl->lastCallbackID = scheduledCallbackID;
    if (readySignal != nullptr) {
        const std::lock_guard<std::mutex> lock(impl->wakeup->mutex);
        readySignal->used = true;
    }
    return scheduledCallbackID;
}

void CallbackScheduler::unschedule(int64_t scheduledCallbackID) {
    if (std::this_thread::get_id() != impl->ownerThread) {
        throw std::logic_error("CallbackScheduler accessed from a non-owning thread");
    }
    const std::map<int64_t, ScheduledCallback>::iterator entry = impl->entries.find(scheduledCallbackID);
    if (entry == impl->entries.end()) {
        return;
    }
    if (entry->second.readySignal != nullptr) {
        const std::lock_guard<std::mutex> lock(impl->wakeup->mutex);
        entry->second.readySignal->active = false;
    }
    // Destroy retained JS values outside the native mutex, on the owner thread.
    const ScheduledCallback removed = std::move(entry->second);
    impl->entries.erase(entry);
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
    ready.reserve(impl->entries.size());
    {
        const std::lock_guard<std::mutex> lock(impl->wakeup->mutex);
        for (const std::pair<const int64_t, ScheduledCallback>& entry : impl->entries) {
            if (!entry.second.callback) {
                continue;
            }
            std::optional<chrono::steady_clock::time_point> readyAt;
            if (entry.second.readySignal != nullptr) {
                readyAt = entry.second.readySignal->notifiedAt;
            }
            if (entry.second.deadline.has_value() && entry.second.deadline.value() <= now &&
                (!readyAt.has_value() || entry.second.deadline.value() < readyAt.value())) {
                readyAt = entry.second.deadline;
            }
            if (readyAt.has_value()) {
                ready.emplace_back(readyAt.value(), entry.first);
            }
        }
    }
    std::ranges::sort(ready);
    for (const std::pair<chrono::steady_clock::time_point, int64_t>& readyEntry : ready) {
        const std::map<int64_t, ScheduledCallback>::iterator entry = impl->entries.find(readyEntry.second);
        if (entry == impl->entries.end()) {
            continue; // Earlier callbacks may remove other registrations.
        }
        const ScheduledCallback pending = std::move(entry->second);
        if (pending.readySignal != nullptr) {
            const std::lock_guard<std::mutex> lock(impl->wakeup->mutex);
            pending.readySignal->active = false;
        }
        impl->entries.erase(entry);
        pending.callback();
        if (static_cast<int>(JS_HasException(impl->jsContext)) != 0) {
            throw std::runtime_error("Scheduled callback left an unhandled JS exception");
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
    if (static_cast<int>(JS_IsJobPending(JS_GetRuntime(impl->jsContext))) != 0) {
        return;
    }
    std::unique_lock<std::mutex> lock(impl->wakeup->mutex);
    while (true) {
        const chrono::steady_clock::time_point now = chrono::steady_clock::now();
        std::optional<chrono::steady_clock::time_point> nearest;
        bool canProgress = false;
        for (const std::pair<const int64_t, ScheduledCallback>& entry : impl->entries) {
            if (!entry.second.callback) {
                continue;
            }
            canProgress = true;
            if (entry.second.readySignal != nullptr && entry.second.readySignal->notifiedAt.has_value()) {
                return;
            }
            if (entry.second.deadline.has_value()) {
                if (entry.second.deadline.value() <= now) {
                    return;
                }
                if (!nearest.has_value() || entry.second.deadline.value() < nearest.value()) {
                    nearest = entry.second.deadline;
                }
            }
        }
        if (!canProgress) {
            throw std::logic_error("No pending callback can make progress; invocation is stalled");
        }
        // Checking readiness and entering the wait share the notification mutex.
        if (nearest.has_value()) {
            impl->wakeup->condition.wait_until(lock, nearest.value());
        } else {
            impl->wakeup->condition.wait(lock);
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
    std::map<int64_t, ScheduledCallback> pending;
    pending.swap(impl->entries);
    {
        const std::lock_guard<std::mutex> lock(impl->wakeup->mutex);
        for (const std::pair<const int64_t, ScheduledCallback>& entry : pending) {
            if (entry.second.readySignal != nullptr) {
                entry.second.readySignal->active = false;
            }
        }
    }
    std::exception_ptr failure;
    std::optional<JSValue> firstJSException;
    for (const std::pair<const int64_t, ScheduledCallback>& entry : pending) {
        if (!entry.second.cancel) {
            continue;
        }
        try {
            entry.second.cancel();
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
                    firstJSException = exception;
                } else {
                    JS_FreeValue(impl->jsContext, exception);
                }
            }
        }
    }
    pending.clear();
    impl->cancelling = false;
    if (failure != nullptr) {
        if (firstJSException.has_value()) {
            JS_Throw(impl->jsContext, firstJSException.value());
        }
        std::rethrow_exception(failure);
    }
}

} // namespace amj::javascript
