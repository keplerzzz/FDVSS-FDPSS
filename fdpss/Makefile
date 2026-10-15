CXX ?= c++
CPPFLAGS := -Iinclude
CXXFLAGS := -std=c++20 -O3 -DNDEBUG -Wall -Wextra -Wshadow -pthread -flto
LDFLAGS := -pthread -flto

ifeq ($(NATIVE),1)
  HOST_ARCH := $(shell uname -m)
  ifneq (,$(filter arm64 aarch64,$(HOST_ARCH)))
    CXXFLAGS += -mcpu=native
  else
    CXXFLAGS += -march=native
  endif
endif

BUILD := build
CORE := $(BUILD)/core.o

.PHONY: all native test sanitize clean

all: $(BUILD)/fdpss_bench $(BUILD)/fdpss_tests

native:
	$(MAKE) clean
	$(MAKE) NATIVE=1 all

$(BUILD):
	mkdir -p $(BUILD)

$(CORE): src/core.cpp include/fdvss/core.hpp include/fdvss/field.hpp include/fdvss/digest.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/main.o: src/main.cpp include/fdvss/core.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/test_main.o: tests/test_main.cpp include/fdvss/core.hpp include/fdvss/field.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/fdpss_bench: $(CORE) $(BUILD)/main.o
	$(CXX) $^ $(LDFLAGS) -o $@

$(BUILD)/fdpss_tests: $(CORE) $(BUILD)/test_main.o
	$(CXX) $^ $(LDFLAGS) -o $@

test: $(BUILD)/fdpss_tests
	$(BUILD)/fdpss_tests

sanitize:
	$(MAKE) clean
	$(MAKE) \
		CXXFLAGS="-std=c++20 -O1 -g -Wall -Wextra -Wshadow -pthread -fsanitize=address,undefined -fno-omit-frame-pointer" \
		LDFLAGS="-pthread -fsanitize=address,undefined" \
		all
	$(BUILD)/fdpss_tests

clean:
	rm -rf $(BUILD)
