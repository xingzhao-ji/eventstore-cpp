#pragma once
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <sys/types.h>

namespace eventstore {
inline constexpr std::size_t max_category_bytes = 64;
inline constexpr std::size_t max_payload_bytes = 1024 * 1024;
inline constexpr std::size_t max_group_count = 100000;
struct Input {
    std::int64_t timestamp;
    std::string category;
    std::int64_t value;
    std::string payload;
};
struct Event {
    std::uint64_t id;
    std::int64_t timestamp;
    std::string category;
    std::int64_t value;
    std::string payload;
    bool operator==(const Event&) const = default;
};
struct Query {
    std::int64_t from = std::numeric_limits<std::int64_t>::min();
    std::int64_t to = std::numeric_limits<std::int64_t>::max();
    std::optional<std::string> category;
};
enum class Access { ReadOnly, ReadWrite };
// POSIX local files only. Lock held for lifetime; calls serialize on one object.
// Visitors must not re-enter the Store.
class Store final {
public:
    explicit Store(const std::string& path, Access access = Access::ReadWrite);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    Event append(const Input& input);
    void query(const Query& filter, const std::function<void(const Event&)>& visitor) const;
    std::map<std::string, std::uint64_t> counts(const Query& filter) const;
    std::uint64_t recovered_bytes() const noexcept { return recovered_; }
private:
    void require_healthy() const;
    int fd_ = -1;
    bool writable_;
    bool poisoned_ = false;
    off_t end_ = 16;
    std::uint64_t last_id_ = 0;
    std::uint64_t recovered_ = 0;
    mutable std::mutex mutex_;
};
std::string to_json(const Event& event);
std::string json_string(const std::string& text);
} // namespace eventstore
