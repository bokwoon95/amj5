#pragma once

#include <any>
#include <memory>
#include <source_location>
#include <string>
#include <system_error>
#include <vector>

namespace amj {

// All modules share this type. Module-specific enums use their own error
// categories; detail may hold any copyable module-specific error struct.
// Treat published errors and their cause chains as immutable and acyclic.
struct Error {
    std::error_code errorCode = std::error_code();
    std::string errorMessage = "";
    std::any detail = std::any();
    std::shared_ptr<const Error> cause = nullptr;

    // Empty unless explicitly captured for an unexpected failure.
    // Stored as text so the public type does not depend on stacktrace support.
    std::vector<std::string> stacktrace = {};
};

// Searches every error in the cause chain for an exact code match.
[[nodiscard]] bool matchesError(const Error& error, std::error_code code);

// Returns a borrowed pointer, valid only while the corresponding detail remains
// alive and unchanged. A missing detail type returns nullptr.
template <typename T>
[[nodiscard]] const T* findErrorDetail(const Error& error) {
    for (const Error* current = &error; current != nullptr; current = current->cause.get()) {
        const T* detail = std::any_cast<T>(&current->detail);
        if (detail != nullptr) {
            return detail;
        }
    }
    return nullptr;
}

// Includes contextual messages, underlying failures, and any captured trace.
[[nodiscard]] std::string formatError(const Error& error);

// Captures at most 30 frames; does nothing if the chain already has a trace.
// Call at the origin of an unexpected failure, not at every propagation site.
// Without standard-library stacktrace support, records only the caller's
// source location instead of a full stack. Allocation/formatting may throw.
[[nodiscard]] Error withStacktrace(Error error, std::source_location location = std::source_location::current());

} // namespace amj
