# JavaScript port: CallbackScheduler discussion handoff

## Goal and current status

We are porting `~/Documents/amj4` into this repository file by file. The next foundation under discussion is:

- `~/Documents/amj4/src/javascript/callback_scheduler.hpp`
- `~/Documents/amj4/src/javascript/callback_scheduler.cpp`

**Do not implement it yet.** The user still wants to understand its mechanisms before settling on a header/API signature they like. Continue explaining the existing implementation, then design the API together. The old implementation is reference material, not a design that must be preserved verbatim.

The user wants to understand exactly how future timeout and fetch modules will build on the scheduler. Concrete ownership, shared data shapes, thread boundaries, and call sequences are more useful than abstract descriptions or opaque wrappers. Keep exploratory answers concise and answer the specific question asked.

Only this handoff was written during this discussion; no scheduler implementation or header has been ported.

## Decisions already made

### Skip RuntimeClass

The previous agent recommended porting `class_id.hpp` first. Its `RuntimeClass` type holds a mutex and a map from `JSRuntime*` to `JSClassID`, lazily allocates IDs, and registers runtime-finalization cleanup.

The user rejected both this abstraction and the suggestion to rename it `RuntimeClassID`: a wrapper around a mutex and map does not deserve a separate class here, and these names obscure its actual contents.

**Do not port `class_id.hpp`.** Each future module should maintain its own internal mutex and per-runtime class-ID map, including runtime cleanup. The source uses QuickJS-NG's per-runtime class-ID allocation.

### Expose readiness as an object, not an opaque function

The old scheduler returns `std::function<void()>` from `readyNotifier(callbackID)`. The user explicitly wants the scheduler to expose a **`Readiness` object type** instead. A named readiness object with a notification method makes the mechanism clearer than an opaque function.

The exact API, ownership representation, and header signature are **not settled**. Do not interpret this decision as approval of the rest of the old interface.

## Execution model the user now understands

On the runtime's owner thread, C++ host logic and JavaScript execution alternate:

```text
C++ execution loop
  -> calls QuickJS to execute script
  <- script returns
  -> checks timers / completed native work
  -> calls QuickJS to execute a callback
  <- callback returns
  -> ...
```

QuickJS itself is native code: "running JavaScript" means the host called into the engine. JavaScript callbacks do not interrupt currently executing JavaScript. A timer becoming due only makes it eligible to run after control returns to the host.

This resembles cooperative multitasking, but ordinary callbacks run to completion rather than suspend/resume as coroutines. Promise continuations and async/await provide separate suspension-like behavior. Background HTTP workers can run native code concurrently, but they must not execute JavaScript.

The scheduler is an object, not a thread. Almost all its methods execute on the owner thread. In the old design, only returned readiness notifier functions may be invoked from worker threads.

## What CallbackScheduler actually does

It stores native callbacks to execute later on the owner thread. An entry can become ready through:

- A timer deadline passing.
- A background worker signalling readiness.

It does not implement HTTP, convert HTTP results to JS objects, or itself execute the QuickJS Promise-job queue. It invokes callbacks supplied by modules; those callbacks know their own data and JS operations.

The outer caller must alternate between executing QuickJS jobs, calling ready scheduler callbacks, and waiting when necessary. The scheduler yields while QuickJS jobs are pending, including between callbacks. For example, a fetch Promise continuation must get a chance to clear a timeout before the next timer callback runs.

### Timeout example

The timeout module registers a callback that invokes the JS function and a steady-clock deadline. The scheduler stores them and returns an ID. Later dispatch sees that the deadline passed, removes the entry, and invokes the callback.

There is no timer thread. The deadline is a timestamp used for readiness checks and for deciding how long the owner thread can sleep.

### Fetch: two different callbacks, not one

This distinction was initially confusing and is essential:

1. **Work callback:** registered with the scheduler by fetch. Runs later on the owner thread, reads native results, constructs JS values, and calls JS resolve/reject functions.
2. **Readiness notifier:** invoked by the HTTP worker. Only marks the work callback ready and wakes the owner thread. No JS conversion or resolve/reject happens here. Replace this opaque function with the agreed readiness object in the new design.

```text
Owner / JavaScript thread                   HTTP worker
-------------------------                   -----------
fetch module:
  Register work callback
  Obtain readiness handle
  Start worker ---------------------------> Perform HTTP request
                                            Store native data
                                            Signal readiness
Scheduler:                              <--- Mark ready / wake owner
  Invoke registered work callback

fetch module's callback:
  Read native data
  Construct JS values
  Call resolve() / reject()

Outer execution loop:
  Execute resulting QuickJS Promise jobs
```

Calling fetch's callback through the scheduler does not make its HTTP/JS translation logic part of the scheduler. The scheduler only knows it has a `void()` callback to invoke.

## The actual shared HTTP storage

The user asked what "somewhere" meant and whether worker and callback must agree on the result shape. Yes: both sides are implemented in `~/Documents/amj4/src/javascript/fetch.cpp` and share its native-only `Transport` struct.

`Transport` exists before the worker starts; the worker fills its fields. Relevant fields in the old source:

- `std::mutex mutex`
- `std::condition_variable condition`
- `int status`
- `std::vector<std::pair<std::string, std::string>> headers`
- `std::deque<std::vector<unsigned char>> chunks`
- `std::size_t bufferedBytes`
- `std::optional<Failure> failure` (error type and message)
- `std::exception_ptr workerException`
- Flags: `headersReceived`, `finished`, `canceled`
- Headers and body-read notifier handles
- HTTP client and `std::jthread worker`

The worker writes protected fields under the transport mutex, unlocks, and signals readiness. The fetch callback locks that same mutex, copies/moves the result out, unlocks, then constructs JS values and settles the Promise.

**Fetch defines the data contract, producer, and consumer. CallbackScheduler knows nothing about Transport and does not carry the HTTP result.** A readiness signal is just a doorbell: "your callback can now run."

## Streaming fetch: exactly how many scheduler callbacks?

This was the final topic discussed. In the existing implementation, callbacks are scheduled **per requested body read, not per arriving network chunk**.

### Initial fetch

Fetch registers:

- One headers callback (`deliverHeaders`) to settle the initial fetch Promise.
- One cleanup-only entry, with no work callback, whose cancellation hook closes/reaps the request during teardown.

When valid headers arrive, the worker stores status/headers and signals headers readiness. The scheduler eventually runs the headers callback once and removes it. Transport completion also signals headers readiness so failure before headers cannot leave the Promise waiting forever.

### Body reads

Each time JavaScript requests a body read:

1. Fetch creates a Promise for that read.
2. It registers a new one-shot scheduler callback calling `deliverRead(requestState)`.
3. It installs that entry's readiness notifier in the transport under the transport mutex.
4. If chunks are already buffered or the worker is finished, fetch immediately signals readiness itself. Otherwise it waits for the worker to signal.
5. Dispatch removes the scheduler entry and calls `deliverRead`.
6. `deliverRead` clears the transport's read notifier and takes one buffered chunk. If no chunks remain and the worker is finished, it produces completion or an error instead.
7. It converts the result to JS and settles that read's Promise.

Only one body read may be outstanding per request. Each read registers a new callback entry, but all use the same `deliverRead` implementation.

```text
Worker receives chunks A, B, C -> buffers them

JS requests read -> new callback -> delivers A
JS requests read -> new callback -> delivers B
JS requests read -> new callback -> delivers C
JS requests read -> new callback -> waits for more data or completion
```

### Worker notifications and buffering

- For each stored body chunk, the worker signals the current read readiness if a read notifier is installed.
- When the transport finishes (success or failure), it signals headers readiness and any installed body-read readiness.
- With no pending read, chunks are simply buffered. The worker does not register scheduler callbacks.
- If the bounded body buffer fills, the worker waits on the transport's own condition variable. Consuming a chunk wakes it so it can continue.
- Multiple signals before a read callback runs coalesce into one ready state; they do not enqueue multiple executions. Remaining chunks stay buffered for future reads.

This describes the old module's native read protocol; do not assume every JavaScript-level convenience method maps directly to one such read without inspecting the JS wrapper.

## Readiness, Go channels, and condition variables

The user's preferred mental model was Go's `chan struct{}` closed to signal readiness. They asked why C++ needs an opaque function rather than a concrete synchronization object.

Important explanation:

- A `std::condition_variable` provides waiting/wakeup, but **does not remember readiness**.
- A notification sent when nobody is waiting is not retained like a closed Go channel.
- The conventional C++ combination is a persistent ready flag, a mutex protecting it, and a condition variable to wake the recipient.
- The recipient checks the flag while holding the mutex. Waiting atomically releases that mutex and begins waiting, preventing a lost wakeup between checking readiness and sleeping.
- The scheduler must wait for **any pending operation or the nearest timer deadline**, not block on one request alone. Thus per-entry readiness uses a shared scheduler wakeup mechanism.
- `std::promise<void>` / `std::future<void>` is a standard persistent one-shot completion mechanism, but standard C++ does not provide a convenient wait-any-over-many-futures-plus-timer operation for this scheduler.

The old implementation already has concrete internal objects:

```text
Wakeup:
  mutex
  condition_variable

Readiness:
  shared ownership of Wakeup
  active flag
  ready flag
  readyAt timestamp
```

The returned notifier captures a shared pointer to `Readiness`. Under the shared wakeup mutex it ignores inactive entries, records the first readiness timestamp, and sets ready. Then it notifies the condition variable.

This is precisely the state the user wants represented explicitly in the public interface instead of hidden in `std::function<void()>`. A well-designed readiness object can preserve the same lifetime/thread-safety guarantees.

## Existing scheduler mechanisms still to review together

These are reference notes for the next agent, **not evidence the user has understood or approved each detail**. Read the original files before proposing an implementation.

- `Impl` holds borrowed `JSContext*`, owner thread ID, shared wakeup state, a lifetime token, map of callback entries, monotonically increasing IDs, and a cancellation guard.
- Each entry holds a work callback, a cancellation hook, optional steady-clock deadline, and shared readiness state.
- `addCallback` enforces owner-thread access and rejects registrations during cancellation. Cleanup-only entries are permitted. Old limits: 4096 pending entries and IDs no greater than 9007199254740991 (JS exact-integer boundary).
- Dispatch first yields if QuickJS jobs are pending. It snapshots ready entries, orders them by readiness/deadline time and then ID, and removes each entry before invoking its callback. Earlier callbacks can remove later entries. Newly scheduled entries wait for a subsequent pass.
- Readiness can come from a worker signal or an elapsed deadline; the earlier qualifying time is used.
- A worker may retain readiness after removal or scheduler destruction. Removal/cleanup marks it inactive; late signals are ignored. Worker-held state must not retain callbacks capturing JS values, a borrowed context, or a borrowed scheduler pointer.
- `waitForReadyCallbacks` executes no JS. It returns for queued QuickJS jobs or runnable callbacks; otherwise it waits on the shared condition variable, optionally until the nearest deadline. It throws if no callback-capable entry exists (cleanup-only entries do not count). This is not a general proof that an external worker will actually progress.
- `removeCallback` drops an entry and marks readiness inactive but **does not call its cancellation hook**.
- Normal callback dispatch also does not invoke cancellation hooks. Fetch's cleanup-only entry separately retains request cleanup responsibility.
- `cancelPendingWork` marks all old entries inactive, attempts every cancellation hook, releases callback captures on the owner thread, and rethrows the first failure after cleanup. It handles pending JS exceptions while doing this. The scheduler remains reusable, and IDs are not reused.
- The destructor calls cancellation and terminates on cleanup failure. Scheduler destruction must happen before destruction of its borrowed JS context.
- A weak lifetime token lets JS bindings detect expiration before dereferencing their borrowed scheduler pointer. It expires after destructor cleanup, not after each invocation.
- Worker result storage uses the transport mutex; scheduler readiness uses the scheduler wakeup mutex. They are separate synchronization domains.

## Latest discussion: await, threads, and Promise jobs

The user is walking through fetch step by step to understand exactly when control crosses between JS, native bindings, the owner loop, and HTTP workers. Do not assume the walkthrough has settled all their questions.

- `fetch()` returns a Promise; it does not block the owner thread waiting for HTTP. `await` suspends the async function, not the runtime or thread.
- The host starts JS by calling QuickJS evaluation APIs. For a script calling `main()`, `main` executes until suspension, returns its Promise, and the remaining top-level script runs to completion. Evaluation then returns to the host. Suspension does not interrupt arbitrary surrounding JS or immediately jump to the host loop.
- Native fetch bindings execute synchronously on the owner thread when called by JS. They register scheduler entries and start the HTTP worker before returning a Promise to JS. The outer owner loop does not start the request later.
- JS and the HTTP worker can execute concurrently. Only JS execution and outer host dispatch alternate on the owner thread.
- The HTTP worker exists because the current HTTP client performs blocking I/O. It prevents HTTP from blocking timers and other requests' completions/Promise continuations. With nothing runnable, the owner sleeps. A nonblocking HTTP implementation integrated into an event loop could avoid a separate HTTP worker.
- `setTimeout` starts no thread: register a callback and deadline, then invoke the callback on the owner thread after the deadline when control is available. Timers cannot interrupt executing JS.

### Two queues, concretely

A **scheduler callback** here is the native function fetch registers to read transport state, construct JS values, and call a Promise's resolve/reject function. A **Promise continuation** means the code after `await`, or a callback supplied to `.then()`. Calling resolve/reject queues Promise-related work in QuickJS; it does not directly resume the suspended function. The owner explicitly calls QuickJS's pending-job execution API to run that work.

Conceptual owner loop (JavaScript-style pseudocode, NOT actual API names or the exact old loop):

```js
// Actual JS:
async function* reply() {
    const response = await fetch(url);
    const reader = response.body.getReader();
    try {
        while (true) {
            const { value, done } = await reader.read();
            if (done) break;
            yield value;
        }
    } finally {
        reader.releaseLock();
    }
}
```

```js
// JS-style pseudocode for the C++ host; real QuickJS API names.
// Error handling and JS-value cleanup abbreviated.

const iterator = JS_Call(jsContext, replyFunction, providerObject, 0, null);
const nextFunction = JS_GetPropertyStr(jsContext, iterator, "next");

while (true) {
    const promise = JS_Call(jsContext, nextFunction, iterator, 0, null);
    if (JS_IsException(promise)) {
        throw JS_GetException(jsContext);
    }
    while (true) {
        callbackScheduler.callReadyCallbacks();
        let jobContext = null;
        while (JS_IsJobPending(jsRuntime)) {
            if (JS_ExecutePendingJob(jsRuntime, &jobContext) < 0) {
                throw JS_GetException(jobContext);
            }
        }
        const state = JS_PromiseState(jsContext, promise);
        if (state === JS_PROMISE_REJECTED) {
            throw JS_PromiseResult(jsContext, promise);
        }
        if (state === JS_PROMISE_FULFILLED) {
            break;
        }
        callbackScheduler.waitForReadyCallbacks();
    }
    const result = JS_PromiseResult(jsContext, promise);
    const done = JS_GetPropertyStr(jsContext, result, "done");
    if (JS_ToBool(jsContext, done) === 1) {
        break;
    }
    const value = JS_GetPropertyStr(jsContext, result, "value");
    deliverToCaller(value);
}
```

These are real API names. Only the surrounding syntax is JS-style; `&jobContext` retains C++’s output-parameter syntax. Native JS-value cleanup is omitted.

Termination policy is intentionally unspecified in this pseudocode. Dispatch must still yield to pending QuickJS jobs between callbacks, as described above.

`Promise.all`, `allSettled`, `any`, and `race` operate inside QuickJS, not CallbackScheduler. They add no scheduler entry of their own and do not automatically cancel unfinished HTTP requests. Respectively they settle when all fulfill/one rejects; all settle; one fulfills/all reject; or the first input settles.

`queueMicrotask(fn)` was introduced during the conversation and confused the user because its meaning was not explained. It is a browser API for explicitly queuing a JS function for microtask processing; it is not needed to explain fetch. Do not imply that amj4 already exposes this API or introduce it again without a reason.

### Exact fetch lifecycle clarified

The source was inspected at `C:/Users/bokwoonchua/Documents/amj4/src/javascript/fetch.cpp` and `fetch.js`.

1. JS `fetch` wrapper calls native `start` and awaits the metadata Promise.
2. Native `start`, still executing inside the JS-to-C++ call, creates transport and Promise state, registers cleanup and headers entries, installs headers readiness, then calls `transport.start()` to launch the worker. It returns the metadata Promise.
3. The calling async JS functions suspend; once surrounding JS finishes, evaluation/job execution returns to the owner loop.
4. Worker stores headers under the transport mutex and signals readiness. This only marks the headers callback ready and wakes the owner; it does not invoke that callback.
5. Owner dispatches the headers callback. It reads transport and resolves the metadata Promise (or rejects on error).
6. Owner executes QuickJS jobs. The JS fetch wrapper resumes, constructs a **Response**, and returns it, settling fetch's Promise. Subsequent Promise work resumes the caller after `await fetch(url)`.
7. JS requests `reader.read()`. Its wrapper calls native `read`, which creates a new Promise and registers a new one-shot scheduler entry for `deliverRead`. This is when the next callback is registered, NOT when bytes arrive.
8. Native read installs its readiness handle under the transport mutex and checks for buffered chunks or transport completion. If already ready it signals the handle itself; otherwise the worker will signal it later. JS awaits the read and suspends.
9. Owner dispatches that read callback, which takes one chunk or reports completion/error and settles the read Promise. The entry is removed before callback invocation. QuickJS jobs then resume JS consumption and the next read registers another entry.
10. Repeat until `done: true`. Each registration is new, but the native callback implementation is the same `deliverRead` function.

Worker byte arrivals only buffer bytes and notify an outstanding read, if one exists. No outstanding read means no new delivery callback registration. Multiple arrivals may precede a single read. Transport completion also wakes outstanding reads so EOF/errors can be delivered.

## Fetch body API and async iteration

- Correct browser spelling: `response.body.getReader()`, NOT `response.getReader()`.
- QuickJS supports `for await...of` syntax and async iteration.
- amj4 `src/javascript/fetch.js` supplies a custom `Body`/`Reader`, not a complete browser `ReadableStream`.
- Its existing supported interface is `response.body.getReader()`, followed by `await reader.read()` returning `{ value: Uint8Array, done: boolean }`.
- `Body` currently has no `[Symbol.asyncIterator]()`, so `for await (const chunk of response.body)` does NOT currently work.
- Adding that JS interface atop the existing reader is sufficient; no C++ scheduler/HTTP changes are needed. Browser-like iteration acquires a reader, awaits reads, cancels on early exit (e.g. `break`), and releases the lock when done.
- This was explained but **no fetch changes were requested or made**. Any walkthrough using `for await` is conditional on adding the iterator.

## Communication lessons from this discussion

- The user wants concrete actors and call sequences, not vague categories such as "other runnable JS or scheduled callbacks." In the fetch/timeout model, identify timers and HTTP callbacks followed by Promise continuations explicitly.
- Explain new terms at introduction. Do not casually introduce jobs, microtasks, or continuations as if already understood.
- Use **JavaScript-style pseudocode**, not Python, for explanatory code. Clearly distinguish conceptual pseudocode from real APIs and which thread runs it.
- Do not conflate higher-level JS features with native scheduler consumers. For the user's intended SSE implementation, native callbacks resolve raw-byte read Promises; JS parses SSE events. There is no separate SSE event scheduler callback.

## Suggested continuation

Continue from the user's fetch lifecycle walkthrough and answer the next specific uncertainty. The latest explanation covered initial evaluation, native registration before worker launch, headers-to-Promise-job execution, and registration of a fresh callback on each requested read. Do not implement fetch or its iterator unless asked. Distinguish existing behavior from proposed API choices. Only settle `callback_scheduler.hpp` and implement `.hpp`/`.cpp` once the user understands and approves the design.

Do not port fetch or timeout yet; use their old implementations only to explain the scheduler's consumers. Do not reintroduce `RuntimeClass` or an opaque readiness function.

## Possible future native event sources (not implementation commitments)

Other browser-like modules that could use an owner-thread delivery mechanism include:

| Module | What makes its callback ready |
|---|---|
| **WebSocket** | Connection opens, a message arrives, connection closes |
| **Asynchronous file/storage APIs** | A file read/write or database operation completes |
| **Web Workers** | Another worker posts a message |
| **`MessageChannel` / `postMessage`** | A message is queued for delivery |
| **UI/input events**—if you implement a UI | A mouse, keyboard, or window event arrives |
| **`requestAnimationFrame`**—if you implement rendering | The next rendering opportunity occurs |

These do **not** necessarily require one background thread each. They need a way to queue work for the owner thread; the underlying producer could be a worker thread, OS event loop, or another owner-thread callback.

These are illustrative possibilities, not proof that each feature needs its own native scheduler callback. Higher-level JS features can build on existing byte/message delivery. CallbackScheduler delivers native readiness into JS; QuickJS’s job queue handles resulting Promise continuations.
