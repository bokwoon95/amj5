#pragma once

#include <quickjs.h>

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace amj::javascript {

// Owns one QuickJS reference and releases it on destruction unless release()
// is called. Noncopyable. The context must outlive this wrapper. Use only on
// the context's owner thread.
class AutoJSValue {
  public:
    JSContext* jsContext = nullptr;
    JSValue value = JS_UNDEFINED;
    AutoJSValue(JSContext* jsContext, JSValue value) {
        this->jsContext = jsContext;
        this->value = value;
    }
    ~AutoJSValue() {
        JS_FreeValue(jsContext, value);
    }
    AutoJSValue(const AutoJSValue&) = delete;
    AutoJSValue& operator=(const AutoJSValue&) = delete;
    JSValue release() {
        return std::exchange(value, JS_UNDEFINED);
    }
};

// An in-memory JSON tree, not serialized JSON text. Numbers must be finite.
// MSVC map moves can allocate; implicit moves may throw and callers handle allocation failures.
struct JSONValue { // NOLINT(bugprone-exception-escape)
    std::variant<std::nullptr_t, bool, double, std::string, std::vector<JSONValue>, std::map<std::string, JSONValue>> value = nullptr;
};

// Creates an owned JS value. Objects have a null prototype. Returns JS_EXCEPTION
// with a pending exception on failure, including nesting deeper than 128 levels.
JSValue jsonToJSValue(JSContext* jsContext, const JSONValue& jsonValue);

// Converts to UTF-8 without type validation or size limits. Returns nullopt on
// failure with a pending JS exception. Check JS_IsString() first if coercion is unwanted.
std::optional<std::string> jsValueToString(JSContext* jsContext, JSValueConst value);

// Defines an own, writable/enumerable/configurable string property.
// Returns false with a pending JS exception on failure.
bool setJSStringField(const AutoJSValue& obj, const char* key, std::string_view value);

} // namespace amj::javascript
