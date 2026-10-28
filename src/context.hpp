#pragma once

#include <chrono>
#include <optional>
#include <stop_token>
#include <system_error>

namespace amj {

// Copies share cancellation state through the token; deadlines are copied.
// A default context has no cancellation source and no deadline.
// This is cooperative: operations must explicitly observe the context.
struct Context {
    std::stop_token cancellation;
    std::optional<std::chrono::steady_clock::time_point> deadline;

    // Empty code means no error. Cancellation takes precedence over timeout.
    // Uses std::errc::operation_canceled and std::errc::timed_out codes.
    [[nodiscard]] std::error_code errorCode() const;
};

} // namespace amj
