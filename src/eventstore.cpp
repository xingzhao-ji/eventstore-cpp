#include "eventstore.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace eventstore {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t file_header_size = 16;
constexpr std::size_t frame_header_size = 16;
constexpr std::size_t fixed_body_size = 32;
constexpr std::size_t max_body_size = fixed_body_size + max_category_bytes + max_payload_bytes;
[[noreturn]] void io_error(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}
[[noreturn]] void corrupt(const char* detail) {
    throw std::runtime_error(std::string("corrupt event store: ") + detail);
}
std::uint32_t crc32(const std::uint8_t* data, std::size_t count) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> result{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            auto n = i;
            for (int bit = 0; bit < 8; ++bit) n = (n >> 1) ^ ((n & 1) ? 0xedb88320U : 0U);
            result[i] = n;
        }
        return result;
    }();
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < count; ++i) crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffU;
}
void put32(Bytes& out, std::uint32_t n) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(n >> (i * 8)));
}
void put64(Bytes& out, std::uint64_t n) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(n >> (i * 8)));
}
std::uint32_t get32(const std::uint8_t* p) {
    std::uint32_t n = 0;
    for (int i = 0; i < 4; ++i) n |= static_cast<std::uint32_t>(p[i]) << (i * 8);
    return n;
}
std::uint64_t get64(const std::uint8_t* p) {
    std::uint64_t n = 0;
    for (int i = 0; i < 8; ++i) n |= static_cast<std::uint64_t>(p[i]) << (i * 8);
    return n;
}
Bytes file_header() {
    Bytes bytes{'E', 'V', 'T', 'L', 'O', 'G', '0', '1'};
    put32(bytes, 1);
    put32(bytes, crc32(bytes.data(), bytes.size()));
    return bytes;
}
void read_all(int fd, std::uint8_t* data, std::size_t size, off_t offset) {
    std::size_t done = 0;
    while (done < size) {
        const auto n = ::pread(fd, data + done, size - done, offset + static_cast<off_t>(done));
        if (n < 0) { if (errno == EINTR) continue; io_error("pread"); }
        if (n == 0) throw std::runtime_error("file changed during read");
        done += static_cast<std::size_t>(n);
    }
}
void write_all(int fd, const Bytes& bytes, off_t offset) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        const auto n = ::pwrite(fd, bytes.data() + done, bytes.size() - done, offset + static_cast<off_t>(done));
        if (n < 0) { if (errno == EINTR) continue; io_error("pwrite"); }
        if (n == 0) throw std::runtime_error("pwrite made no progress");
        done += static_cast<std::size_t>(n);
    }
}
void sync_file(int fd) {
    while (::fsync(fd) != 0) { if (errno == EINTR) continue; io_error("fsync"); }
}
void truncate_synced(int fd, off_t size) {
    while (::ftruncate(fd, size) != 0) { if (errno == EINTR) continue; io_error("ftruncate"); }
    sync_file(fd);
}
void sync_parent(const std::string& path) {
    auto parent = std::filesystem::path(path).parent_path();
    if (parent.empty()) parent = ".";
    const int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) io_error("open parent directory");
    try { sync_file(fd); } catch (...) { ::close(fd); throw; }
    ::close(fd);
}
bool valid_utf8(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80) continue;
        unsigned remaining;
        std::uint32_t code, minimum;
        if (first >= 0xc2 && first <= 0xdf) { remaining = 1; code = first & 0x1f; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { remaining = 2; code = first & 0x0f; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { remaining = 3; code = first & 0x07; minimum = 0x10000; }
        else return false;
        if (remaining > text.size() - i) return false;
        for (unsigned j = 0; j < remaining; ++j) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) return false;
            code = (code << 6) | (next & 0x3f);
        }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}
void validate_category(const std::string& category) {
    if (category.empty() || category.size() > max_category_bytes || !valid_utf8(category))
        throw std::invalid_argument("category must be 1..64 bytes of valid UTF-8");
}
void validate_input(const Input& input) {
    validate_category(input.category);
    if (input.payload.size() > max_payload_bytes || !valid_utf8(input.payload))
        throw std::invalid_argument("payload must be valid UTF-8 and at most 1048576 bytes");
}
Bytes encode(const Event& event) {
    Bytes body;
    body.reserve(fixed_body_size + event.category.size() + event.payload.size());
    put64(body, event.id);
    put64(body, std::bit_cast<std::uint64_t>(event.timestamp));
    put32(body, static_cast<std::uint32_t>(event.category.size()));
    put32(body, static_cast<std::uint32_t>(event.payload.size()));
    put64(body, std::bit_cast<std::uint64_t>(event.value));
    body.insert(body.end(), event.category.begin(), event.category.end());
    body.insert(body.end(), event.payload.begin(), event.payload.end());
    Bytes frame{'E', 'V', 'T', '1'};
    frame.reserve(frame_header_size + body.size());
    put32(frame, static_cast<std::uint32_t>(body.size()));
    put32(frame, crc32(body.data(), body.size()));
    put32(frame, crc32(frame.data(), frame.size()));
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
}
Event decode(const Bytes& body, std::uint64_t expected_id) {
    const auto id = get64(body.data());
    const auto category_size = get32(body.data() + 16);
    const auto payload_size = get32(body.data() + 20);
    if (id != expected_id || id == 0) corrupt("non-contiguous event ID");
    if (category_size == 0 || category_size > max_category_bytes || payload_size > max_payload_bytes ||
        fixed_body_size + static_cast<std::size_t>(category_size) + payload_size != body.size())
        corrupt("invalid string lengths");
    Event event{id, std::bit_cast<std::int64_t>(get64(body.data() + 8)),
                std::string(body.begin() + fixed_body_size, body.begin() + fixed_body_size + category_size),
                std::bit_cast<std::int64_t>(get64(body.data() + 24)),
                std::string(body.begin() + fixed_body_size + category_size, body.end())};
    if (!valid_utf8(event.category) || !valid_utf8(event.payload)) corrupt("invalid UTF-8");
    return event;
}
struct Scan { off_t end; std::uint64_t last_id; };
Scan walk(int fd, off_t file_size, bool allow_torn, const std::function<void(const Event&)>& visitor = {}) {
    off_t offset = file_header_size;
    std::uint64_t last_id = 0;
    while (offset < file_size) {
        const auto remaining = static_cast<std::uint64_t>(file_size - offset);
        if (remaining < frame_header_size) {
            if (allow_torn) return {offset, last_id};
            throw std::runtime_error("incomplete trailing frame; run recover with exclusive access");
        }
        std::array<std::uint8_t, frame_header_size> header{};
        read_all(fd, header.data(), header.size(), offset);
        if (std::memcmp(header.data(), "EVT1", 4) != 0 ||
            get32(header.data() + 12) != crc32(header.data(), 12)) corrupt("frame header checksum");
        const auto body_size = get32(header.data() + 4);
        if (body_size < fixed_body_size || body_size > max_body_size) corrupt("frame length out of bounds");
        if (frame_header_size + static_cast<std::uint64_t>(body_size) > remaining) {
            if (allow_torn) return {offset, last_id};
            throw std::runtime_error("incomplete trailing body; run recover with exclusive access");
        }
        Bytes body(body_size);
        read_all(fd, body.data(), body.size(), offset + frame_header_size);
        if (get32(header.data() + 8) != crc32(body.data(), body.size())) corrupt("record checksum");
        if (last_id == std::numeric_limits<std::uint64_t>::max()) corrupt("event ID overflow");
        const auto event = decode(body, last_id + 1);
        last_id = event.id;
        if (visitor) visitor(event);
        offset += static_cast<off_t>(frame_header_size + body_size);
    }
    return {offset, last_id};
}
} // namespace

Store::Store(const std::string& path, Access access) : writable_(access == Access::ReadWrite) {
    const int flags = (writable_ ? O_RDWR | O_CREAT : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    fd_ = ::open(path.c_str(), flags, 0600);
    if (fd_ < 0) io_error("open event store");
    try {
        struct stat statbuf{};
        if (::fstat(fd_, &statbuf) != 0) io_error("fstat");
        if (!S_ISREG(statbuf.st_mode)) throw std::runtime_error("event store must be a regular file");
        if (::flock(fd_, (writable_ ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0) io_error("lock event store");
        if (::fstat(fd_, &statbuf) != 0) io_error("fstat"); // size under the lock
        const auto expected = file_header();
        if (statbuf.st_size < static_cast<off_t>(file_header_size)) {
            Bytes actual(static_cast<std::size_t>(statbuf.st_size));
            read_all(fd_, actual.data(), actual.size(), 0);
            if (!std::equal(actual.begin(), actual.end(), expected.begin())) corrupt("file header");
            if (!writable_) throw std::runtime_error("incomplete file header; run recover");
            recovered_ = actual.size();
            write_all(fd_, expected, 0);
            sync_file(fd_);
            statbuf.st_size = file_header_size;
        } else {
            Bytes actual(file_header_size);
            read_all(fd_, actual.data(), actual.size(), 0);
            if (actual != expected) corrupt("file header or unsupported version");
        }
        const auto scan = walk(fd_, statbuf.st_size, writable_);
        end_ = scan.end;
        last_id_ = scan.last_id;
        if (end_ < statbuf.st_size) {
            recovered_ += static_cast<std::uint64_t>(statbuf.st_size - end_);
            truncate_synced(fd_, end_);
        }
        if (writable_) sync_parent(path); // directory entry durable before any acknowledgment
    } catch (...) { ::close(fd_); fd_ = -1; throw; }
}
Store::~Store() { if (fd_ >= 0) ::close(fd_); }
void Store::require_healthy() const {
    if (poisoned_) throw std::runtime_error("store is poisoned after failed rollback; close and inspect/reopen");
}
Event Store::append(const Input& input) {
    std::lock_guard<std::mutex> guard(mutex_);
    require_healthy();
    if (!writable_) throw std::runtime_error("cannot append to a read-only store");
    validate_input(input);
    if (last_id_ == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("event IDs exhausted");
    Event event{last_id_ + 1, input.timestamp, input.category, input.value, input.payload};
    const auto frame = encode(event);
    if (end_ > std::numeric_limits<off_t>::max() - static_cast<off_t>(frame.size()))
        throw std::overflow_error("file offset exhausted");
    try { write_all(fd_, frame, end_); sync_file(fd_); }
    catch (...) {
        const auto failure = std::current_exception();
        try { truncate_synced(fd_, end_); }
        catch (...) {
            poisoned_ = true;
            throw std::runtime_error("append failed and durable rollback failed; store poisoned; outcome uncertain");
        }
        std::rethrow_exception(failure);
    }
    end_ += static_cast<off_t>(frame.size());
    last_id_ = event.id;
    return event;
}
void Store::query(const Query& filter, const std::function<void(const Event&)>& visitor) const {
    std::lock_guard<std::mutex> guard(mutex_);
    require_healthy();
    if (filter.from > filter.to) throw std::invalid_argument("query start must not exceed end");
    if (filter.category) validate_category(*filter.category);
    walk(fd_, end_, false, [&](const Event& event) {
        if (event.timestamp >= filter.from && event.timestamp <= filter.to &&
            (!filter.category || event.category == *filter.category)) visitor(event);
    });
}
std::map<std::string, std::uint64_t> Store::counts(const Query& filter) const {
    std::map<std::string, std::uint64_t> result;
    query(filter, [&](const Event& event) {
        if (!result.contains(event.category) && result.size() == max_group_count)
            throw std::runtime_error("group count exceeds 100000; narrow the query");
        ++result[event.category];
    });
    return result;
}
std::string json_string(const std::string& text) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "\"";
    for (unsigned char ch : text) {
        if (ch == '"' || ch == '\\') { result += '\\'; result += static_cast<char>(ch); }
        else if (ch < 0x20) { result += "\\u00"; result += hex[ch >> 4]; result += hex[ch & 15]; }
        else result += static_cast<char>(ch);
    }
    result += '"';
    return result;
}
std::string to_json(const Event& event) {
    return "{\"id\":" + std::to_string(event.id) + ",\"timestamp\":" + std::to_string(event.timestamp) +
           ",\"category\":" + json_string(event.category) + ",\"value\":" + std::to_string(event.value) +
           ",\"payload\":" + json_string(event.payload) + "}";
}
} // namespace eventstore
