#pragma once

#include <quickjs.h>

#include <cstdint>
#include <string>

namespace amj::javascript {

// Component strings accepted by set(), represented as an enum inside C++.
enum class URLComponent : std::uint8_t {
    Href,
    Protocol,
    Username,
    Password,
    Host,
    Hostname,
    Port,
    Pathname,
    Search,
    Hash,
};

// Plain snapshot converted to a JS object with these same string fields.
struct URLParts {
    std::string href = "";
    std::string origin = "";
    std::string protocol = "";
    std::string username = "";
    std::string password = "";
    std::string host = "";
    std::string hostname = "";
    std::string port = "";
    std::string pathname = "";
    std::string search = "";
    std::string hash = "";
};

// Stateless Ada operations used by url.js. These methods accept already-coerced
// scalar-value strings: JS owns constructors, argument coercion, URLSearchParams
// lists/iteration, and live URL/query synchronization. No scheduler is needed.
// All calls require the JSContext's owner thread. Native failures return
// JS_EXCEPTION with a pending exception. Limits: 1 MiB UTF-8 input/output/query
// and 65536 query pairs. Nothing is installed on a target or global object.
class URLImpl {
  public:
    // input: string; baseOrNull: string or null. Invalid URLs/bases return null.
    // Otherwise returns URLParts as a plain JS object.
    // search/hash include their leading ?/# when present.
    static JSValue parse(JSContext* jsContext, JSValueConst input, JSValueConst baseOrNull);

    // href/component/value: strings. component is a URLComponent string:
    // href, protocol, username, password, host, hostname, port, pathname, search, hash.
    // Returns an updated snapshot with the same shape as parse(), or null if
    // Ada rejects the URL/change. JS decides whether rejection throws or is ignored.
    static JSValue set(JSContext* jsContext, JSValueConst href, JSValueConst component, JSValueConst value);

    // query: string (an optional leading ? is accepted).
    // Returns [[name, value], ...], preserving duplicates and order, using Ada's
    // application/x-www-form-urlencoded decoding (including + and invalid UTF-8).
    static JSValue parseQuery(JSContext* jsContext, JSValueConst query);

    // pairs: array of two-string arrays. Returns an encoded query without ?.
    static JSValue serializeQuery(JSContext* jsContext, JSValueConst pairs);

    // Registers amj:url_impl, exporting urlImpl. Call once per context before loading
    // url.js. Returns a context-owned module, or nullptr with a pending exception.
    static JSModuleDef* createModule(JSContext* jsContext);
};

} // namespace amj::javascript
