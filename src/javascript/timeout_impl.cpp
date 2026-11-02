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
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <utility>

namespace amj::javascript {
namespace chrono = std::chrono;
namespace errors = amj::errors;
namespace {

// NOLINTBEGIN(bugprone-throwing-static-initialization): JS_MKVAL only constructs scalars.
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
    int64_t callbackID = 0;

    ~Timer() {
        JS_FreeValue(jsContext, callback);
    }
};

} // namespace

struct TimeoutImplState {
    JSContext* jsContext = nullptr;
    Scheduler* scheduler = nullptr;
    std::map<int64_t, std::shared_ptr<Timer>> timers = {};
    bool active = false;
};

namespace {

struct TimeoutHolder {
    std::unique_ptr<TimeoutImpl> timeoutImpl = nullptr;
};

void finalizeTimeout(JSRuntime* /* jsRuntime */, JSValue value) {
    const std::unique_ptr<TimeoutHolder> timeoutHolder(static_cast<TimeoutHolder*>(JS_GetOpaque(value, JS_GetClassID(value))));
}

JSValue timeoutOperation(JSContext* jsContext, JSValueConst /* thisValue */, int argc, JSValueConst* argv, int operation, JSValueConst* functionData) {
    const TimeoutHolder* timeoutHolder = static_cast<TimeoutHolder*>(JS_GetOpaque2(jsContext, functionData[0], JS_GetClassID(functionData[0])));
    if (timeoutHolder == nullptr) {
        return exceptionValue;
    }
    if (timeoutHolder->timeoutImpl == nullptr) {
        return JS_ThrowInternalError(jsContext, "Missing TimeoutImpl instance");
    }
    int numberIndex = 0;
    if (operation == 0) {
        if (argc < 2 || !JS_IsFunction(jsContext, argv[0])) {
            return JS_ThrowTypeError(jsContext, "TimeoutImpl.setTimeout requires a callback and delay");
        }
        numberIndex = 1;
    } else if (argc < 1) {
        return JS_ThrowTypeError(jsContext, "TimeoutImpl.clearTimeout requires a timer ID");
    }
    if (!JS_IsNumber(argv[numberIndex])) {
        return JS_ThrowTypeError(jsContext, "TimeoutImpl requires normalized numeric arguments");
    }
    double number = 0.0;
    if (JS_ToFloat64(jsContext, &number, argv[numberIndex]) < 0) {
        return exceptionValue;
    }
    if (!std::isfinite(number) || std::floor(number) != number || number < 0.0 || number > 9007199254740991.0) {
        return JS_ThrowRangeError(jsContext, "TimeoutImpl requires a nonnegative safe integer");
    }
    if (operation == 0) {
        return timeoutHolder->timeoutImpl->setTimeout(jsContext, argv[0], static_cast<int64_t>(number));
    }
    return timeoutHolder->timeoutImpl->clearTimeout(jsContext, static_cast<int64_t>(number));
}

} // namespace

JSValue TimeoutImpl::setTimeout(JSContext* jsContext, JSValueConst callback, int64_t delay) try {
    if (timeoutImplState == nullptr || !timeoutImplState->active || timeoutImplState->jsContext != jsContext) {
        return JS_ThrowInternalError(jsContext, "TimeoutImpl is inactive or belongs to another context");
    }
    if (!JS_IsFunction(jsContext, callback)) {
        return JS_ThrowTypeError(jsContext, "TimeoutImpl.setTimeout requires a function callback");
    }
    if (delay < 0 || delay > 2147483647) {
        return JS_ThrowRangeError(jsContext, "Timer delay is outside the normalized range");
    }
    if (timeoutImplState->timers.size() >= 128) {
        return JS_ThrowRangeError(jsContext, "Too many pending timers");
    }
    const chrono::steady_clock::time_point now = chrono::steady_clock::now();
    const chrono::milliseconds duration(delay);
    if (now > chrono::steady_clock::time_point::max() - duration) {
        return JS_ThrowRangeError(jsContext, "Timer deadline is out of range");
    }
    const std::shared_ptr<Timer> timer = std::make_shared<Timer>();
    timer->jsContext = jsContext;
    timer->callback = JS_DupValue(jsContext, callback);
    const std::weak_ptr<TimeoutImplState> weakTimeoutImplState = timeoutImplState;
    const std::weak_ptr<Timer> weakTimer = timer;
    const std::expected<int64_t, errors::Error> callbackIDResult = timeoutImplState->scheduler->registerCallback([weakTimeoutImplState, weakTimer]() -> errors::Error {
        const std::shared_ptr<TimeoutImplState> timeoutImplState = weakTimeoutImplState.lock();
        const std::shared_ptr<Timer> timer = weakTimer.lock();
        if (timeoutImplState == nullptr || timer == nullptr || !timeoutImplState->active) {
            return {};
        }
        timeoutImplState->timers.erase(timer->callbackID);
        const JSValue result = JS_Call(timer->jsContext, timer->callback, undefinedValue, 0, nullptr);
        if (JS_IsException(result)) {
            return errors::Error{
                .errorCode = ErrorCode::JSException,
                .errorMessage = "Timer callback threw a JavaScript exception",
            };
        }
        JS_FreeValue(timer->jsContext, result);
        return {};
    }, now + duration);
    if (!callbackIDResult.has_value()) {
        return JS_ThrowInternalError(jsContext, "%s", callbackIDResult.error().toString().c_str());
    }
    timer->callbackID = callbackIDResult.value();
    try {
        if (timer->callbackID > 9007199254740991) {
            throw std::runtime_error("Timer IDs exhausted");
        }
        timeoutImplState->timers.insert_or_assign(timer->callbackID, timer);
    } catch (...) {
        const errors::Error error = timeoutImplState->scheduler->unregisterCallback(timer->callbackID);
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
    return JS_ThrowInternalError(jsContext, "Unexpected timer scheduling failure");
}

JSValue TimeoutImpl::clearTimeout(JSContext* jsContext, int64_t timerID) try {
    if (timeoutImplState == nullptr || timeoutImplState->jsContext != jsContext) {
        return JS_ThrowTypeError(jsContext, "TimeoutImpl belongs to another context");
    }
    if (!timeoutImplState->active) {
        return undefinedValue;
    }
    if (timeoutImplState->timers.contains(timerID)) {
        const errors::Error error = timeoutImplState->scheduler->unregisterCallback(timerID);
        if (error.errorCode) {
            return JS_ThrowInternalError(jsContext, "%s", error.toString().c_str());
        }
        timeoutImplState->timers.erase(timerID);
    }
    return undefinedValue;
} catch (const std::bad_alloc&) {
    return JS_ThrowOutOfMemory(jsContext);
} catch (const std::exception& exception) {
    if (JS_HasException(jsContext)) {
        return exceptionValue;
    }
    return JS_ThrowInternalError(jsContext, "%s", exception.what());
} catch (...) {
    return JS_ThrowInternalError(jsContext, "Unexpected timer cancellation failure");
}

JSModuleDef* TimeoutImpl::createModule(JSContext* jsContext, Scheduler& scheduler) try {
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
            .class_name = "TimeoutImpl",
            .finalizer = finalizeTimeout,
            .gc_mark = nullptr,
            .call = nullptr,
            .exotic = nullptr,
        };
        if (JS_NewClass(jsRuntime, timeoutClassID, &definition) < 0) {
            JS_ThrowOutOfMemory(jsContext);
            return nullptr;
        }
    }
    const std::shared_ptr<TimeoutImplState> timeoutImplState = std::make_shared<TimeoutImplState>();
    timeoutImplState->jsContext = jsContext;
    timeoutImplState->scheduler = &scheduler;
    std::unique_ptr<TimeoutHolder> timeoutHolder = std::make_unique<TimeoutHolder>();
    timeoutHolder->timeoutImpl = std::make_unique<TimeoutImpl>();
    timeoutHolder->timeoutImpl->timeoutImplState = timeoutImplState;
    const JSValue object = JS_NewObjectProtoClass(jsContext, nullValue, timeoutClassID);
    if (JS_IsException(object)) {
        return nullptr;
    }
    const auto releaseObject = [jsContext](const JSValue* value) {
        JS_FreeValue(jsContext, *value);
    };
    const std::unique_ptr<const JSValue, decltype(releaseObject)> ownedObject(&object, releaseObject);
    JS_SetOpaque(object, timeoutHolder.release());
    const std::array<const char*, 2> names = {"setTimeout", "clearTimeout"};
    for (int operation = 0; operation < 2; operation++) {
        JSValue functionData = object;
        const JSValue function = JS_NewCFunctionData(jsContext, timeoutOperation, 2 - operation, operation, 1, &functionData);
        if (JS_IsException(function) || JS_DefinePropertyValueStr(jsContext, object, names.at(static_cast<std::size_t>(operation)), function, JS_PROP_C_W_E) < 0) {
            return nullptr;
        }
    }
    const std::expected<int64_t, errors::Error> cleanupIDResult = scheduler.registerCleanup([timeoutImplState]() -> errors::Error {
        timeoutImplState->active = false;
        errors::Error firstError;
        for (const std::pair<const int64_t, std::shared_ptr<Timer>>& entry : timeoutImplState->timers) {
            const errors::Error error = timeoutImplState->scheduler->unregisterCallback(entry.first);
            if (!firstError.errorCode && error.errorCode) {
                firstError = error;
            }
        }
        timeoutImplState->timers.clear();
        return firstError;
    });
    if (!cleanupIDResult.has_value()) {
        JS_ThrowInternalError(jsContext, "%s", cleanupIDResult.error().toString().c_str());
        return nullptr;
    }
    timeoutImplState->active = true;
    JSModuleDef* module = JS_NewCModule(jsContext, "amj:timeout_impl", [](JSContext* jsContext, JSModuleDef* module) -> int {
        const JSValue timeoutImpl = JS_GetModulePrivateValue(jsContext, module);
        if (JS_IsException(timeoutImpl)) {
            return -1;
        }
        return JS_SetModuleExport(jsContext, module, "timeoutImpl", timeoutImpl);
    });
    if (module == nullptr || JS_AddModuleExport(jsContext, module, "timeoutImpl") < 0) {
        return nullptr;
    }
    if (JS_SetModulePrivateValue(jsContext, module, JS_DupValue(jsContext, object)) < 0) {
        return nullptr;
    }
    return module;
} catch (const std::bad_alloc&) {
    JS_ThrowOutOfMemory(jsContext);
    return nullptr;
} catch (const std::exception& exception) {
    if (!JS_HasException(jsContext)) {
        JS_ThrowInternalError(jsContext, "%s", exception.what());
    }
    return nullptr;
} catch (...) {
    if (!JS_HasException(jsContext)) {
        JS_ThrowInternalError(jsContext, "Unexpected TimeoutImpl module creation failure");
    }
    return nullptr;
}

} // namespace amj::javascript
