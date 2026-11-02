#include "javascript.hpp"

#include <quickjs.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace amj::javascript {

std::optional<std::string> jsValueToString(JSContext* jsContext, JSValueConst value) {
    std::size_t length = 0;
    const char* text = JS_ToCStringLen(jsContext, &length, value);
    if (text == nullptr) {
        return std::nullopt;
    }
    const auto release = [jsContext](const char* data) {
        JS_FreeCString(jsContext, data);
    };
    const std::unique_ptr<const char, decltype(release)> ownedText(text, release);
    return std::string(text, length);
}

bool setJSStringField(const AutoJSValue& obj, const char* key, std::string_view value) {
    const JSValue text = JS_NewStringLen(obj.jsContext, value.data(), value.size());
    if (JS_IsException(text)) {
        return false;
    }
    return JS_DefinePropertyValueStr(obj.jsContext, obj.value, key, text, JS_PROP_C_W_E) >= 0;
}

} // namespace amj::javascript
