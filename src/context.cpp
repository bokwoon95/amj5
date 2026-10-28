#include "context.hpp"

#include <chrono>
#include <optional>
#include <stop_token>
#include <system_error>

#include "errors.hpp"

namespace amj {

std::optional<amj::Error> Context::errorOption() const {
    if (cancellation.stop_requested()) {
        return amj::Error{
            .errorCode = std::make_error_code(std::errc::operation_canceled),
            .errorMessage = "context canceled",
        };
    }
    if (deadline.has_value() && std::chrono::steady_clock::now() >= deadline.value()) {
        return amj::Error{
            .errorCode = std::make_error_code(std::errc::timed_out),
            .errorMessage = "context deadline exceeded",
        };
    }
    return std::nullopt;
}

} // namespace amj
