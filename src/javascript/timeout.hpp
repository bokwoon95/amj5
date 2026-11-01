#pragma once

#include <quickjs.h>

namespace amj::javascript {

class Scheduler;

// Installs setTimeout(callback, delay, ...arguments) and clearTimeout(id) on target.
// No timer thread: runNext() waits for deadlines and invokes one callback.
// JS callback exceptions remain pending and are returned by runNext().
// Limits: 128 pending timers and 256 extra arguments per installation. Delays
// are clamped to [0, 2147483647] milliseconds. IDs are exactly representable in JS.
// Install/use on the scheduler/context owner thread, with the same JSContext.
// The scheduler and context are borrowed; clear/destroy the scheduler before the
// context. Cleanup cancels timers without calling JS. Reinstall after clear().
// Returns 0 or -1 with a pending JS exception. Installation may be partial.
int installTimeout(JSContext* jsContext, JSValueConst target, Scheduler& scheduler);

} // namespace amj::javascript
