#include "url.hpp"

#include <ada.h> // IWYU pragma: keep
#include <ada/implementation.h>
#include <ada/url_aggregator.h>
#include <ada/url_search_params.h>
#include <quickjs.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace amj::javascript {
namespace {

// NOLINTBEGIN(bugprone-throwing-static-initialization): JS_MKVAL only constructs scalars.
const JSValue undefinedValue = JS_UNDEFINED;
const JSValue nullValue = JS_NULL;
const JSValue exceptionValue = JS_EXCEPTION;
// NOLINTEND(bugprone-throwing-static-initialization)
constexpr std::size_t inputLimit = 1048576;
constexpr int64_t pairLimit = 65536;

class JSValueOwner {
  public:
    JSContext* jsContext = nullptr;
    JSValue value = undefinedValue;
    JSValueOwner(JSContext* jsContext, JSValue value) {
        this->jsContext = jsContext;
        this->value = value;
    }
    ~JSValueOwner() {
        JS_FreeValue(jsContext, value);
    }
    JSValueOwner(const JSValueOwner&) = delete;
    JSValueOwner& operator=(const JSValueOwner&) = delete;
    JSValue release() {
        return std::exchange(value, undefinedValue);
    }
};

std::optional<std::string> readString(JSContext* jsContext, JSValueConst value) {
    if (!JS_IsString(value)) {
        JS_ThrowTypeError(jsContext, "Native URL operations require strings");
        return std::nullopt;
    }
    std::size_t length = 0;
    const char* text = JS_ToCStringLen(jsContext, &length, value);
    if (text == nullptr) {
        return std::nullopt;
    }
    const auto release = [jsContext](const char* data) {
        JS_FreeCString(jsContext, data);
    };
    const std::unique_ptr<const char, decltype(release)> ownedText(text, release);
    if (length > inputLimit) {
        JS_ThrowRangeError(jsContext, "URL input exceeds 1 MiB");
        return std::nullopt;
    }
    return std::string(text, length);
}

JSValue snapshot(JSContext* jsContext, const ada::url_aggregator& url) {
    if (url.get_href().size() > inputLimit) {
        return JS_ThrowRangeError(jsContext, "Normalized URL exceeds 1 MiB");
    }
    const std::string_view search = url.get_search();
    if (std::count(search.begin(), search.end(), '&') >= pairLimit) {
        return JS_ThrowRangeError(jsContext, "URL query exceeds 65536 pairs");
    }
    const std::string origin = url.get_origin();
    const std::array<std::pair<const char*, std::string_view>, 11> fields = {
        {{"href", url.get_href()}, {"origin", origin}, {"protocol", url.get_protocol()}, {"username", url.get_username()}, {"password", url.get_password()}, {"host", url.get_host()}, {"hostname", url.get_hostname()}, {"port", url.get_port()}, {"pathname", url.get_pathname()}, {"search", search}, {"hash", url.get_hash()}}};
    const JSValueOwner result(jsContext, JS_NewObjectProto(jsContext, nullValue));
    if (JS_IsException(result.value)) {
        return exceptionValue;
    }
    for (const std::pair<const char*, std::string_view>& field : fields) {
        const JSValue text = JS_NewStringLen(jsContext, field.second.data(), field.second.size());
        if (JS_IsException(text) || JS_DefinePropertyValueStr(jsContext, result.value, field.first, text, JS_PROP_C_W_E) < 0) {
            return exceptionValue;
        }
    }
    return JS_DupValue(jsContext, result.value);
}

JSValue urlOperation(JSContext* jsContext, JSValueConst /* thisValue */, int argc, JSValueConst* argv, int operation) {
    if (operation == 0 && argc >= 2) {
        return NativeURL::parse(jsContext, argv[0], argv[1]);
    }
    if (operation == 1 && argc >= 3) {
        return NativeURL::set(jsContext, argv[0], argv[1], argv[2]);
    }
    if (operation == 2 && argc >= 1) {
        return NativeURL::parseQuery(jsContext, argv[0]);
    }
    if (operation == 3 && argc >= 1) {
        return NativeURL::serializeQuery(jsContext, argv[0]);
    }
    return JS_ThrowTypeError(jsContext, "Missing native URL arguments");
}

} // namespace

JSValue NativeURL::parse(JSContext* jsContext, JSValueConst input, JSValueConst baseOrNull) try {
    const std::optional<std::string> inputText = readString(jsContext, input);
    if (!inputText.has_value()) {
        return exceptionValue;
    }
    ada::result<ada::url_aggregator> parsed;
    if (JS_IsNull(baseOrNull)) {
        parsed = ada::parse<ada::url_aggregator>(inputText.value(), nullptr);
    } else {
        const std::optional<std::string> baseText = readString(jsContext, baseOrNull);
        if (!baseText.has_value()) {
            return exceptionValue;
        }
        const ada::result<ada::url_aggregator> base = ada::parse<ada::url_aggregator>(baseText.value(), nullptr);
        if (!base.has_value()) {
            return nullValue;
        }
        parsed = ada::parse<ada::url_aggregator>(inputText.value(), &base.value());
    }
    if (!parsed.has_value()) {
        return nullValue;
    }
    return snapshot(jsContext, parsed.value());
} catch (const std::bad_alloc&) {
    return JS_ThrowOutOfMemory(jsContext);
} catch (const std::exception& exception) {
    return JS_ThrowInternalError(jsContext, "%s", exception.what());
} catch (...) {
    return JS_ThrowInternalError(jsContext, "Unexpected native URL parsing failure");
}

JSValue NativeURL::set(JSContext* jsContext, JSValueConst href, JSValueConst component, JSValueConst value) try {
    const std::optional<std::string> hrefText = readString(jsContext, href);
    const std::optional<std::string> componentText = readString(jsContext, component);
    const std::optional<std::string> valueText = readString(jsContext, value);
    if (!hrefText.has_value() || !componentText.has_value() || !valueText.has_value()) {
        return exceptionValue;
    }
    ada::result<ada::url_aggregator> parsed = ada::parse<ada::url_aggregator>(hrefText.value(), nullptr);
    if (!parsed.has_value()) {
        return nullValue;
    }
    ada::url_aggregator& url = parsed.value();
    const std::string& name = componentText.value();
    bool accepted = true;
    if (name == "href") {
        accepted = url.set_href(valueText.value());
    } else if (name == "protocol") {
        accepted = url.set_protocol(valueText.value());
    } else if (name == "username") {
        accepted = url.set_username(valueText.value());
    } else if (name == "password") {
        accepted = url.set_password(valueText.value());
    } else if (name == "host") {
        accepted = url.set_host(valueText.value());
    } else if (name == "hostname") {
        accepted = url.set_hostname(valueText.value());
    } else if (name == "port") {
        accepted = url.set_port(valueText.value());
    } else if (name == "pathname") {
        accepted = url.set_pathname(valueText.value());
    } else if (name == "search") {
        url.set_search(valueText.value());
    } else if (name == "hash") {
        url.set_hash(valueText.value());
    } else {
        return JS_ThrowTypeError(jsContext, "Unknown URL component");
    }
    if (!accepted) {
        return nullValue;
    }
    return snapshot(jsContext, url);
} catch (const std::bad_alloc&) {
    return JS_ThrowOutOfMemory(jsContext);
} catch (const std::exception& exception) {
    return JS_ThrowInternalError(jsContext, "%s", exception.what());
} catch (...) {
    return JS_ThrowInternalError(jsContext, "Unexpected native URL mutation failure");
}

JSValue NativeURL::parseQuery(JSContext* jsContext, JSValueConst query) try {
    const std::optional<std::string> text = readString(jsContext, query);
    if (!text.has_value()) {
        return exceptionValue;
    }
    if (std::count(text.value().begin(), text.value().end(), '&') >= pairLimit) {
        return JS_ThrowRangeError(jsContext, "URL query exceeds 65536 pairs");
    }
    const ada::url_search_params searchParams(text.value());
    JSValueOwner result(jsContext, JS_NewArray(jsContext));
    if (JS_IsException(result.value)) {
        return exceptionValue;
    }
    uint32_t index = 0;
    for (const std::pair<std::string, std::string>& pair : searchParams) {
        JSValueOwner fields(jsContext, JS_NewArray(jsContext));
        if (JS_IsException(fields.value)) {
            return exceptionValue;
        }
        const std::array<std::string_view, 2> strings = {pair.first, pair.second};
        for (uint32_t field = 0; field < strings.size(); field++) {
            const std::string_view string = strings.at(field);
            const JSValue value = JS_NewStringLen(jsContext, string.data(), string.size());
            if (JS_IsException(value) || JS_DefinePropertyValueUint32(jsContext, fields.value, field, value, JS_PROP_C_W_E) < 0) {
                return exceptionValue;
            }
        }
        if (JS_DefinePropertyValueUint32(jsContext, result.value, index, fields.release(), JS_PROP_C_W_E) < 0) {
            return exceptionValue;
        }
        index++;
    }
    return result.release();
} catch (const std::bad_alloc&) {
    return JS_ThrowOutOfMemory(jsContext);
} catch (const std::exception& exception) {
    return JS_ThrowInternalError(jsContext, "%s", exception.what());
} catch (...) {
    return JS_ThrowInternalError(jsContext, "Unexpected native query parsing failure");
}

JSValue NativeURL::serializeQuery(JSContext* jsContext, JSValueConst pairs) try {
    int64_t count = 0;
    if (!JS_IsArray(pairs) || JS_GetLength(jsContext, pairs, &count) < 0) {
        if (JS_HasException(jsContext)) {
            return exceptionValue;
        }
        return JS_ThrowTypeError(jsContext, "Query pairs must be an array");
    }
    if (count < 0 || count > pairLimit) {
        return JS_ThrowRangeError(jsContext, "URL query exceeds 65536 pairs");
    }
    ada::url_search_params searchParams;
    std::size_t bytes = 0;
    for (int64_t index = 0; index < count; index++) {
        const JSValueOwner pair(jsContext, JS_GetPropertyUint32(jsContext, pairs, static_cast<uint32_t>(index)));
        int64_t length = 0;
        if (JS_IsException(pair.value)) {
            return exceptionValue;
        }
        if (!JS_IsArray(pair.value) || JS_GetLength(jsContext, pair.value, &length) < 0 || length != 2) {
            if (JS_HasException(jsContext)) {
                return exceptionValue;
            }
            return JS_ThrowTypeError(jsContext, "Expected two-string query pairs");
        }
        const JSValueOwner name(jsContext, JS_GetPropertyUint32(jsContext, pair.value, 0));
        const JSValueOwner value(jsContext, JS_GetPropertyUint32(jsContext, pair.value, 1));
        if (JS_IsException(name.value) || JS_IsException(value.value)) {
            return exceptionValue;
        }
        const std::optional<std::string> nameText = readString(jsContext, name.value);
        const std::optional<std::string> valueText = readString(jsContext, value.value);
        if (!nameText.has_value() || !valueText.has_value()) {
            return exceptionValue;
        }
        for (const std::string* field : {&nameText.value(), &valueText.value()}) {
            if (field->size() > inputLimit - bytes) {
                return JS_ThrowRangeError(jsContext, "URL query exceeds 1 MiB");
            }
            bytes += field->size();
        }
        searchParams.append(nameText.value(), valueText.value());
    }
    const std::string encoded = searchParams.to_string();
    if (encoded.size() > inputLimit) {
        return JS_ThrowRangeError(jsContext, "Encoded URL query exceeds 1 MiB");
    }
    return JS_NewStringLen(jsContext, encoded.data(), encoded.size());
} catch (const std::bad_alloc&) {
    return JS_ThrowOutOfMemory(jsContext);
} catch (const std::exception& exception) {
    return JS_ThrowInternalError(jsContext, "%s", exception.what());
} catch (...) {
    return JS_ThrowInternalError(jsContext, "Unexpected native query serialization failure");
}

JSValue NativeURL::create(JSContext* jsContext) {
    JSValueOwner result(jsContext, JS_NewObjectProto(jsContext, nullValue));
    if (JS_IsException(result.value)) {
        return exceptionValue;
    }
    const std::array<const char*, 4> names = {"parse", "set", "parseQuery", "serializeQuery"};
    const std::array<int, 4> argumentCounts = {2, 3, 1, 1};
    for (std::size_t index = 0; index < names.size(); index++) {
        const JSValue function = JS_NewCFunctionMagic(jsContext, urlOperation, names.at(index), argumentCounts.at(index), JS_CFUNC_generic_magic, static_cast<int>(index));
        if (JS_IsException(function) || JS_DefinePropertyValueStr(jsContext, result.value, names.at(index), function, JS_PROP_C_W_E) < 0) {
            return exceptionValue;
        }
    }
    return result.release();
}

} // namespace amj::javascript
