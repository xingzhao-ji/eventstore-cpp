#include "eventstore.hpp"
#include <charconv>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
std::int64_t integer(std::string_view input) {
    std::int64_t value{};
    const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), value);
    if (error != std::errc{} || end != input.data() + input.size())
        throw std::invalid_argument("expected a complete signed 64-bit decimal integer");
    return value;
}
void emit(const eventstore::Event& event) {
    // Append acknowledgment is flushed only after Store::append's fsync.
    std::cout << eventstore::to_json(event) << std::endl;
    if (!std::cout) throw std::runtime_error("output failed; the append may already be committed");
}
bool bounded_line(std::string& line) {
    constexpr auto limit = eventstore::max_payload_bytes + eventstore::max_category_bytes + 64;
    line.clear();
    char ch;
    while (std::cin.get(ch)) {
        if (ch == '\n') return true;
        if (line.size() >= limit) throw std::invalid_argument("input line exceeds bounded record size");
        line += ch;
    }
    if (std::cin.bad()) throw std::runtime_error("input read failed");
    return !line.empty();
}
void ingest(eventstore::Store& store) {
    std::string line;
    while (bounded_line(line)) {
        const auto a = line.find('\t');
        const auto b = a == std::string::npos ? a : line.find('\t', a + 1);
        const auto c = b == std::string::npos ? b : line.find('\t', b + 1);
        if (c == std::string::npos)
            throw std::invalid_argument("ingest requires timestamp<TAB>category<TAB>value<TAB>payload");
        emit(store.append({integer(std::string_view(line).substr(0, a)), line.substr(a + 1, b - a - 1),
                           integer(std::string_view(line).substr(b + 1, c - b - 1)), line.substr(c + 1)}));
    }
}
void usage() {
    std::cerr << "Usage:\n"
                 "  eventstore append FILE TIMESTAMP CATEGORY VALUE PAYLOAD\n"
                 "  eventstore ingest FILE  # TSV stdin; one synced JSON acknowledgment per event\n"
                 "  eventstore query FILE FROM TO [CATEGORY]\n"
                 "  eventstore counts FILE FROM TO [CATEGORY]\n"
                 "  eventstore recover FILE\n"
                 "Timestamps are signed 64-bit microseconds; ranges are inclusive.\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 3) { usage(); return 2; }
        const std::string command = argv[1];
        if (command == "append" && argc == 7) {
            const eventstore::Input input{integer(argv[3]), argv[4], integer(argv[5]), argv[6]};
            eventstore::Store store(argv[2]);
            emit(store.append(input));
        } else if (command == "ingest" && argc == 3) {
            eventstore::Store store(argv[2]);
            ingest(store);
        } else if (command == "recover" && argc == 3) {
            eventstore::Store store(argv[2]);
            std::cout << "{\"recovered_bytes\":" << store.recovered_bytes() << "}" << std::endl;
        } else if ((command == "query" || command == "counts") && (argc == 5 || argc == 6)) {
            eventstore::Query filter{integer(argv[3]), integer(argv[4]), std::nullopt};
            if (argc == 6) filter.category = argv[5];
            eventstore::Store store(argv[2], eventstore::Access::ReadOnly);
            if (command == "query") store.query(filter, emit);
            else for (const auto& [category, count] : store.counts(filter))
                std::cout << "{\"category\":" << eventstore::json_string(category) << ",\"count\":" << count << "}\n";
        } else { usage(); return 2; }
        std::cout.flush();
        if (!std::cout) throw std::runtime_error("output failed");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "eventstore: " << error.what() << '\n';
        return 1;
    }
}
