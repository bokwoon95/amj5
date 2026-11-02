#pragma once

#include <quickjs.h>

namespace amj::javascript {

// Stateless Ada operations used by url.js. These methods accept already-coerced
// scalar-value strings: JS owns constructors, argument coercion, URLSearchParams
// lists/iteration, and live URL/query synchronization. No scheduler is needed.
// All calls require the JSContext's owner thread. Native failures return
// JS_EXCEPTION with a pending exception. Limits: 1 MiB UTF-8 input/output/query
// and 65536 query pairs. Nothing is installed on a target or global object.
class URLImpl {
  public:
    // input: string; baseOrNull: string or null. Invalid URLs/bases return null.
    // Otherwise returns a plain snapshot with string fields:
    // href, origin, protocol, username, password, host, hostname, port,
    // pathname, search, hash. search/hash include their leading ?/# when present.
    static JSValue parse(JSContext* jsContext, JSValueConst input, JSValueConst baseOrNull);

    // href/component/value: strings. component is href, protocol, username,
    // password, host, hostname, port, pathname, search, or hash.
    // Returns an updated snapshot with the same shape as parse(), or null if
    // Ada rejects the URL/change. JS decides whether rejection throws or is ignored.
    static JSValue set(JSContext* jsContext, JSValueConst href, JSValueConst component, JSValueConst value);

    // query: string (an optional leading ? is accepted).
    // Returns [[name, value], ...], preserving duplicates and order, using Ada's
    // application/x-www-form-urlencoded decoding (including + and invalid UTF-8).
    static JSValue parseQuery(JSContext* jsContext, JSValueConst query);

    // pairs: array of two-string arrays. Returns an encoded query without ?.
    static JSValue serializeQuery(JSContext* jsContext, JSValueConst pairs);

    // Returns a private JS object exposing parse, set, parseQuery, serializeQuery,
    // bound to the corresponding methods above, or JS_EXCEPTION on failure.
    // Used by createModule(). No public URL classes live in C++.
    static JSValue create(JSContext* jsContext);

    // Registers amj:url, exporting urlImpl. Call once per context before loading
    // url.js. Returns a context-owned module, or nullptr with a pending exception.
    static JSModuleDef* createModule(JSContext* jsContext);
};

} // namespace amj::javascript
