#include "eventstore.hpp"
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start) { return std::chrono::duration<double>(Clock::now() - start).count(); }
int main(int argc, char** argv) {
    try {
        if (argc != 5) throw std::invalid_argument("query_bench FILE FROM TO REPETITIONS");
        const eventstore::Query filter{std::stoll(argv[2]), std::stoll(argv[3]), std::nullopt};
        const int repetitions = std::stoi(argv[4]);
        if (repetitions < 1 || repetitions > 1000) throw std::invalid_argument("invalid repetitions");
        auto start = Clock::now();
        eventstore::Store store(argv[1], eventstore::Access::ReadOnly);
        const auto open_seconds = elapsed(start);
        auto result = store.counts(filter); // warm up file pages before timed queries
        std::vector<double> times;
        for (int i = 0; i < repetitions; ++i) {
            start = Clock::now();
            result = store.counts(filter);
            times.push_back(elapsed(start));
        }
        std::cout << "{\"open_seconds\":" << open_seconds << ",\"query_seconds\":[";
        for (std::size_t i = 0; i < times.size(); ++i) std::cout << (i ? "," : "") << times[i];
        std::cout << "],\"counts\":[";
        bool first = true;
        for (const auto& [category, count] : result) {
            std::cout << (first ? "" : ",") << "{\"category\":" << eventstore::json_string(category)
                      << ",\"count\":" << count << "}";
            first = false;
        }
        std::cout << "]}\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
