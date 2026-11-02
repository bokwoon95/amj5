#pragma once

#include <quickjs.h>

namespace amj::javascript {

class Scheduler;

// C++ implementation of the amj:timeout_impl module; no JS facade.
// JS exports: setTimeout(callback, delay, ...arguments) -> number,
// clearTimeout(id) -> undefined. No timer thread: runNext() waits for deadlines
// and invokes one callback. JS callback exceptions remain pending and are
// returned by runNext(). Limits: 128 pending timers and 256 extra arguments per
// owner. Delays are clamped to [0, 2147483647] milliseconds; IDs are exactly
// representable in JS. Create/use on the scheduler/context owner thread with the
// same JSContext. The scheduler and context are borrowed; clear/destroy the
// scheduler before the context. Cleanup cancels timers without calling JS.
class TimeoutImpl {
  public:
    // Returns a JS object exposing setTimeout/clearTimeout, or JS_EXCEPTION.
    // Create a replacement after scheduler.clear(). Used by createModule().
    static JSValue create(JSContext* jsContext, Scheduler& scheduler);

    // Registers amj:timeout_impl with those two direct function exports.
    // Call once per context before loading consumers. Returns a context-owned
    // module, or nullptr with a pending exception. Use a fresh context/module
    // registration after clearing the scheduler.
    static JSModuleDef* createModule(JSContext* jsContext, Scheduler& scheduler);
};

} // namespace amj::javascript
