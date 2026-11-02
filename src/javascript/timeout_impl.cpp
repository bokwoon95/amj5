#include "timeout_impl.hpp"

#include "errors/errors.hpp"
#include "scheduler.hpp"

#include <quickjs.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace amj::javascript {
namespace chrono = std::chrono;
namespace errors = amj::errors;
namespace {

// JS_MKVAL only constructs scalars; its C declaration lacks noexcept.
// NOLINTBEGIN(bugprone-throwing-static-initialization)
const JSValue undefinedValue = JS_UNDEFINED;
const JSValue nullValue = JS_NULL;
const JSValue exceptionValue = JS_EXCEPTION;
// NOLINTEND(bugprone-throwing-static-initialization)

std::mutex timeoutClassIDMutex = {};
std::map<JSRuntime*, JSClassID> timeoutClassIDMap = {}; // NOLINT(bugprone-throwing-static-initialization): startup allocation failure may terminate.

void releaseTimeoutClassID(JSRuntime* jsRuntime, void* /* argument */) {
    const std::lock_guard<std::mutex> lock(timeoutClassIDMutex);
    timeoutClassIDMap.erase(jsRuntime);
}

struct Timer {
    JSContext* jsContext = nullptr;
    JSValue callback = undefinedValue;
    std::vector<JSValue> arguments = {};
    int64_t callbackID = 0;

    ~Timer() {
        JS_FreeValue(jsContext, callback);
        for (const JSValue argument : arguments) {
            JS_FreeValue(jsContext, argument);
        }
    }
};

struct TimeoutBinding {
    Scheduler* scheduler = nullptr;
    std::map<int64_t, std::shared_ptr<Timer>> timers = {};
    bool active = false;
};

struct TimeoutHolder {
    std::shared_ptr<TimeoutBinding> timeoutBinding = nullptr;
};

void finalizeTimeout(JSRuntime* /* jsRuntime */, JSValue value) {
    const std::unique_ptr<TimeoutHolder> timeoutHolder(static_cast<TimeoutHolder*>(JS_GetOpaque(value, JS_GetClassID(value))));
}

JSValue timeoutOperation(JSContext* jsContext, JSValueConst /* thisValue */, int argc, JSValueConst* argv, int operation, JSValueConst* functionData) {
    const TimeoutHolder* timeoutHolder = static_cast<TimeoutHolder*>(JS_GetOpaque2(jsContext, functionData[0], JS_GetClassID(functionData[0])));
    if (timeoutHolder == nullptr) {
        return exceptionValue;
    }
    const std::shared_ptr<TimeoutBinding> timeoutBinding = timeoutHolder->timeoutBinding;
    if (!timeoutBinding->active) {
        if (operation == 1) {
            return undefinedValue;
        }
        return JS_ThrowInternalError(jsContext, "Timer binding has been cleared");
    }
    try {
        if (operation == 1) {
            if (argc < 1) {
                return undefinedValue;
            }
            double number = 0.0;
            if (JS_ToFloat64(jsContext, &number, argv[0]) < 0) {
                return exceptionValue;
            }
            if (!std::isfinite(number) || number < 1.0 || number > 9007199254740991.0 || std::floor(number) != number) {
                return undefinedValue;
            }
            const int64_t callbackID = static_cast<int64_t>(number);
            if (timeoutBinding->timers.contains(callbackID)) {
                const errors::Error error = timeoutBinding->scheduler->unregisterCallback(callbackID);
                if (error.errorCode) {
                    return JS_ThrowInternalError(jsContext, "%s", error.toString().c_str());
                }
                timeoutBinding->timers.erase(callbackID);
            }
            return undefinedValue;
        }
        if (argc < 1 || !JS_IsFunction(jsContext, argv[0])) {
            return JS_ThrowTypeError(jsContext, "setTimeout requires a function callback");
        }
        if (argc > 258) {
            return JS_ThrowRangeError(jsContext, "Timer argument limit exceeded");
        }
        double delay = 0.0;
        if (argc > 1 && JS_ToFloat64(jsContext, &delay, argv[1]) < 0) {
            return exceptionValue;
        }
        if (std::isnan(delay) || delay < 0.0) {
            delay = 0.0;
        } else if (delay > 2147483647.0) {
            delay = 2147483647.0;
        }
        // Numeric coercion can execute JS, including clearing the scheduler.
        if (!timeoutBinding->active) {
            return JS_ThrowInternalError(jsContext, "Timer binding has been cleared");
        }
        if (timeoutBinding->timers.size() >= 128) {
            return JS_ThrowRangeError(jsContext, "Too many pending timers");
        }
        const std::shared_ptr<Timer> timer = std::make_shared<Timer>();
        timer->jsContext = jsContext;
        timer->callback = JS_DupValue(jsContext, argv[0]);
        if (argc > 2) {
            timer->arguments.reserve(static_cast<std::size_t>(argc - 2));
            for (int index = 2; index < argc; index++) {
                timer->arguments.push_back(JS_DupValue(jsContext, argv[index]));
            }
        }
        const chrono::steady_clock::time_point now = chrono::steady_clock::now();
        const chrono::milliseconds duration(static_cast<int64_t>(delay));
        if (now > chrono::steady_clock::time_point::max() - duration) {
            return JS_ThrowRangeError(jsContext, "Timer deadline is out of range");
        }
        const std::weak_ptr<TimeoutBinding> weakTimeoutBinding = timeoutBinding;
        const std::weak_ptr<Timer> weakTimer = timer;
        const std::expected<int64_t, errors::Error> callbackIDResult = timeoutBinding->scheduler->registerCallback([weakTimeoutBinding, weakTimer]() -> errors::Error {
            const std::shared_ptr<TimeoutBinding> timeoutBinding = weakTimeoutBinding.lock();
            const std::shared_ptr<Timer> timer = weakTimer.lock();
            if (timeoutBinding == nullptr || timer == nullptr || !timeoutBinding->active) {
                return {};
            }
            timeoutBinding->timers.erase(timer->callbackID);
            const JSValue result = JS_Call(timer->jsContext, timer->callback, undefinedValue, static_cast<int>(timer->arguments.size()), timer->arguments.data());
            if (JS_IsException(result)) {
                return errors::Error{
                    .errorCode = ErrorCode::JSException,
                    .errorMessage = "Timer callback threw a JavaScript exception",
                };
            }
            JS_FreeValue(timer->jsContext, result);
            return {};
        },
                                                                                                                   now + duration);
        if (!callbackIDResult.has_value()) {
            return JS_ThrowInternalError(jsContext, "%s", callbackIDResult.error().toString().c_str());
        }
        timer->callbackID = callbackIDResult.value();
        try {
            if (timer->callbackID > 9007199254740991) {
                throw std::runtime_error("Timer IDs exhausted");
            }
            timeoutBinding->timers.insert_or_assign(timer->callbackID, timer);
        } catch (...) {
            const errors::Error error = timeoutBinding->scheduler->unregisterCallback(timer->callbackID);
            if (error.errorCode) {
                std::terminate();
            }
            throw;
        }
        return JS_NewInt64(jsContext, timer->callbackID);
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        if (JS_HasException(jsContext)) {
            return exceptionValue;
        }
        return JS_ThrowInternalError(jsContext, "%s", exception.what());
    } catch (...) {
        return JS_ThrowInternalError(jsContext, "Unexpected timer operation failure");
    }
}

} // namespace

JSValue TimeoutImpl::create(JSContext* jsContext, Scheduler& scheduler) try {
    JSRuntime* jsRuntime = JS_GetRuntime(jsContext);
    JSClassID timeoutClassID = 0;
    {
        const std::lock_guard<std::mutex> lock(timeoutClassIDMutex);
        if (timeoutClassIDMap.contains(jsRuntime)) {
            timeoutClassID = timeoutClassIDMap.at(jsRuntime);
        } else {
            JS_NewClassID(jsRuntime, &timeoutClassID);
            timeoutClassIDMap.insert_or_assign(jsRuntime, timeoutClassID);
            if (JS_AddRuntimeFinalizer(jsRuntime, releaseTimeoutClassID, nullptr) < 0) {
                timeoutClassIDMap.erase(jsRuntime);
                throw std::bad_alloc();
            }
        }
    }
    if (!JS_IsRegisteredClass(jsRuntime, timeoutClassID)) {
        const JSClassDef definition = {
            .class_name = "TimeoutBinding",
            .finalizer = finalizeTimeout,
            .gc_mark = nullptr,
            .call = nullptr,
            .exotic = nullptr,
        };
        if (JS_NewClass(jsRuntime, timeoutClassID, &definition) < 0) {
            return JS_ThrowOutOfMemory(jsContext);
        }
    }
    const std::shared_ptr<TimeoutBinding> timeoutBinding = std::make_shared<TimeoutBinding>();
    timeoutBinding->scheduler = &scheduler;
    std::unique_ptr<TimeoutHolder> timeoutHolder = std::make_unique<TimeoutHolder>();
    timeoutHolder->timeoutBinding = timeoutBinding;
    const JSValue object = JS_NewObjectProtoClass(jsContext, nullValue, timeoutClassID);
    if (JS_IsException(object)) {
        return exceptionValue;
    }
    const auto releaseObject = [jsContext](const JSValue* value) {
        JS_FreeValue(jsContext, *value);
    };
    const std::unique_ptr<const JSValue, decltype(releaseObject)> ownedObject(&object, releaseObject);
    JS_SetOpaque(object, timeoutHolder.release());
    const JSValue target = JS_NewObjectProto(jsContext, nullValue);
    if (JS_IsException(target)) {
        return exceptionValue;
    }
    const std::unique_ptr<const JSValue, decltype(releaseObject)> ownedTarget(&target, releaseObject);
    const std::expected<int64_t, errors::Error> cleanupIDResult = scheduler.registerCleanup([timeoutBinding]() -> errors::Error {
        timeoutBinding->active = false;
        errors::Error firstError;
        for (const std::pair<const int64_t, std::shared_ptr<Timer>>& entry : timeoutBinding->timers) {
            const errors::Error error = timeoutBinding->scheduler->unregisterCallback(entry.first);
            if (!firstError.errorCode && error.errorCode) {
                firstError = error;
            }
        }
        timeoutBinding->timers.clear();
        return firstError;
    });
    if (!cleanupIDResult.has_value()) {
        JS_ThrowInternalError(jsContext, "%s", cleanupIDResult.error().toString().c_str());
        return exceptionValue;
    }
    timeoutBinding->active = true;
    try {
        const std::array<const char*, 2> names = {"setTimeout", "clearTimeout"};
        for (int operation = 0; operation < 2; operation++) {
            JSValue functionData = object;
            const JSValue function = JS_NewCFunctionData(jsContext, timeoutOperation, 2 - operation, operation, 1, &functionData);
            if (JS_IsException(function) || JS_SetPropertyStr(jsContext, target, names.at(static_cast<std::size_t>(operation)), function) < 0) {
                const errors::Error error = scheduler.cleanup(cleanupIDResult.value());
                if (error.errorCode) {
                    throw std::runtime_error(error.toString());
                }
                return exceptionValue;
            }
        }
    } catch (...) {
        const errors::Error error = scheduler.cleanup(cleanupIDResult.value());
        if (error.errorCode) {
            std::terminate();
        }
        throw;
    }
    if (!timeoutBinding->active) {
        return JS_ThrowInternalError(jsContext, "Timer binding was cleared during creation");
    }
    return JS_DupValue(jsContext, target);
} catch (const std::bad_alloc&) {
    return JS_ThrowOutOfMemory(jsContext);
} catch (const std::exception& exception) {
    if (!JS_HasException(jsContext)) {
        JS_ThrowInternalError(jsContext, "%s", exception.what());
    }
    return exceptionValue;
} catch (...) {
    if (!JS_HasException(jsContext)) {
        JS_ThrowInternalError(jsContext, "Unexpected timer creation failure");
    }
    return exceptionValue;
}

JSModuleDef* TimeoutImpl::createModule(JSContext* jsContext, Scheduler& scheduler) {
    const JSValue exports = TimeoutImpl::create(jsContext, scheduler);
    if (JS_IsException(exports)) {
        return nullptr;
    }
    JSModuleDef* module = JS_NewCModule(jsContext, "amj:timeout_impl", [](JSContext* jsContext, JSModuleDef* module) -> int {
        const JSValue exports = JS_GetModulePrivateValue(jsContext, module);
        if (JS_IsException(exports)) {
            return -1;
        }
        const std::array<const char*, 2> names = {"setTimeout", "clearTimeout"};
        for (const char* name : names) {
            const JSValue function = JS_GetPropertyStr(jsContext, exports, name);
            if (JS_IsException(function) || JS_SetModuleExport(jsContext, module, name, function) < 0) {
                JS_FreeValue(jsContext, exports);
                return -1;
            }
        }
        JS_FreeValue(jsContext, exports);
        return 0;
    });
    if (module == nullptr) {
        JS_FreeValue(jsContext, exports);
        return nullptr;
    }
    if (JS_AddModuleExport(jsContext, module, "setTimeout") < 0 || JS_AddModuleExport(jsContext, module, "clearTimeout") < 0) {
        JS_FreeValue(jsContext, exports);
        return nullptr;
    }
    if (JS_SetModulePrivateValue(jsContext, module, exports) < 0) {
        return nullptr;
    }
    return module;
}

} // namespace amj::javascript
