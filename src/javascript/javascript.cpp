#include "javascript.hpp"

#include <quickjs.h>

#include <stdexcept>
#include <utility>

namespace amj::javascript {

RefCountedJSValue::RefCountedJSValue(JSContext* jsContext, JSValue value) {
    if (jsContext == nullptr) {
        throw std::invalid_argument("RefCountedJSValue requires a valid context");
    }
    this->jsContext = jsContext;
    this->value = value;
}

RefCountedJSValue::~RefCountedJSValue() {
    if (jsContext != nullptr) {
        JS_FreeValue(jsContext, value);
    }
}

RefCountedJSValue::RefCountedJSValue(const RefCountedJSValue& other) {
    this->jsContext = other.jsContext;
    if (other.jsContext != nullptr) {
        this->value = JS_DupValue(other.jsContext, other.value);
    }
}

RefCountedJSValue& RefCountedJSValue::operator=(const RefCountedJSValue& other) {
    if (this == &other) {
        return *this;
    }
    JSValue duplicated = JS_UNDEFINED;
    if (other.jsContext != nullptr) {
        duplicated = JS_DupValue(other.jsContext, other.value);
    }
    if (this->jsContext != nullptr) {
        JS_FreeValue(this->jsContext, this->value);
    }
    this->jsContext = other.jsContext;
    this->value = duplicated;
    return *this;
}

RefCountedJSValue::RefCountedJSValue(RefCountedJSValue&& other) noexcept {
    this->jsContext = std::exchange(other.jsContext, nullptr);
    this->value = std::exchange(other.value, JS_UNDEFINED);
}

RefCountedJSValue& RefCountedJSValue::operator=(RefCountedJSValue&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    if (this->jsContext != nullptr) {
        JS_FreeValue(this->jsContext, this->value);
    }
    this->jsContext = std::exchange(other.jsContext, nullptr);
    this->value = std::exchange(other.value, JS_UNDEFINED);
    return *this;
}

RefCountedJSValue RefCountedJSValue::fromBorrowed(JSContext* jsContext, JSValueConst value) {
    if (jsContext == nullptr) {
        throw std::invalid_argument("RefCountedJSValue requires a valid context");
    }
    return RefCountedJSValue(jsContext, JS_DupValue(jsContext, value));
}

JSContext* RefCountedJSValue::getContext() const {
    return jsContext;
}

JSValue RefCountedJSValue::getValue() const {
    return value;
}

JSValue RefCountedJSValue::release() {
    return std::exchange(value, JS_UNDEFINED);
}

} // namespace amj::javascript
