# JavaScript modules

Dependency order: **text -> URL -> scheduler -> timeout -> fetch**. JavaScript
facades export their public API; they do not install anything on the global object.
The host chooses which target receives those exports.

## Native/JS boundary

- `text.js` exports UTF-8 `TextEncoder` and `TextDecoder` classes.
- `URLImpl::createModule(jsContext)` registers `amj:url`, exporting `urlImpl`.
  Its stateless Ada operations are `parse`, `set`, `parseQuery`, and
  `serializeQuery`. `url.js` imports that object and exports `URL` and
  `URLSearchParams`, implementing coercion, iteration, and query synchronization.
- C++ `installTimeout(jsContext, target, scheduler)` directly installs
  `setTimeout` and `clearTimeout`. There is no JS facade.
- `FetchImpl::createModule(jsContext, scheduler)` registers `amj:fetch`,
  exporting `fetchImpl` with `start`, `read`, and `close` operations bound to
  one native HTTP owner. `fetch.js` imports that object and the URL/text classes,
  and exports `Headers` and `fetch`. Response objects and body readers remain
  internal classes, not public constructors.

Register each native module once per context before loading the JS facades;
check for nullptr and a pending JS exception. Module definitions belong to the
context, not the caller. The lower-level `URLImpl::create()` and
`FetchImpl::create()` object factories remain available.

The host must provide a module loader for the JS files and preserve the registered
`amj:url` / `amj:fetch` module names. After evaluating each facade, the host can
obtain its namespace with `JS_GetModuleNamespace`, enumerate own string exports,
and copy their values onto its chosen target. No installer functions or
hardcoded constructor-name lists are needed. Check evaluation/property-operation
failures and manage QuickJS value and atom references normally. Decide explicitly
whether existing target properties may be overwritten. Copying exports copies
values rather than installing live module bindings; these API exports are not
subsequently reassigned.

Native calls use `urlImpl.method()` / `fetchImpl.method()` directly. All native
instances use the same context and owner thread as their scheduler.

CMake copies the JS facades and their `.d.ts` files into the build's
`javascript/` directory. Keep the files together when distributing them.

## Static type checking

The JS implementations use JSDoc and `// @ts-check`, without transpilation.
`url_impl.d.ts` and `fetch_impl.d.ts` describe the `URLImpl` and `FetchImpl`
interfaces exported by the corresponding C++ implementations. Their module names
remain `amj:url` and `amj:fetch`; JS imports `urlImpl` and `fetchImpl` respectively.
These declarations, `text_globals.d.ts`, and `timeout.d.ts` also declare the globals
the host makes available to user scripts. JS-backed globals reference exported
classes/functions with `typeof import(...)`, avoiding duplicated public API
signatures. Timer globals are declared directly, since they have no JS facade.

The `_impl` / `_globals` filename suffixes avoid shadowing inferred JS module
types: a `url.d.ts` beside `url.js` would take precedence when resolving imports.
Native declarations are static contracts, not runtime validation; keep them
synchronized with the corresponding C++ headers and implementation.

The root `tsconfig.json` references these declarations instead of browser/Node
libraries. `src/javascript/tsconfig.json` checks the implementation project.
Both retain strict checking, checked indexed access, and exact optional
properties. Run from the repository root with TypeScript 5.2 or newer:

```sh
tsc --project tsconfig.json
tsc --project src/javascript/tsconfig.json
```

For future user-script directories, generate a tsconfig with `allowJs`,
`checkJs`, `noEmit`, `lib: ["ES2023"]`, `types: []`, and a `files` list pointing
to the installed `text_globals.d.ts`, `url_impl.d.ts`, `timeout.d.ts`, and
`fetch_impl.d.ts` files. Relative imports within the
declarations resolve against their install directory. Editors can then navigate
JS-backed APIs to their exported implementations and native-only APIs to their
declarations. These editor references do not import code into QuickJS or install
runtime globals. No browser DOM or Node APIs are assumed.

## Scheduling and cleanup

The outer owner loop drains all QuickJS jobs, checks its awaited Promise, then
calls `scheduler.runNext()` to wait for and execute one native callback. Repeat.
The scheduler does not execute the QuickJS job queue itself.

HTTP workers access native buffers only and call `markReady()`. Their request
owners remain alive until explicit close or module cleanup. Scheduler callbacks
hold weak request references, so removing them cannot join a worker under the
scheduler mutex. Buffer locks are released before notifying the scheduler or
calling JS. Completion callback exceptions propagate through `runNext()`.

`fetchImpl.close(request)` joins the worker and drops pending completion
functions without calling them. JS cancellation settles its pending Promises and
closes the handle; scheduler teardown deliberately does not settle abandoned
Promises or run JS. Callers should cancel/release readers in `finally`.
Cancellation/join can block on native DNS/connect/OS calls: do not run this host
loop on the UI thread.

Clear/destroy the scheduler before freeing the JS context. `clear()` invalidates
native fetch instances and timer installations. Reinstall timers and use a fresh
context/module registration for fetch, or use a new lower-level native owner.
URL and text do not depend on scheduler lifetime.

The fetch facade remains a subset: HTTP(S), no redirects/cookies/retries,
string/Uint8Array uploads, `.text()`/`.json()`, and `body.getReader()` streaming.
There are no public Request/Response constructors, cloning, full WHATWG streams,
or body async iteration. Native HTTP validation and memory limits remain enforced
in C++ even though public normalization and API behavior are implemented in JS.
