#include <chrono>
#include <gtest/gtest.h>
#include <optional>
#include <stop_token>
#include <system_error>

#include "context.hpp"
#include "errors.hpp"

namespace amj {
namespace {

TEST(Context, DefaultHasNoError) {
    const Context context;
    EXPECT_FALSE(context.errorOption().has_value());
}

TEST(Context, ExpiredDeadlineTimesOut) {
    const Context context = {
        .cancellation = std::stop_token(),
        .deadline = std::chrono::steady_clock::now(),
    };
    const std::optional<Error> errorOption = context.errorOption();
    if (!errorOption.has_value()) {
        FAIL() << "expected a context error";
        return;
    }
    const Error& error = errorOption.value();
    EXPECT_EQ(error.errorCode, std::make_error_code(std::errc::timed_out));
    EXPECT_EQ(error.errorMessage, "context deadline exceeded");
    EXPECT_TRUE(error.stacktrace.empty());
}

TEST(Context, CancellationTakesPrecedenceOverTimeout) {
    std::stop_source stopSource;
    const Context context = {
        .cancellation = stopSource.get_token(),
        .deadline = std::chrono::steady_clock::now(),
    };
    stopSource.request_stop();
    const std::optional<Error> errorOption = context.errorOption();
    if (!errorOption.has_value()) {
        FAIL() << "expected a context error";
        return;
    }
    const Error& error = errorOption.value();
    EXPECT_EQ(error.errorCode, std::make_error_code(std::errc::operation_canceled));
    EXPECT_EQ(error.errorMessage, "context canceled");
    EXPECT_TRUE(error.stacktrace.empty());
}

} // namespace
} // namespace amj
