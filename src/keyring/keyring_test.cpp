#include "keyring.hpp"

#include "errors.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <expected>
#include <iostream>
#include <random>
#include <string>
#include <system_error>
#include <vector>

namespace amj::keyring {
namespace {

// A skipped operation must exit the test itself, not just a helper function.
#define ASSERT_KEYRING_SUCCESS(result)                                                                 \
    do {                                                                                              \
        if (!(result).has_value()) {                                                                  \
            if ((result).error().is(ErrorCode::AccessDenied) || (result).error().is(ErrorCode::Unavailable)) { \
                GTEST_SKIP() << #result << ": " << (result).error().toString();                          \
            }                                                                                         \
            FAIL() << #result << ": " << (result).error().toString();                                    \
        }                                                                                             \
    } while (false)

TEST(KeyringErrors, CodesHaveStableCategoryAndMessages) {
    const std::array<ErrorCode, 5> errorCodes = {ErrorCode::GenericError, ErrorCode::NotFound, ErrorCode::Unavailable, ErrorCode::SecretTooLong, ErrorCode::AccessDenied};
    const std::array<std::string, 5> messages = {"Credential store operation failed", "Secret not found", "Credential store unavailable", "Secret exceeds credential store capacity", "Credential store access denied"};
    for (std::size_t index = 0; index < errorCodes.size(); ++index) {
        const std::error_code errorCode = make_error_code(errorCodes.at(index));
        EXPECT_STREQ(errorCode.category().name(), "amj.keyring");
        EXPECT_EQ(errorCode.value(), static_cast<int>(errorCodes.at(index)));
        EXPECT_EQ(errorCode.message(), messages.at(index));
        EXPECT_EQ(&errorCode.category(), &make_error_code(ErrorCode::GenericError).category());
        const amj::Error error = {
            .errorCode = errorCode,
        };
        EXPECT_TRUE(error.is(errorCodes.at(index)));
        EXPECT_NE(errorCode, std::make_error_code(std::errc::permission_denied));
    }
    EXPECT_EQ(make_error_code(ErrorCode::GenericError).category().message(255), "Unknown keyring error");
}

class KeyringIntegration : public testing::Test {
  protected:
    std::string service = "";
    std::vector<std::string> services = {};
    const std::array<std::string, 2> users = {"test-user", "test-user-\xE7\x94\xA8\xE6\x88\xB7"};

    void SetUp() override {
        const char* enabled = std::getenv("AMJ_KEYRING_TESTS");
        if (enabled == nullptr || std::string(enabled) != "1") {
            GTEST_SKIP() << "Set AMJ_KEYRING_TESTS=1 to access the real OS credential store (may prompt/block).";
        }
        std::random_device randomDevice;
        service = "amj.keyring.tests." + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "." + std::to_string(randomDevice()) + "." + std::to_string(randomDevice());
        std::cerr << "Keyring integration test: OS authorization may be required; waiting for native calls. Service: " << service << '\n';
        const std::expected<bool, amj::Error> availableResult = isAvailable();
        ASSERT_KEYRING_SUCCESS(availableResult);
        if (!availableResult.value()) {
            GTEST_SKIP() << "OS credential store is unavailable.";
        }
        // Register identities before writes so cleanup also runs after assertion failures.
        services = {service, service + ".\xE6\x9C\x8D\xE5\x8A\xA1"};
    }

    void TearDown() override {
        for (const std::string& entryService : services) {
            for (const std::string& user : users) {
                const std::expected<void, amj::Error> removeResult = remove(entryService, user);
                if (removeResult.has_value() || removeResult.error().is(ErrorCode::NotFound)) {
                    continue;
                }
                std::cerr << "Keyring cleanup failed; remove this test entry manually: service=" << entryService << ", user=" << user << ": " << removeResult.error().toString() << '\n';
                if (!removeResult.error().is(ErrorCode::AccessDenied) && !removeResult.error().is(ErrorCode::Unavailable)) {
                    ADD_FAILURE() << "Unexpected keyring cleanup error: " << removeResult.error().toString();
                }
            }
        }
    }
};

TEST_F(KeyringIntegration, MissingEntriesReportNotFound) {
    const std::expected<std::string, amj::Error> loadResult = load(service, users.at(0));
    if (!loadResult.has_value() && (loadResult.error().is(ErrorCode::AccessDenied) || loadResult.error().is(ErrorCode::Unavailable))) {
        GTEST_SKIP() << loadResult.error().toString();
    }
    ASSERT_FALSE(loadResult.has_value());
    EXPECT_TRUE(loadResult.error().is(ErrorCode::NotFound)) << loadResult.error().toString();
    const std::expected<void, amj::Error> removeResult = remove(service, users.at(0));
    if (!removeResult.has_value() && (removeResult.error().is(ErrorCode::AccessDenied) || removeResult.error().is(ErrorCode::Unavailable))) {
        GTEST_SKIP() << removeResult.error().toString();
    }
    ASSERT_FALSE(removeResult.has_value());
    EXPECT_TRUE(removeResult.error().is(ErrorCode::NotFound)) << removeResult.error().toString();
}

TEST_F(KeyringIntegration, RoundTripOverwriteEmptyUnicodeAndSizes) {
    // Exercise both sides of Windows' direct-blob limit, multiple encrypted
    // attributes, and large-to-small/empty updates without assuming OS storage format.
    const std::vector<std::string> values = {"original", "replacement-\xE5\xAF\x86\xE7\xA0\x81", "", std::string(2560, 'a'), std::string(2561, 'b'), std::string(12000, 'c'), "small-again", ""};
    for (const std::string& value : values) {
        SCOPED_TRACE("Secret size: " + std::to_string(value.size()));
        const std::expected<void, amj::Error> saveResult = save(service, users.at(0), value);
        ASSERT_KEYRING_SUCCESS(saveResult);
        const std::expected<std::string, amj::Error> loadResult = load(service, users.at(0));
        ASSERT_KEYRING_SUCCESS(loadResult);
        // Do not print credential contents on mismatch.
        EXPECT_TRUE(loadResult.value() == value) << "Loaded secret differs from saved bytes";
    }
    const std::expected<void, amj::Error> removeResult = remove(service, users.at(0));
    ASSERT_KEYRING_SUCCESS(removeResult);
    const std::expected<std::string, amj::Error> loadResult = load(service, users.at(0));
    if (!loadResult.has_value() && (loadResult.error().is(ErrorCode::AccessDenied) || loadResult.error().is(ErrorCode::Unavailable))) {
        GTEST_SKIP() << loadResult.error().toString();
    }
    ASSERT_FALSE(loadResult.has_value());
    EXPECT_TRUE(loadResult.error().is(ErrorCode::NotFound)) << loadResult.error().toString();
}

TEST_F(KeyringIntegration, ServicesAndUsersAreIndependent) {
    int valueIndex = 0;
    for (const std::string& entryService : services) {
        for (const std::string& user : users) {
            const std::expected<void, amj::Error> saveResult = save(entryService, user, "value-" + std::to_string(valueIndex));
            ASSERT_KEYRING_SUCCESS(saveResult);
            ++valueIndex;
        }
    }
    const std::expected<void, amj::Error> overwriteResult = save(service, users.at(0), "changed");
    ASSERT_KEYRING_SUCCESS(overwriteResult);
    valueIndex = 0;
    for (const std::string& entryService : services) {
        for (const std::string& user : users) {
            const std::expected<std::string, amj::Error> loadResult = load(entryService, user);
            ASSERT_KEYRING_SUCCESS(loadResult);
            std::string expectedValue = "value-" + std::to_string(valueIndex);
            if (valueIndex == 0) {
                expectedValue = "changed";
            }
            EXPECT_TRUE(loadResult.value() == expectedValue);
            ++valueIndex;
        }
    }
}

// Linux explicitly documents UTF-8, NUL-terminated text rather than binary secrets.
#if defined(_WIN32) || defined(__APPLE__)
TEST_F(KeyringIntegration, EmbeddedNulBytesRoundTrip) {
    const std::string value("before\0after\0", 13);
    const std::expected<void, amj::Error> saveResult = save(service, users.at(0), value);
    ASSERT_KEYRING_SUCCESS(saveResult);
    const std::expected<std::string, amj::Error> loadResult = load(service, users.at(0));
    ASSERT_KEYRING_SUCCESS(loadResult);
    EXPECT_TRUE(loadResult.value() == value);
}
#endif

#if defined(_WIN32)
TEST_F(KeyringIntegration, OversizedWritePreservesExistingSecret) {
    const std::expected<void, amj::Error> saveResult = save(service, users.at(0), "original");
    ASSERT_KEYRING_SUCCESS(saveResult);
    // DPAPI overhead means even plaintext at the ciphertext ceiling cannot fit.
    for (const std::size_t size : {18944U, 18945U}) {
        const std::expected<void, amj::Error> oversizedResult = save(service, users.at(0), std::string(size, 'x'));
        ASSERT_FALSE(oversizedResult.has_value());
        EXPECT_TRUE(oversizedResult.error().is(ErrorCode::SecretTooLong)) << oversizedResult.error().toString();
        const std::expected<std::string, amj::Error> loadResult = load(service, users.at(0));
        ASSERT_KEYRING_SUCCESS(loadResult);
        EXPECT_TRUE(loadResult.value() == "original");
    }
}
#endif

#undef ASSERT_KEYRING_SUCCESS

} // namespace
} // namespace amj::keyring
