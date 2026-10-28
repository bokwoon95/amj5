#include "errors.hpp"

#include <cstdint>
#include <memory>
#include <stacktrace>
#include <string>
#include <system_error>
#include <tuple>

namespace amj {

bool Error::is(std::error_code targetErrorCode) const {
    return as(targetErrorCode) != nullptr;
}

const Error* Error::as(std::error_code targetErrorCode) const {
    for (const Error* current = this; current != nullptr; current = current->cause.get()) {
        if (current->errorCode == targetErrorCode) {
            return current;
        }
    }
    return nullptr;
}

std::string Error::toString() const {
    std::string text;
    for (const Error* current = this; current != nullptr; current = current->cause.get()) {
        if (!text.empty()) {
            text += ": ";
        }
        if (!current->errorMessage.empty()) {
            text += current->errorMessage;
        } else {
            text += current->errorCode.message();
        }
    }
    for (const Error* current = this; current != nullptr; current = current->cause.get()) {
        for (const std::tuple<std::string, std::uint_least32_t, std::string>& frame : current->stacktrace) {
            const std::string& file = std::get<0>(frame);
            const std::uint_least32_t line = std::get<1>(frame);
            const std::string& description = std::get<2>(frame);
            text += "\n  at " + file + ":" + std::to_string(line);
            if (!description.empty()) {
                text += " " + description;
            }
        }
    }
    return text;
}

Error withStackTrace(Error error) {
    for (const Error* current = &error; current != nullptr; current = current->cause.get()) {
        if (!current->stacktrace.empty()) {
            return error;
        }
    }
    for (const std::stacktrace_entry& frame : std::stacktrace::current(1, 30)) {
        error.stacktrace.emplace_back(frame.source_file(), frame.source_line(), frame.description());
    }
    return error;
}

} // namespace amj
