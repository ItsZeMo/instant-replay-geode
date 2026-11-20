#pragma once
// Minimal stand-in for Geode's Objective-C method hooks (same semantics as the iOS / macOS loader:
// method_setImplementation on the class' instance method, returning the previous IMP).
#include <objc/runtime.h>
#include <string>

namespace geode::hook {

struct MockImpResult {
    void* value = nullptr;
    std::string error;
    explicit operator bool() const noexcept { return error.empty(); }
    void* unwrap() const noexcept { return value; }
    const std::string& unwrapErr() const noexcept { return error; }
};

inline MockImpResult getObjcMethodImp(char const* className, char const* selectorName) {
    Class cls = objc_getClass(className);
    if (!cls) return {nullptr, "Class not found"};
    Method method = class_getInstanceMethod(cls, sel_registerName(selectorName));
    if (!method) return {nullptr, "Method not found"};
    return {reinterpret_cast<void*>(method_getImplementation(method)), {}};
}

inline MockImpResult replaceObjcMethod(char const* className, char const* selectorName, void* imp) {
    Class cls = objc_getClass(className);
    if (!cls) return {nullptr, "Class not found"};
    Method method = class_getInstanceMethod(cls, sel_registerName(selectorName));
    if (!method) return {nullptr, "Method not found"};
    return {reinterpret_cast<void*>(method_setImplementation(method, reinterpret_cast<IMP>(imp))), {}};
}

} // namespace geode::hook
