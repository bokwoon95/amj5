#include "context.hpp"

#include "errors.hpp"

#include <chrono>
#include <optional>
#include <stop_token>
#include <system_error>

namespace amj {

std::optional<Error> Context::errorOption() const {
    if (cancellation.stop_requested()) {
        return Error{
            .errorCode = std::make_error_code(std::errc::operation_canceled),
            .errorMessage = "context canceled",
        };
    }
    if (deadline.has_value() && std::chrono::steady_clock::now() >= deadline.value()) {
        return Error{
            .errorCode = std::make_error_code(std::errc::timed_out),
            .errorMessage = "context deadline exceeded",
        };
    }
    return std::nullopt;
}

} // namespace amj
