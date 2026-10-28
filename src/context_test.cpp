#include <chrono>
#include <gtest/gtest.h>
#include <stop_token>
#include <system_error>

#include "context.hpp"

TEST(Context, DefaultHasNoError) {
    const amj::Context ctx;
    EXPECT_FALSE(static_cast<bool>(ctx.error()));
}

TEST(Context, ExpiredDeadlineTimesOut) {
    const amj::Context ctx = {
        .cancellation = std::stop_token(),
        .deadline = std::chrono::steady_clock::now(),
    };
    EXPECT_EQ(ctx.error(), std::make_error_code(std::errc::timed_out));
}

TEST(Context, CancellationTakesPrecedenceOverTimeout) {
    std::stop_source stopSource;
    const amj::Context ctx = {
        .cancellation = stopSource.get_token(),
        .deadline = std::chrono::steady_clock::now(),
    };
    stopSource.request_stop();
    EXPECT_EQ(ctx.error(), std::make_error_code(std::errc::operation_canceled));
}
