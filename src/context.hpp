#pragma once

#include <chrono>
#include <optional>
#include <stop_token>

#include "errors.hpp"

namespace amj {

// Copies share cancellation state through the token; deadlines are copied.
// A default context has no cancellation source and no deadline.
// This is cooperative: operations must explicitly observe the context.
struct Context {
    std::stop_token cancellation = std::stop_token();
    std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt;

    // nullopt means still active. Cancellation takes precedence over timeout.
    // Uses std::errc::operation_canceled and std::errc::timed_out codes.
    [[nodiscard]] std::optional<Error> error() const;
};

} // namespace amj
