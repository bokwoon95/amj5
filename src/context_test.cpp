#include <chrono>
#include <gtest/gtest.h>
#include <optional>
#include <stop_token>
#include <system_error>

#include "context.hpp"
#include "errors.hpp"

TEST(Context, DefaultHasNoError) {
    const amj::Context ctx;
    EXPECT_FALSE(ctx.errorOption().has_value());
}

TEST(Context, ExpiredDeadlineTimesOut) {
    const amj::Context ctx = {
        .cancellation = std::stop_token(),
        .deadline = std::chrono::steady_clock::now(),
    };
    const std::optional<amj::Error> errorOption = ctx.errorOption();
    if (!errorOption.has_value()) {
        FAIL() << "expected a context error";
        return;
    }
    const amj::Error& error = errorOption.value();
    EXPECT_EQ(error.errorCode, std::make_error_code(std::errc::timed_out));
    EXPECT_EQ(error.errorMessage, "context deadline exceeded");
    EXPECT_TRUE(error.stacktrace.empty());
}

TEST(Context, CancellationTakesPrecedenceOverTimeout) {
    std::stop_source stopSource;
    const amj::Context ctx = {
        .cancellation = stopSource.get_token(),
        .deadline = std::chrono::steady_clock::now(),
    };
    stopSource.request_stop();
    const std::optional<amj::Error> errorOption = ctx.errorOption();
    if (!errorOption.has_value()) {
        FAIL() << "expected a context error";
        return;
    }
    const amj::Error& error = errorOption.value();
    EXPECT_EQ(error.errorCode, std::make_error_code(std::errc::operation_canceled));
    EXPECT_EQ(error.errorMessage, "context canceled");
    EXPECT_TRUE(error.stacktrace.empty());
}
