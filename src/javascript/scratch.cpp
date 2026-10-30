#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <quickjs.h>
#include <set>

namespace scratch {
namespace chrono = std::chrono;

struct ScheduledCallback {
    std::function<void()> callback = {};
    std::function<void()> cancel = {};
    std::optional<chrono::steady_clock::time_point> deadline = {};
};

struct CallbackScheduler {
    explicit CallbackScheduler(JSContext* jsContext);
    ~CallbackScheduler();

    std::set<int64_t> set = {};

    void wait() {
    };
};

} // namespace scratch
