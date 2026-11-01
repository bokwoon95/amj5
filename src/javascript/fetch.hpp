#pragma once

#include <quickjs.h>

#include <memory>

namespace amj::javascript {

class Scheduler;
struct FetchState;

// Native HTTP operations used by fetch.js. JS owns Promises, abort reasons and
// listeners, Error/Response objects, and public argument normalization. Native
// code owns workers, bounded buffers, and one-shot scheduler completion callbacks.
// All methods require the scheduler/context owner thread and matching JSContext.
class NativeFetch {
  public:
    // url/method: normalized strings; headerPairs: array of two-string arrays;
    // bytes: Uint8Array; onHeaders: function(failureOrNull, metadataOrNull).
    // Returns an opaque request handle, or JS_EXCEPTION on synchronous failure.
    // Later invokes onHeaders once through the scheduler. metadata is a plain
    // {status, headers: [[name, value], ...]}; failure is plain {type, message}.
    // No native Promise or JS Error is created. 4xx/5xx are successful responses.
    JSValue start(JSContext* jsContext, JSValueConst url, JSValueConst method, JSValueConst headerPairs, JSValueConst bytes, JSValueConst onHeaders);

    // request: handle from this NativeFetch instance, after successful headers.
    // onRead: function(failureOrNull, chunkOrNull). One outstanding read is allowed.
    // Later invokes onRead once through the scheduler. chunk is Uint8Array; null
    // with no failure means EOF. Returns undefined, or JS_EXCEPTION on failure.
    JSValue read(JSContext* jsContext, JSValueConst request, JSValueConst onRead);

    // Stops/joins the worker and unregisters pending callbacks. Idempotent for a
    // closed handle. Drops completion functions without invoking them. Returns
    // undefined, or JS_EXCEPTION on failure. JS settles its own cancelled Promises.
    JSValue close(JSContext* jsContext, JSValueConst request);

    // Returns a private JS object exposing start/read/close bound to an owned
    // NativeFetch instance, or JS_EXCEPTION. Pass it to JS
    // installFetch(target, nativeFetch, URL, TextEncoder, TextDecoder).
    // The scheduler/context are borrowed. Clear/destroy the scheduler before the
    // context. Cleanup invalidates the instance and joins workers without calling
    // JS; recreate after clear(). Completion callbacks hold weak native references,
    // so destroying scheduler callbacks cannot join workers under its mutex.
    //
    // HTTP(S) only, verified TLS, no credentials/redirects/retries/cookies. Limits:
    // 64 requests, 64 KiB headers/URL, 32 MiB upload, 64 KiB chunks, 256 KiB queued
    // body per request. Timeouts: 10s connect, 60s read, 30s write. DNS/connect/OS
    // calls may delay cancellation/join: use off the UI thread.
    static JSValue create(JSContext* jsContext, Scheduler& scheduler);

  private:
    std::shared_ptr<FetchState> fetchState = nullptr;
};

} // namespace amj::javascript
