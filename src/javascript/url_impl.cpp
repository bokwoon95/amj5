#include "url_impl.hpp"

#include "javascript.hpp"

#include <ada.h> // IWYU pragma: keep
#include <ada/implementation.h>
#include <ada/url_aggregator.h>
#include <ada/url_search_params.h>
#include <quickjs.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace amj::javascript {
namespace {

constexpr std::size_t inputLimit = 1048576;
constexpr int64_t pairLimit = 65536;

} // namespace

JSValue urlParse(JSContext* jsContext, JSValueConst /* thisValue */, int argc, JSValueConst* argv) try {
    if (argc < 1) {
        return JS_ThrowTypeError(jsContext, "urlParse: requires 1 argument (%d provided)", argc);
    }
    if (!JS_IsString(argv[0])) {
        return JS_ThrowTypeError(jsContext, "urlParse: arg0: input must be a string");
    }
    const std::optional<std::string> inputText = jsValueToString(jsContext, argv[0]);
    if (!inputText.has_value()) {
        return JS_EXCEPTION;
    }
    if (inputText.value().size() > inputLimit) {
        return JS_ThrowRangeError(jsContext, "urlParse: arg0: input exceeds 1 MB");
    }
    ada::result<ada::url_aggregator> parsed;
    if (argc < 2 || JS_IsUndefined(argv[1])) {
        parsed = ada::parse<ada::url_aggregator>(inputText.value(), nullptr);
    } else {
        if (!JS_IsString(argv[1])) {
            return JS_ThrowTypeError(jsContext, "urlParse: arg1: base must be a string or undefined");
        }
        const std::optional<std::string> baseText = jsValueToString(jsContext, argv[1]);
        if (!baseText.has_value()) {
            return JS_EXCEPTION;
        }
        if (baseText.value().size() > inputLimit) {
            return JS_ThrowRangeError(jsContext, "urlParse: arg1: base exceeds 1 MB");
        }
        const ada::result<ada::url_aggregator> base = ada::parse<ada::url_aggregator>(baseText.value(), nullptr);
        if (!base.has_value()) {
            return JS_NULL;
        }
        parsed = ada::parse<ada::url_aggregator>(inputText.value(), &base.value());
    }
    if (!parsed.has_value()) {
        return JS_NULL;
    }
    if (parsed.value().get_href().size() > inputLimit) {
        return JS_ThrowRangeError(jsContext, "urlParse: result exceeds 1 MB");
    }
    const std::string_view search = parsed.value().get_search();
    if (std::count(search.begin(), search.end(), '&') >= pairLimit) {
        return JS_ThrowRangeError(jsContext, "urlParse: result.search exceeds 65536 pairs");
    }
    const ada::url_aggregator& url = parsed.value();
    return jsonToJSValue(jsContext, JSONValue(std::map<std::string, JSONValue>{
        {"href", std::string(url.get_href())},
        {"origin", url.get_origin()},
        {"protocol", std::string(url.get_protocol())},
        {"username", std::string(url.get_username())},
        {"password", std::string(url.get_password())},
        {"host", std::string(url.get_host())},
        {"hostname", std::string(url.get_hostname())},
        {"port", std::string(url.get_port())},
        {"pathname", std::string(url.get_pathname())},
        {"search", std::string(url.get_search())},
        {"hash", std::string(url.get_hash())},
    }));
} catch (const std::bad_alloc&) {
    return JS_ThrowOutOfMemory(jsContext);
} catch (const std::exception& exception) {
    return JS_ThrowInternalError(jsContext, "urlParse: %s", exception.what());
} catch (...) {
    return JS_ThrowInternalError(jsContext, "urlParse: unexpected native failure");
}

JSValue urlWithComponent(JSContext* jsContext, JSValueConst /* thisValue */, int argc, JSValueConst* argv) try {
    if (argc < 3) {
        return JS_ThrowTypeError(jsContext, "urlWithComponent: requires 3 arguments (%d provided)", argc);
    }
    if (!JS_IsString(argv[0])) {
        return JS_ThrowTypeError(jsContext, "urlWithComponent: arg0: href must be a string");
    }
    const std::optional<std::string> hrefText = jsValueToString(jsContext, argv[0]);
    if (!hrefText.has_value()) {
        return JS_EXCEPTION;
    }
    if (hrefText.value().size() > inputLimit) {
        return JS_ThrowRangeError(jsContext, "urlWithComponent: arg0: href exceeds 1 MB");
    }
    if (!JS_IsString(argv[1])) {
        return JS_ThrowTypeError(jsContext, "urlWithComponent: arg1: component must be a string");
    }
    const std::optional<std::string> componentText = jsValueToString(jsContext, argv[1]);
    if (!componentText.has_value()) {
        return JS_EXCEPTION;
    }
    if (componentText.value().size() > inputLimit) {
        return JS_ThrowRangeError(jsContext, "urlWithComponent: arg1: component exceeds 1 MB");
    }
    if (!JS_IsString(argv[2])) {
        return JS_ThrowTypeError(jsContext, "urlWithComponent: arg2: value must be a string");
    }
    const std::optional<std::string> valueText = jsValueToString(jsContext, argv[2]);
    if (!valueText.has_value()) {
        return JS_EXCEPTION;
    }
    if (valueText.value().size() > inputLimit) {
        return JS_ThrowRangeError(jsContext, "urlWithComponent: arg2: value exceeds 1 MB");
    }
    ada::result<ada::url_aggregator> parsed = ada::parse<ada::url_aggregator>(hrefText.value(), nullptr);
    if (!parsed.has_value()) {
        return JS_NULL;
    }
    ada::url_aggregator& url = parsed.value();
    bool accepted = true;
    if (componentText.value() == "href") {
        accepted = url.set_href(valueText.value());
    } else if (componentText.value() == "protocol") {
        accepted = url.set_protocol(valueText.value());
    } else if (componentText.value() == "username") {
        accepted = url.set_username(valueText.value());
    } else if (componentText.value() == "password") {
        accepted = url.set_password(valueText.value());
    } else if (componentText.value() == "host") {
        accepted = url.set_host(valueText.value());
    } else if (componentText.value() == "hostname") {
        accepted = url.set_hostname(valueText.value());
    } else if (componentText.value() == "port") {
        accepted = url.set_port(valueText.value());
    } else if (componentText.value() == "pathname") {
        accepted = url.set_pathname(valueText.value());
    } else if (componentText.value() == "search") {
        url.set_search(valueText.value());
    } else if (componentText.value() == "hash") {
        url.set_hash(valueText.value());
    } else {
        return JS_ThrowTypeError(jsContext, "urlWithComponent: arg1: unknown URL component");
    }
    if (!accepted) {
        return JS_NULL;
    }
    if (url.get_href().size() > inputLimit) {
        return JS_ThrowRangeError(jsContext, "urlWithComponent: result exceeds 1 MB");
    }
    const std::string_view search = url.get_search();
    if (std::count(search.begin(), search.end(), '&') >= pairLimit) {
        return JS_ThrowRangeError(jsContext, "urlWithComponent: result.search exceeds 65536 pairs");
    }
    return jsonToJSValue(jsContext, JSONValue(std::map<std::string, JSONValue>{
        {"href", std::string(url.get_href())},
        {"origin", url.get_origin()},
        {"protocol", std::string(url.get_protocol())},
        {"username", std::string(url.get_username())},
        {"password", std::string(url.get_password())},
        {"host", std::string(url.get_host())},
        {"hostname", std::string(url.get_hostname())},
        {"port", std::string(url.get_port())},
        {"pathname", std::string(url.get_pathname())},
        {"search", std::string(url.get_search())},
        {"hash", std::string(url.get_hash())},
    }));
} catch (const std::bad_alloc&) {
    return JS_ThrowOutOfMemory(jsContext);
} catch (const std::exception& exception) {
    return JS_ThrowInternalError(jsContext, "urlWithComponent: %s", exception.what());
} catch (...) {
    return JS_ThrowInternalError(jsContext, "urlWithComponent: unexpected native failure");
}

JSValue queryStringParse(JSContext* jsContext, JSValueConst /* thisValue */, int argc, JSValueConst* argv) try {
    if (argc < 1) {
        return JS_ThrowTypeError(jsContext, "queryStringParse: requires 1 argument (%d provided)", argc);
    }
    if (!JS_IsString(argv[0])) {
        return JS_ThrowTypeError(jsContext, "queryStringParse: arg0: queryString must be a string");
    }
    const std::optional<std::string> text = jsValueToString(jsContext, argv[0]);
    if (!text.has_value()) {
        return JS_EXCEPTION;
    }
    if (text.value().size() > inputLimit) {
        return JS_ThrowRangeError(jsContext, "queryStringParse: arg0: queryString exceeds 1 MB");
    }
    if (std::count(text.value().begin(), text.value().end(), '&') >= pairLimit) {
        return JS_ThrowRangeError(jsContext, "queryStringParse: arg0: queryString exceeds 65536 pairs");
    }
    const ada::url_search_params searchParams(text.value());
    std::vector<JSONValue> pairs;
    for (const std::pair<std::string, std::string>& pair : searchParams) {
        pairs.push_back(JSONValue(std::vector<JSONValue>{pair.first, pair.second}));
    }
    return jsonToJSValue(jsContext, JSONValue(pairs));
} catch (const std::bad_alloc&) {
    return JS_ThrowOutOfMemory(jsContext);
} catch (const std::exception& exception) {
    return JS_ThrowInternalError(jsContext, "queryStringParse: %s", exception.what());
} catch (...) {
    return JS_ThrowInternalError(jsContext, "queryStringParse: unexpected native failure");
}

JSValue queryStringify(JSContext* jsContext, JSValueConst /* thisValue */, int argc, JSValueConst* argv) try {
    if (argc < 1) {
        return JS_ThrowTypeError(jsContext, "queryStringify: requires 1 argument (%d provided)", argc);
    }
    const JSValueConst pairs = argv[0];
    if (!JS_IsArray(pairs)) {
        return JS_ThrowTypeError(jsContext, "queryStringify: arg0: pairs must be an array");
    }
    int64_t count = 0;
    if (JS_GetLength(jsContext, pairs, &count) < 0) {
        return JS_EXCEPTION;
    }
    if (count < 0 || count > pairLimit) {
        return JS_ThrowRangeError(jsContext, "queryStringify: arg0: pairs exceeds 65536 pairs");
    }
    ada::url_search_params searchParams;
    std::size_t bytes = 0;
    for (int64_t index = 0; index < count; index++) {
        const AutoJSValue pair(jsContext, JS_GetPropertyUint32(jsContext, pairs, static_cast<uint32_t>(index)));
        if (JS_IsException(pair.value)) {
            return JS_EXCEPTION;
        }
        if (!JS_IsArray(pair.value)) {
            return JS_ThrowTypeError(jsContext, "queryStringify: arg0: pairs[%lld] must be an array", static_cast<long long>(index));
        }
        int64_t length = 0;
        if (JS_GetLength(jsContext, pair.value, &length) < 0) {
            return JS_EXCEPTION;
        }
        if (length != 2) {
            return JS_ThrowTypeError(jsContext, "queryStringify: arg0: pairs[%lld] must contain 2 elements (%lld provided)", static_cast<long long>(index), static_cast<long long>(length));
        }
        const AutoJSValue name(jsContext, JS_GetPropertyUint32(jsContext, pair.value, 0));
        if (JS_IsException(name.value)) {
            return JS_EXCEPTION;
        }
        if (!JS_IsString(name.value)) {
            return JS_ThrowTypeError(jsContext, "queryStringify: arg0: pairs[%lld][0] must be a string", static_cast<long long>(index));
        }
        const std::optional<std::string> nameText = jsValueToString(jsContext, name.value);
        if (!nameText.has_value()) {
            return JS_EXCEPTION;
        }
        if (nameText.value().size() > inputLimit) {
            return JS_ThrowRangeError(jsContext, "queryStringify: arg0: pairs[%lld][0] exceeds 1 MB", static_cast<long long>(index));
        }
        const AutoJSValue value(jsContext, JS_GetPropertyUint32(jsContext, pair.value, 1));
        if (JS_IsException(value.value)) {
            return JS_EXCEPTION;
        }
        if (!JS_IsString(value.value)) {
            return JS_ThrowTypeError(jsContext, "queryStringify: arg0: pairs[%lld][1] must be a string", static_cast<long long>(index));
        }
        const std::optional<std::string> valueText = jsValueToString(jsContext, value.value);
        if (!valueText.has_value()) {
            return JS_EXCEPTION;
        }
        if (valueText.value().size() > inputLimit) {
            return JS_ThrowRangeError(jsContext, "queryStringify: arg0: pairs[%lld][1] exceeds 1 MB", static_cast<long long>(index));
        }
        for (const std::string* field : {&nameText.value(), &valueText.value()}) {
            if (field->size() > inputLimit - bytes) {
                return JS_ThrowRangeError(jsContext, "queryStringify: arg0: pairs exceeds 1 MB");
            }
            bytes += field->size();
        }
        searchParams.append(nameText.value(), valueText.value());
    }
    const std::string encoded = searchParams.to_string();
    if (encoded.size() > inputLimit) {
        return JS_ThrowRangeError(jsContext, "queryStringify: result exceeds 1 MB");
    }
    return jsonToJSValue(jsContext, JSONValue(encoded));
} catch (const std::bad_alloc&) {
    return JS_ThrowOutOfMemory(jsContext);
} catch (const std::exception& exception) {
    return JS_ThrowInternalError(jsContext, "queryStringify: %s", exception.what());
} catch (...) {
    return JS_ThrowInternalError(jsContext, "queryStringify: unexpected native failure");
}

JSModuleDef* createURLImplModule(JSContext* jsContext) {
    JSModuleDef* module = JS_NewCModule(jsContext, "amj:url_impl", [](JSContext* jsContext, JSModuleDef* module) -> int {
        const JSValue parseFunction = JS_NewCFunction(jsContext, urlParse, "urlParse", 1);
        if (JS_IsException(parseFunction) || JS_SetModuleExport(jsContext, module, "urlParse", parseFunction) < 0) {
            return -1;
        }
        const JSValue withComponentFunction = JS_NewCFunction(jsContext, urlWithComponent, "urlWithComponent", 3);
        if (JS_IsException(withComponentFunction) || JS_SetModuleExport(jsContext, module, "urlWithComponent", withComponentFunction) < 0) {
            return -1;
        }
        const JSValue queryParseFunction = JS_NewCFunction(jsContext, queryStringParse, "queryStringParse", 1);
        if (JS_IsException(queryParseFunction) || JS_SetModuleExport(jsContext, module, "queryStringParse", queryParseFunction) < 0) {
            return -1;
        }
        const JSValue queryStringifyFunction = JS_NewCFunction(jsContext, queryStringify, "queryStringify", 1);
        if (JS_IsException(queryStringifyFunction) || JS_SetModuleExport(jsContext, module, "queryStringify", queryStringifyFunction) < 0) {
            return -1;
        }
        return 0;
    });
    if (module == nullptr) {
        return nullptr;
    }
    if (JS_AddModuleExport(jsContext, module, "urlParse") < 0 ||
        JS_AddModuleExport(jsContext, module, "urlWithComponent") < 0 ||
        JS_AddModuleExport(jsContext, module, "queryStringParse") < 0 ||
        JS_AddModuleExport(jsContext, module, "queryStringify") < 0) {
        return nullptr;
    }
    return module;
}

} // namespace amj::javascript
