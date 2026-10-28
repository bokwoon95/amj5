#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <system_error>

#include "errors.hpp"

TEST(Errors, DefaultHasNoCode) {
    const amj::Error error;
    EXPECT_FALSE(static_cast<bool>(error.errorCode));
}

TEST(Errors, MatchesCauseWithoutOuterCode) {
    const std::error_code errorCode = std::make_error_code(std::errc::invalid_argument);
    const amj::Error cause = {
        .errorCode = errorCode,
        .errorMessage = "invalid input",
    };
    const amj::Error error = {
        .errorMessage = "operation failed",
        .cause = std::make_shared<const amj::Error>(cause),
    };
    EXPECT_FALSE(static_cast<bool>(error.errorCode));
    ASSERT_NE(error.cause, nullptr);
    EXPECT_EQ(error.cause->errorMessage, "invalid input");
    EXPECT_TRUE(amj::matchesError(error, errorCode));
    EXPECT_FALSE(amj::matchesError(error, std::make_error_code(std::errc::timed_out)));
    EXPECT_EQ(amj::formatError(error), "operation failed: invalid input");
}
