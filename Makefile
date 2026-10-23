CXX ?= c++
CPPFLAGS += -Iinclude
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++20 -Wall -Wextra -Wpedantic -Werror -pthread
LDFLAGS += -pthread
ifeq ($(shell uname -s),Linux)
TEST_EXTRA = build/faultio.so
endif

.PHONY: all test clean bench
all: build/eventstore build/libeventstore.a

build:
	mkdir -p build

build/eventstore.o: src/eventstore.cpp include/eventstore.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

build/libeventstore.a: build/eventstore.o
	ar rcs $@ $<

build/eventstore: src/main.cpp build/libeventstore.a include/eventstore.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< build/libeventstore.a $(LDFLAGS) -o $@

build/library_tests: tests/library_tests.cpp build/libeventstore.a include/eventstore.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< build/libeventstore.a $(LDFLAGS) -o $@

build/faultio.so: tests/faultio.c | build
	$(CC) -shared -fPIC -Wall -Wextra -Werror $< -ldl -o $@

test: all build/library_tests $(TEST_EXTRA)
	python3 -m unittest discover -s tests -p 'test_*.py' -v

build/query_bench: bench/query_bench.cpp build/libeventstore.a include/eventstore.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< build/libeventstore.a $(LDFLAGS) -o $@

bench: all build/query_bench
	python3 bench/compare.py

clean:
	rm -rf build
