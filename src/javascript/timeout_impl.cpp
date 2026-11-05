#include "timeout_impl.hpp"

#include "errors/errors.hpp"
#include "javascript.hpp"
#include "scheduler.hpp"

#include <quickjs.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>

namespace amj::javascript {
namespace chrono = std::chrono;
namespace errors = amj::errors;
namespace {

std::mutex timeoutClassIDMutex = {};
std::map<JSRuntime*, JSClassID> timeoutClassIDMap = {}; // NOLINT(bugprone-throwing-static-initialization): startup allocation failure may terminate.

void releaseTimeoutClassID(JSRuntime* jsRuntime, void* /* argument */) {
    const std::lock_guard<std::mutex> lock(timeoutClassIDMutex);
    timeoutClassIDMap.erase(jsRuntime);
}

// Owned by the scheduler callback. Destruction handles cancellation and clear()
// without reentering the scheduler.
struct TimeoutCallback {
    JSContext* jsContext = nullptr;
    JSValue callback = JS_UNDEFINED;
    std::weak_ptr<Timeout> weakTimeout = {};
    int32_t timerID = 0;
    int64_t callbackID = 0;

    ~TimeoutCallback() {
        // A timer ID may have been reused while this callback was executing.
        const std::shared_ptr<Timeout> timeout = weakTimeout.lock();
        if (timeout != nullptr && timeout->timerCallbackIDs.contains(timerID) && timeout->timerCallbackIDs.at(timerID) == callbackID) {
            timeout->timerCallbackIDs.erase(timerID);
        }
        JS_FreeValue(jsContext, callback);
    }

    errors::Error call() {
        const std::shared_ptr<Timeout> timeout = weakTimeout.lock();
        if (timeout == nullptr) {
            return {};
        }
        if (timeout->timerCallbackIDs.contains(timerID) && timeout->timerCallbackIDs.at(timerID) == callbackID) {
            timeout->timerCallbackIDs.erase(timerID);
        }
        const ScopedJSValue result(jsContext, JS_Call(jsContext, callback, JS_UNDEFINED, 0, nullptr));
        if (JS_IsException(result.value)) {
            return errors::Error{
                .errorCode = ErrorCode::JSException,
                .errorMessage = "Timer callback threw a JavaScript exception",
            };
        }
        return {};
    }
};

void finalizeTimeout(JSRuntime* /* jsRuntime */, JSValue value) {
    // QuickJS owns this heap-stored smart pointer, not an untracked Timeout*.
    const std::unique_ptr<std::shared_ptr<Timeout>> timeout(static_cast<std::shared_ptr<Timeout>*>(JS_GetOpaque(value, JS_GetClassID(value))));
}

} // namespace

JSValue timeoutSet(JSContext* jsContext, JSValueConst /* thisValue */, int argc, JSValueConst* argv, std::shared_ptr<Timeout> timeout) {
    try {
        if (timeout == nullptr) {
            return JS_ThrowInternalError(jsContext, "timeoutSet: missing timeout");
        }
        if (timeout->scheduler == nullptr) {
            return JS_ThrowInternalError(jsContext, "timeoutSet: missing scheduler");
        }
        if (argc < 2) {
            return JS_ThrowTypeError(jsContext, "timeoutSet: requires at least 2 arguments (%d provided)", argc);
        }

        // Get callback function.
        if (!JS_IsFunction(jsContext, argv[0])) {
            return JS_ThrowTypeError(jsContext, "timeoutSet: arg0: callback not a function");
        }
        const JSValue callback = argv[0];

        // Get delayMs number.
        if (!JS_IsNumber(argv[1])) {
            return JS_ThrowTypeError(jsContext, "timeoutSet: arg1: delay not a number");
        }
        double delayMs = 0.0;
        if (JS_ToFloat64(jsContext, &delayMs, argv[1]) < 0) {
            return JS_EXCEPTION;
        }
        if (!std::isfinite(delayMs) || std::floor(delayMs) != delayMs || delayMs < 0.0 || delayMs > 2147483647.0) {
            return JS_ThrowRangeError(jsContext, "timeoutSet: arg1: delay must be an integer in [0, 2147483647]");
        }

        const chrono::steady_clock::time_point now = chrono::steady_clock::now();
        const chrono::milliseconds delayDuration(static_cast<int64_t>(delayMs));
        if (now > chrono::steady_clock::time_point::max() - delayDuration) {
            return JS_ThrowRangeError(jsContext, "timeoutSet: timer deadline is out of range");
        }
        if (timeout->timerCallbackIDs.size() >= static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
            return JS_ThrowRangeError(jsContext, "timeoutSet: timerIDs exhausted");
        }
        while (true) {
            if (timeout->timerIDCounter == std::numeric_limits<int32_t>::max()) {
                timeout->timerIDCounter = 1;
            } else {
                timeout->timerIDCounter++;
            }
            if (timeout->timerCallbackIDs.contains(timeout->timerIDCounter)) {
                continue;
            }
            break;
        }
        const std::shared_ptr<TimeoutCallback> timeoutCallback = std::make_shared<TimeoutCallback>();
        timeoutCallback->jsContext = jsContext;
        timeoutCallback->callback = JS_DupValue(jsContext, callback);
        timeoutCallback->weakTimeout = timeout;
        timeoutCallback->timerID = timeout->timerIDCounter;
        const auto schedulerCallback = [timeoutCallback]() -> errors::Error {
            return timeoutCallback->call();
        };
        const std::expected<int64_t, errors::Error> callbackID = timeout->scheduler->registerCallback(schedulerCallback, now + delayDuration);
        if (!callbackID.has_value()) {
            return JS_ThrowInternalError(jsContext, "timeoutSet: %s", callbackID.error().toString().c_str());
        }
        timeoutCallback->callbackID = callbackID.value();
        try {
            timeout->timerCallbackIDs.insert_or_assign(timeoutCallback->timerID, timeoutCallback->callbackID);
        } catch (...) {
            const errors::Error error = timeout->scheduler->unregisterCallback(timeoutCallback->callbackID);
            if (error.errorCode) {
                std::terminate();
            }
            throw;
        }
        return JS_NewInt32(jsContext, timeoutCallback->timerID);
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        if (JS_HasException(jsContext)) {
            return JS_EXCEPTION;
        }
        return JS_ThrowInternalError(jsContext, "timeoutSet: %s", exception.what());
    } catch (...) {
        return JS_ThrowInternalError(jsContext, "timeoutSet: unexpected native failure");
    }
}

JSValue timeoutClear(JSContext* jsContext, JSValueConst /* thisValue */, int argc, JSValueConst* argv, std::shared_ptr<Timeout> timeout) {
    try {
        if (timeout == nullptr) {
            return JS_ThrowInternalError(jsContext, "timeoutClear: missing timeout");
        }
        if (argc < 1) {
            return JS_ThrowTypeError(jsContext, "timeoutClear: requires 1 argument (%d provided)", argc);
        }
        if (!JS_IsNumber(argv[0])) {
            return JS_ThrowTypeError(jsContext, "timeoutClear: arg0: timerID not a number");
        }
        double number = 0.0;
        if (JS_ToFloat64(jsContext, &number, argv[0]) < 0) {
            return JS_EXCEPTION;
        }
        if (!std::isfinite(number) || std::floor(number) != number || number < 0.0 || number > 9007199254740991.0) {
            return JS_ThrowRangeError(jsContext, "timeoutClear: arg0: timerID must be a nonnegative safe integer");
        }
        if (timeout->scheduler == nullptr) {
            return JS_ThrowInternalError(jsContext, "timeoutClear: missing scheduler");
        }
        if (number < 1.0 || number > static_cast<double>(std::numeric_limits<int32_t>::max())) {
            return JS_UNDEFINED;
        }
        const int32_t timerID = static_cast<int32_t>(number);
        if (timeout->timerCallbackIDs.contains(timerID)) {
            const int64_t callbackID = timeout->timerCallbackIDs.at(timerID);
            const errors::Error error = timeout->scheduler->unregisterCallback(callbackID);
            if (error.errorCode) {
                return JS_ThrowInternalError(jsContext, "timeoutClear: %s", error.toString().c_str());
            }
            if (timeout->timerCallbackIDs.contains(timerID) && timeout->timerCallbackIDs.at(timerID) == callbackID) {
                timeout->timerCallbackIDs.erase(timerID);
            }
        }
        return JS_UNDEFINED;
    } catch (const std::bad_alloc&) {
        return JS_ThrowOutOfMemory(jsContext);
    } catch (const std::exception& exception) {
        if (JS_HasException(jsContext)) {
            return JS_EXCEPTION;
        }
        return JS_ThrowInternalError(jsContext, "timeoutClear: %s", exception.what());
    } catch (...) {
        return JS_ThrowInternalError(jsContext, "timeoutClear: unexpected native failure");
    }
}

JSModuleDef* createTimeoutImplModule(JSContext* jsContext, std::shared_ptr<Timeout> timeout) {
    try {
        if (timeout == nullptr) {
            JS_ThrowInternalError(jsContext, "createTimeoutImplModule: missing timeout");
            return nullptr;
        }
        if (timeout->scheduler == nullptr) {
            JS_ThrowInternalError(jsContext, "createTimeoutImplModule: missing scheduler");
            return nullptr;
        }
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
                .class_name = "Timeout",
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
        const ScopedJSValue object(jsContext, JS_NewObjectProtoClass(jsContext, JS_NULL, timeoutClassID));
        if (JS_IsException(object.value)) {
            return nullptr;
        }
        std::unique_ptr<std::shared_ptr<Timeout>> ownedTimeout = std::make_unique<std::shared_ptr<Timeout>>(timeout);
        JS_SetOpaque(object.value, ownedTimeout.release());
        JSModuleDef* jsModuleDef = JS_NewCModule(jsContext, "amj:timeout_impl", [](JSContext* jsContext, JSModuleDef* jsModuleDef) -> int {
            const ScopedJSValue object(jsContext, JS_GetModulePrivateValue(jsContext, jsModuleDef));
            if (JS_IsException(object.value)) {
                return -1;
            }
            JSValue functionData = object.value;
            const JSValue setFunction = JS_NewCFunctionData(jsContext, [](JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv, int /* magic */, JSValueConst* functionData) -> JSValue {
                // QuickJS supplies one function-data value, retained by the function.
                const JSValueConst holder = functionData[0];
                const std::shared_ptr<Timeout>* timeout = static_cast<std::shared_ptr<Timeout>*>(JS_GetOpaque2(jsContext, holder, JS_GetClassID(holder)));
                if (timeout == nullptr) {
                    return JS_EXCEPTION;
                }
                return timeoutSet(jsContext, thisValue, argc, argv, *timeout);
            },
                                                            2, 0, 1, &functionData);
            if (JS_IsException(setFunction) || JS_SetModuleExport(jsContext, jsModuleDef, "timeoutSet", setFunction) < 0) {
                return -1;
            }
            const JSValue clearFunction = JS_NewCFunctionData(jsContext, [](JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv, int /* magic */, JSValueConst* functionData) -> JSValue {
                const JSValueConst holder = functionData[0];
                const std::shared_ptr<Timeout>* timeout = static_cast<std::shared_ptr<Timeout>*>(JS_GetOpaque2(jsContext, holder, JS_GetClassID(holder)));
                if (timeout == nullptr) {
                    return JS_EXCEPTION;
                }
                return timeoutClear(jsContext, thisValue, argc, argv, *timeout);
            },
                                                              1, 0, 1, &functionData);
            if (JS_IsException(clearFunction) || JS_SetModuleExport(jsContext, jsModuleDef, "timeoutClear", clearFunction) < 0) {
                return -1;
            }
            return 0;
        });
        if (jsModuleDef == nullptr || JS_AddModuleExport(jsContext, jsModuleDef, "timeoutSet") < 0 || JS_AddModuleExport(jsContext, jsModuleDef, "timeoutClear") < 0) {
            return nullptr;
        }
        if (JS_SetModulePrivateValue(jsContext, jsModuleDef, JS_DupValue(jsContext, object.value)) < 0) {
            return nullptr;
        }
        return jsModuleDef;
    } catch (const std::bad_alloc&) {
        JS_ThrowOutOfMemory(jsContext);
        return nullptr;
    } catch (const std::exception& exception) {
        if (!JS_HasException(jsContext)) {
            JS_ThrowInternalError(jsContext, "createTimeoutImplModule: %s", exception.what());
        }
        return nullptr;
    } catch (...) {
        if (!JS_HasException(jsContext)) {
            JS_ThrowInternalError(jsContext, "createTimeoutImplModule: unexpected native failure");
        }
        return nullptr;
    }
}

} // namespace amj::javascript
