#pragma once

#include <quickjs.h>

#include <cstdint>
#include <map>
#include <memory>

namespace amj::javascript {

class Scheduler;

// Shared by the host and JS bindings, one per scheduler/context pair. Do not
// copy or modify while in use; the shared scheduler must not change. Callbacks
// reference this state weakly. All use and final ownership release require the
// scheduler/context owner thread. Clear pending work before context teardown;
// the context must outlive the scheduler and retained JS values.
// scheduler.clear() releases pending timers; this state and its module remain usable.
struct Timeout {
    std::shared_ptr<Scheduler> scheduler = nullptr;
    int32_t timerIDCounter = 0; // Last allocated timer ID; allocation starts at 1.
    std::map<int32_t, int64_t> timerCallbackIDs = {}; // JS timer ID -> scheduler callback ID.
};

// callback: function; delay: normalized integer milliseconds in [0, 2147483647].
// Returns a timer ID in [1, INT32_MAX], or JS_EXCEPTION. JS owns coercion,
// clamping, and forwarding extra callback arguments.
JSValue timeoutSet(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv, std::shared_ptr<Timeout> timeout);

// timerID: nonnegative safe integer. Unknown/cleared IDs have no effect.
// Returns undefined, or JS_EXCEPTION. Only cancels this state's timers.
JSValue timeoutClear(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv, std::shared_ptr<Timeout> timeout);

// Registers amj:timeout_impl, exporting timeoutSet and timeoutClear. Call once
// per context before loading timeout.js. Retains shared ownership of timeout.
// Returns a context-owned module, or nullptr with a pending exception.
JSModuleDef* createTimeoutImplModule(JSContext* jsContext, std::shared_ptr<Timeout> timeout);

} // namespace amj::javascript
