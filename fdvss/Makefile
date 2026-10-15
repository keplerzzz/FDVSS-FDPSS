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
REFERENCE := $(BUILD)/reference_algebra.o $(BUILD)/reference_fdvss1.o $(BUILD)/reference_fdvss2.o $(BUILD)/reference_fdpss.o

.PHONY: all native test sanitize clean

all: $(BUILD)/fdvss_bench $(BUILD)/fdvss_tests $(BUILD)/fdvss_reference $(BUILD)/reference_tests

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

$(BUILD)/reference_%.o: src/reference_%.cpp include/fdvss/reference.hpp include/fdvss/field.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/reference_test_%.o: tests/reference_%.cpp include/fdvss/reference.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/reference_test1.o: tests/reference1.cpp include/fdvss/reference.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/reference_test2.o: tests/reference2.cpp include/fdvss/reference.hpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD)/fdvss_bench: $(CORE) $(BUILD)/main.o
	$(CXX) $^ $(LDFLAGS) -o $@

$(BUILD)/fdvss_tests: $(CORE) $(BUILD)/test_main.o
	$(CXX) $^ $(LDFLAGS) -o $@

$(BUILD)/fdvss_reference: $(CORE) $(REFERENCE) $(BUILD)/reference_main.o
	$(CXX) $^ $(LDFLAGS) -o $@

$(BUILD)/reference_tests: $(CORE) $(REFERENCE) $(BUILD)/reference_test_main.o $(BUILD)/reference_test_algebra_tests.o $(BUILD)/reference_test_fdpss_tests.o $(BUILD)/reference_test1.o $(BUILD)/reference_test2.o
	$(CXX) $^ $(LDFLAGS) -o $@

test: $(BUILD)/fdvss_tests $(BUILD)/reference_tests
	$(BUILD)/fdvss_tests
	$(BUILD)/reference_tests

sanitize:
	$(MAKE) clean
	$(MAKE) \
		CXXFLAGS="-std=c++20 -O1 -g -Wall -Wextra -Wshadow -pthread -fsanitize=address,undefined -fno-omit-frame-pointer" \
		LDFLAGS="-pthread -fsanitize=address,undefined" \
		all
	$(BUILD)/fdvss_tests
	$(BUILD)/reference_tests

clean:
	rm -rf $(BUILD)
