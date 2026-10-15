# Runtime core: floating origin + .sg_chunk reader/writer.
# Dependency-free so it builds on a plain Linux host and can be cross-compiled
# for Android with the NDK toolchain (CXX=aarch64-linux-android<api>-clang++).

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wshadow -Werror
CPPFLAGS += -Iinclude

BUILD := build
SRC   := src/global_position.cpp src/floating_origin.cpp src/sg_chunk.cpp
TESTS := tests/test_main.cpp tests/test_global_position.cpp \
         tests/test_floating_origin.cpp tests/test_sg_chunk.cpp

.PHONY: test sanitize clean

$(BUILD)/sg_core_tests: $(SRC) $(TESTS) $(wildcard include/sg/*.hpp) tests/test_common.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(SRC) $(TESTS) -o $@

test: $(BUILD)/sg_core_tests
	./$(BUILD)/sg_core_tests

# Same tests under AddressSanitizer + UndefinedBehaviorSanitizer, with a larger fuzz budget.
sanitize:
	@mkdir -p $(BUILD)
	$(CXX) -std=c++17 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined \
	  -fno-sanitize-recover=undefined -Wall -Wextra -Wpedantic -Wshadow -Werror $(CPPFLAGS) \
	  $(SRC) $(TESTS) -o $(BUILD)/sg_core_tests_asan
	SG_FUZZ_ITERS=$${SG_FUZZ_ITERS:-100000} ./$(BUILD)/sg_core_tests_asan

clean:
	rm -rf $(BUILD)
