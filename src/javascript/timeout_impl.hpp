#pragma once

#include <quickjs.h>

#include <cstdint>
#include <set>

namespace amj::javascript {

class Scheduler;

// Caller-owned, one per scheduler/context pair. Do not copy, move, or modify
// while in use. The scheduler is borrowed and must not change. This state must
// outlive its module's use and all pending callbacks; the context must outlive
// the scheduler. All operations require the scheduler/context owner thread.
// scheduler.clear() releases pending timers; this state and its module remain usable.
struct TimeoutState {
    Scheduler* scheduler = nullptr;
    std::set<int64_t> timerIDs = {};
};

// callback: function; delay: normalized integer milliseconds in [0, 2147483647].
// Returns a numeric safe-integer timer ID, or JS_EXCEPTION. At most 65535 pending
// timers. JS owns coercion, clamping, and forwarding extra callback arguments.
JSValue timeoutSet(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv, TimeoutState& timeoutState);

// timerID: nonnegative safe integer. Unknown/cleared IDs have no effect.
// Returns undefined, or JS_EXCEPTION. Only cancels this state's timers.
JSValue timeoutClear(JSContext* jsContext, JSValueConst thisValue, int argc, JSValueConst* argv, TimeoutState& timeoutState);

// Registers amj:timeout_impl, exporting timeoutSet and timeoutClear. Call once
// per context before loading timeout.js. Borrows timeoutState. Returns a
// context-owned module, or nullptr with a pending exception.
JSModuleDef* createTimeoutImplModule(JSContext* jsContext, TimeoutState& timeoutState);

} // namespace amj::javascript
