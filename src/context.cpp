#include "context.hpp"

#include <chrono>
#include <optional>
#include <stop_token>
#include <system_error>

namespace amj {

std::error_code Context::errorCode() const {
    if (cancellation.stop_requested()) {
        return std::make_error_code(std::errc::operation_canceled);
    }
    if (deadline.has_value() && std::chrono::steady_clock::now() >= deadline.value()) {
        return std::make_error_code(std::errc::timed_out);
    }
    return std::error_code();
}

} // namespace amj
