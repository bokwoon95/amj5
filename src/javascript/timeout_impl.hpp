#pragma once

#include <quickjs.h>

#include <cstdint>
#include <memory>

namespace amj::javascript {

class Scheduler;
struct TimeoutImplState;

// Scheduler mechanics for timeout.js. JS owns coercion, clamping, and forwarding
// extra callback arguments. No timer thread: runNext() waits for deadlines and
// invokes one callback. JS exceptions remain pending and propagate via runNext().
// Create/use on the scheduler/context owner thread with the same JSContext.
// The scheduler/context are borrowed; clear/destroy the scheduler before the
// context. Cleanup cancels timers without executing JS.
class TimeoutImpl {
  public:
    // callback: callable JS value invoked without arguments. delay: normalized
    // integer milliseconds in [0, 2147483647]. Returns a numeric timer ID or
    // JS_EXCEPTION. At most 128 pending timers; IDs are exactly representable in JS.
    JSValue setTimeout(JSContext* jsContext, JSValueConst callback, int64_t delay);

    // timerID: normalized integer. Unknown/cleared IDs have no effect.
    // Returns undefined, or JS_EXCEPTION on failure.
    JSValue clearTimeout(JSContext* jsContext, int64_t timerID);

    // Registers amj:timeout_impl, exporting timeoutImpl. Call once per context
    // before loading timeout.js. Returns a context-owned module, or nullptr with
    // a pending exception. Use fresh registration/context after scheduler.clear().
    static JSModuleDef* createModule(JSContext* jsContext, Scheduler& scheduler);

  private:
    std::shared_ptr<TimeoutImplState> timeoutImplState = nullptr;
};

} // namespace amj::javascript
