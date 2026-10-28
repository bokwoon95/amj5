#include "errors.hpp"

#include <memory>
#include <source_location>
#include <string>
#include <system_error>
#include <utility>
#include <version> // IWYU pragma: keep (standard-library feature-test macros)

#if defined(_MSVC_STL_VERSION)
#include <yvals_core.h>
#endif

#if defined(__cpp_lib_stacktrace) && __cpp_lib_stacktrace >= 202011L
#include <sstream>
#include <stacktrace>
#endif

namespace amj {

Error wrapError(Error cause, std::string message) {
    const std::error_code errorCode = cause.errorCode;
    return Error{
        .errorCode = errorCode,
        .errorMessage = std::move(message),
        .cause = std::make_shared<const Error>(std::move(cause)),
    };
}

bool matchesError(const Error& error, std::error_code code) {
    for (const Error* current = &error; current != nullptr; current = current->cause.get()) {
        if (current->errorCode == code) {
            return true;
        }
    }
    return false;
}

std::string formatError(const Error& error) {
    std::string text;
    for (const Error* current = &error; current != nullptr; current = current->cause.get()) {
        if (!text.empty()) {
            text += ": ";
        }
        if (!current->errorMessage.empty()) {
            text += current->errorMessage;
        } else {
            text += current->errorCode.message();
        }
    }
    for (const Error* current = &error; current != nullptr; current = current->cause.get()) {
        for (const std::string& frame : current->stacktrace) {
            text += "\n  at ";
            text += frame;
        }
    }
    return text;
}

Error withStacktrace(Error error, std::source_location location) {
    for (const Error* current = &error; current != nullptr; current = current->cause.get()) {
        if (!current->stacktrace.empty()) {
            return error;
        }
    }

#if defined(__cpp_lib_stacktrace) && __cpp_lib_stacktrace >= 202011L
    for (const std::stacktrace_entry& frame : std::stacktrace::current(1, 30)) {
        std::ostringstream text;
        text << frame;
        error.stacktrace.push_back(text.str());
    }
#endif
    // Also handles failure to capture any native frames on supported platforms.
    if (error.stacktrace.empty()) {
        error.stacktrace.push_back(std::string(location.file_name()) + ":" + std::to_string(location.line()) + " " + location.function_name());
    }
    return error;
}

} // namespace amj
