#include "javascript.hpp"

#include <quickjs.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace amj::javascript {

std::optional<std::string> convertToString(JSContext* jsContext, JSValueConst value) {
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

} // namespace amj::javascript
