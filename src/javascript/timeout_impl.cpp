#include "timeout_impl.hpp"

#include "errors/errors.hpp"
#include "javascript.hpp"
#include "scheduler.hpp"

#include <quickjs.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <expected>
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
// without reentering the scheduler, including when it holds its mutex.
struct Timer {
    JSContext* jsContext = nullptr;
    JSValue callback = JS_UNDEFINED;
    TimeoutState* timeoutState = nullptr;
    int64_t callbackID = 0;

    ~Timer() {
        if (timeoutState != nullptr && callbackID != 0) {
            timeoutState->timerIDs.erase(callbackID);
        }
        JS_FreeValue(jsContext, callback);
    }
};

} // namespace

JSValue timeoutSet(JSContext* jsContext, JSValueConst /* thisValue */, int argc, JSValueConst* argv, TimeoutState& timeoutState) {
    try {
        if (argc < 2) {
            return JS_ThrowTypeError(jsContext, "timeoutSet: requires 2 arguments (%d provided)", argc);
        }
        if (!JS_IsFunction(jsContext, argv[0])) {
            return JS_ThrowTypeError(jsContext, "timeoutSet: arg0: callback not a function");
        }
        if (!JS_IsNumber(argv[1])) {
            return JS_ThrowTypeError(jsContext, "timeoutSet: arg1: delay not a number");
        }
        double number = 0.0;
        if (JS_ToFloat64(jsContext, &number, argv[1]) < 0) {
            return JS_EXCEPTION;
        }
        if (!std::isfinite(number) || std::floor(number) != number || number < 0.0 || number > 2147483647.0) {
            return JS_ThrowRangeError(jsContext, "timeoutSet: arg1: delay must be an integer in [0, 2147483647]");
        }
        if (timeoutState.scheduler == nullptr) {
            return JS_ThrowInternalError(jsContext, "timeoutSet: missing scheduler");
        }
        if (timeoutState.timerIDs.size() >= 65535) {
            return JS_ThrowRangeError(jsContext, "timeoutSet: too many pending timers");
        }
        const chrono::steady_clock::time_point now = chrono::steady_clock::now();
        const chrono::milliseconds duration(static_cast<int64_t>(number));
        if (now > chrono::steady_clock::time_point::max() - duration) {
            return JS_ThrowRangeError(jsContext, "timeoutSet: timer deadline is out of range");
        }
        const std::shared_ptr<Timer> timer = std::make_shared<Timer>();
        timer->jsContext = jsContext;
        timer->callback = JS_DupValue(jsContext, argv[0]);
        timer->timeoutState = &timeoutState;
        const std::expected<int64_t, errors::Error> callbackIDResult = timeoutState.scheduler->registerCallback([timer]() -> errors::Error {
            timer->timeoutState->timerIDs.erase(timer->callbackID);
            const ScopedJSValue result(timer->jsContext, JS_Call(timer->jsContext, timer->callback, JS_UNDEFINED, 0, nullptr));
            if (JS_IsException(result.value)) {
                return errors::Error{
                    .errorCode = ErrorCode::JSException,
                    .errorMessage = "Timer callback threw a JavaScript exception",
                };
            }
            return {};
        }, now + duration);
        if (!callbackIDResult.has_value()) {
            return JS_ThrowInternalError(jsContext, "timeoutSet: %s", callbackIDResult.error().toString().c_str());
        }
        timer->callbackID = callbackIDResult.value();
        try {
            if (timer->callbackID > 9007199254740991) {
                const errors::Error error = timeoutState.scheduler->unregisterCallback(timer->callbackID);
                if (error.errorCode) {
                    std::terminate();
                }
                return JS_ThrowRangeError(jsContext, "timeoutSet: timer IDs exhausted");
            }
            timeoutState.timerIDs.insert(timer->callbackID);
        } catch (...) {
            const errors::Error error = timeoutState.scheduler->unregisterCallback(timer->callbackID);
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
            return JS_EXCEPTION;
        }
        return JS_ThrowInternalError(jsContext, "timeoutSet: %s", exception.what());
    } catch (...) {
        return JS_ThrowInternalError(jsContext, "timeoutSet: unexpected native failure");
    }
}

JSValue timeoutClear(JSContext* jsContext, JSValueConst /* thisValue */, int argc, JSValueConst* argv, TimeoutState& timeoutState) {
    try {
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
        if (timeoutState.scheduler == nullptr) {
            return JS_ThrowInternalError(jsContext, "timeoutClear: missing scheduler");
        }
        const int64_t timerID = static_cast<int64_t>(number);
        if (timeoutState.timerIDs.contains(timerID)) {
            const errors::Error error = timeoutState.scheduler->unregisterCallback(timerID);
            if (error.errorCode) {
                return JS_ThrowInternalError(jsContext, "timeoutClear: %s", error.toString().c_str());
            }
            timeoutState.timerIDs.erase(timerID);
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

JSModuleDef* createTimeoutImplModule(JSContext* jsContext, TimeoutState& timeoutState) {
    try {
        if (timeoutState.scheduler == nullptr) {
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
                .class_name = "TimeoutState",
                .finalizer = nullptr,
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
        // The JS object carries a borrowed pointer; it does not own the state.
        JS_SetOpaque(object.value, &timeoutState);
        JSModuleDef* jsModuleDef = JS_NewCModule(jsContext, "amj:timeout_impl", [](JSContext* jsContext, JSModuleDef* jsModuleDef) -> int {
            const ScopedJSValue object(jsContext, JS_GetModulePrivateValue(jsContext, jsModuleDef));
            if (JS_IsException(object.value)) {
                return -1;
            }
            JSValue functionData = object.value;
            const JSValue setFunction = JS_NewCFunctionData(jsContext, [](JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv, int /* magic */, JSValueConst* functionData) -> JSValue {
                // QuickJS supplies one function-data value, retained by the function.
                const JSValueConst holder = functionData[0];
                TimeoutState* timeoutState = static_cast<TimeoutState*>(JS_GetOpaque2(jsContext, holder, JS_GetClassID(holder)));
                if (timeoutState == nullptr) {
                    return JS_EXCEPTION;
                }
                return timeoutSet(jsContext, thisValue, argc, argv, *timeoutState);
            }, 2, 0, 1, &functionData);
            if (JS_IsException(setFunction) || JS_SetModuleExport(jsContext, jsModuleDef, "timeoutSet", setFunction) < 0) {
                return -1;
            }
            const JSValue clearFunction = JS_NewCFunctionData(jsContext, [](JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv, int /* magic */, JSValueConst* functionData) -> JSValue {
                const JSValueConst holder = functionData[0];
                TimeoutState* timeoutState = static_cast<TimeoutState*>(JS_GetOpaque2(jsContext, holder, JS_GetClassID(holder)));
                if (timeoutState == nullptr) {
                    return JS_EXCEPTION;
                }
                return timeoutClear(jsContext, thisValue, argc, argv, *timeoutState);
            }, 1, 0, 1, &functionData);
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
