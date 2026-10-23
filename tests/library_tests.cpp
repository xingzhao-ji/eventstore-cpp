#include "eventstore.hpp"
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Function> void fails(Function function) {
    bool failed = false;
    try { function(); } catch (const std::exception&) { failed = true; }
    check(failed, "expected operation to fail");
}
struct Temp {
    std::filesystem::path root;
    std::string path;
    Temp() {
        auto pattern = (std::filesystem::temp_directory_path() / "eventstore-test-XXXXXX").string();
        if (!::mkdtemp(pattern.data())) throw std::runtime_error("mkdtemp failed");
        root = pattern;
        path = (root / "test.evt").string();
    }
    ~Temp() { std::error_code error; std::filesystem::remove_all(root, error); }
};
eventstore::Input input() { return {7, "category", 42, "payload"}; }

void normal() {
    Temp temp;
    {
        eventstore::Store store(temp.path);
        auto large = input();
        large.payload = std::string(eventstore::max_payload_bytes, 'x');
        large.category = std::string(eventstore::max_category_bytes, 'c');
        check(store.append(large).id == 1, "maximum-size event rejected");
        const auto size = std::filesystem::file_size(temp.path);
        large.payload += 'x';
        fails([&] { store.append(large); });
        for (const auto& bad : {std::string("\xc0\x80"), std::string("\xed\xa0\x80"),
                               std::string("\xf4\x90\x80\x80"), std::string("\xc2"),
                               std::string("\xe2\x28\xa1")}) {
            auto invalid = input(); invalid.payload = bad;
            fails([&] { store.append(invalid); });
        }
        check(std::filesystem::file_size(temp.path) == size, "invalid input modified file");
        auto nul = input(); nul.payload = std::string("a\0b", 3);
        check(store.append(nul).id == 2, "valid NUL text rejected");
        fails([&] { store.query({}, [](const auto&) { throw std::runtime_error("visitor stopped"); }); });
        check(store.append(input()).id == 3, "visitor exception poisoned store");
        std::vector<std::thread> writers;
        for (int n = 0; n < 4; ++n) writers.emplace_back([&] {
            for (int j = 0; j < 10; ++j) store.append(input());
        });
        for (auto& writer : writers) writer.join();
        std::uint64_t next = 1;
        store.query({}, [&](const auto& event) { check(event.id == next++, "threaded IDs not contiguous"); });
        check(next == 44, "threaded writes lost");
    }
    {
        eventstore::Store first(temp.path, eventstore::Access::ReadOnly);
        eventstore::Store second(temp.path, eventstore::Access::ReadOnly);
        fails([&] { eventstore::Store writer(temp.path); });
        fails([&] { first.append(input()); });
        check(second.counts({}).at("category") == 42, "reopened group count wrong");
    }
}

void partial_write_rollback() {
    Temp temp;
    eventstore::Store store(temp.path);
    store.append(input());
    const auto before = std::filesystem::file_size(temp.path);
    struct rlimit original{};
    check(::getrlimit(RLIMIT_FSIZE, &original) == 0, "getrlimit failed");
    auto limited = original;
    limited.rlim_cur = before + 21; // Header plus part of the body can reach the real file.
    std::signal(SIGXFSZ, SIG_IGN);
    check(::setrlimit(RLIMIT_FSIZE, &limited) == 0, "setrlimit failed");
    fails([&] { store.append(input()); });
    check(::setrlimit(RLIMIT_FSIZE, &original) == 0, "restore rlimit failed");
    check(std::filesystem::file_size(temp.path) == before, "failed write not rolled back");
    check(store.append(input()).id == 2, "rollback consumed an ID or poisoned healthy store");
    check(store.counts({}).at("category") == 2, "rollback left a phantom record");
}

void fault(const std::string& mode) {
    Temp temp;
    eventstore::Store store(temp.path);
    store.append(input());
    const auto before = std::filesystem::file_size(temp.path);
    check(::setenv("EVENTSTORE_TEST_FAIL_FSYNC", mode == "sync-once" ? "once" : "always", 1) == 0, "setenv");
    if (mode == "truncate-fail") {
        ::setenv("EVENTSTORE_TEST_FAIL_FSYNC", "once", 1);
        ::setenv("EVENTSTORE_TEST_FAIL_TRUNCATE", "1", 1);
    }
    fails([&] { store.append(input()); });
    ::unsetenv("EVENTSTORE_TEST_FAIL_FSYNC");
    ::unsetenv("EVENTSTORE_TEST_FAIL_TRUNCATE");
    if (mode == "sync-once") {
        check(std::filesystem::file_size(temp.path) == before, "sync failure not durably rolled back");
        check(store.append(input()).id == 2, "sync rollback consumed ID");
    } else {
        fails([&] { store.append(input()); });
        fails([&] { store.counts({}); });
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        const std::string mode = argc == 2 ? argv[1] : "normal";
        if (mode == "normal") normal();
        else if (mode == "rlimit") partial_write_rollback();
        else fault(mode);
        std::cout << mode << " passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
