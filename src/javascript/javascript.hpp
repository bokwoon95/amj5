#pragma once

#include <quickjs.h>

namespace amj::javascript {

// Owns one QuickJS reference. The context must outlive this wrapper and all copies.
// Use only on the context's owner thread; reference counting is not atomic.
class RefCountedJSValue {
  public:
    RefCountedJSValue() = default;
    // Adopts an existing owned reference without incrementing its reference count.
    // jsContext must be valid, including when value is a primitive.
    RefCountedJSValue(JSContext* jsContext, JSValue value);
    ~RefCountedJSValue();
    RefCountedJSValue(const RefCountedJSValue& other);
    RefCountedJSValue& operator=(const RefCountedJSValue& other);
    RefCountedJSValue(RefCountedJSValue&& other) noexcept;
    RefCountedJSValue& operator=(RefCountedJSValue&& other) noexcept;

    // Creates an owned reference from a borrowed value by incrementing its count.
    static RefCountedJSValue fromBorrowed(JSContext* jsContext, JSValueConst value);
    JSContext* getContext() const;
    // Returns a borrowed value; ownership remains with this wrapper.
    JSValue getValue() const;
    // Transfers the owned reference to the caller, leaving this wrapper empty.
    JSValue release();

  private:
    JSContext* jsContext = nullptr;
    JSValue value = JS_UNDEFINED;
};

} // namespace amj::javascript
