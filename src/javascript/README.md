# JavaScript modules

Dependency order: **URL → text → scheduler → timeout → fetch**. Modules install
only onto caller-supplied targets, never automatically onto the global object.

## Native/JS boundary

`url.hpp` and `fetch.hpp` declare every native operation used by the JS modules,
including argument types, callback shapes, results, and limits.

- `NativeURL::create(jsContext)` returns `{parse, set, parseQuery, serializeQuery}`.
  These are stateless Ada adapters. `url.js` implements the public `URL` and
  `URLSearchParams` classes, coercion, iteration, and live query synchronization.
- `installText(target)` in `text.js` provides UTF-8 `TextEncoder`/`TextDecoder`.
- C++ `installTimeout(jsContext, target, scheduler)` directly installs timers;
  there is no competing JS installer.
- `NativeFetch::create(jsContext, scheduler)` returns `{start, read, close}` bound
  to one native HTTP owner. `start` returns an opaque request, not a Promise.
  Completion callbacks are one-shot scheduler callbacks on the owner thread.
  `fetch.js` creates/settles Promises and implements Errors, abort handling,
  Headers, Response, and body readers. There is no C++ `installFetch`.

The host checks the factory results for `JS_EXCEPTION`, passes them to the JS
installers, and releases its own JSValue references afterward. All native
instances must use the same context and owner thread as their scheduler.

With those native objects supplied to JS:

```javascript
const url = {};
installURL(url, nativeURL);
const text = {};
installText(text);
installFetch(target, nativeFetch, url.URL, text.TextEncoder, text.TextDecoder);
```

Fetch depends on the supplied constructors, not properties of its installation
target. Calls use `nativeURL.method()`/`nativeFetch.method()` directly; native
methods are not rebound to local aliases.

CMake copies `url.js`, `text.js`, and `fetch.js` into the build's `javascript/`
directory. The host loads the modules and calls their exported installers.

## Scheduling and cleanup

The outer owner loop drains all QuickJS jobs, checks its awaited Promise, then
calls `scheduler.runNext()` to wait for and execute one native callback. Repeat.
The scheduler does not execute the QuickJS job queue itself.

HTTP workers access native buffers only and call `markReady()`. Their request
owners remain alive until explicit close or module cleanup. Scheduler callbacks
hold weak request references, so removing them cannot join a worker under the
scheduler mutex. Buffer locks are also released before notifying the scheduler
or calling JS. Completion callback exceptions propagate through `runNext()`.

`NativeFetch.close(request)` joins the worker and drops pending completion
functions without calling them. JS cancellation settles its pending Promises
and closes the handle; scheduler teardown deliberately does not settle abandoned
Promises or run JS. JS callers should cancel/release readers in `finally`.
Cancellation/join can block on native DNS/connect/OS calls: do not run this host
loop on the UI thread.

Clear/destroy the scheduler before freeing the JS context. `clear()` invalidates
native fetch instances and timer installations; create/install replacements to
reuse the scheduler. URL and text do not depend on scheduler lifetime.

The fetch facade remains a subset: HTTP(S), no redirects/cookies/retries,
string/Uint8Array uploads, `.text()`/`.json()`, and `body.getReader()` streaming.
There are no public Request/Response constructors, cloning, full WHATWG streams,
or body async iteration. Native HTTP validation and memory limits remain enforced
in C++ even though public normalization and API behavior are implemented in JS.
