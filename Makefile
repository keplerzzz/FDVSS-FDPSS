CXX ?= c++
PYTHON ?= python3

.PHONY: all native test sanitize clean check-shared

all:
	$(MAKE) -C fdvss all
	$(MAKE) -C fdpss all

native:
	$(MAKE) -C fdvss native
	$(MAKE) -C fdpss native

check-shared:
	cmp fdvss/include/fdvss/field.hpp fdpss/include/fdvss/field.hpp
	cmp fdvss/include/fdvss/digest.hpp fdpss/include/fdvss/digest.hpp

build/field_arithmetic: tests/field_arithmetic.cpp fdvss/include/fdvss/field.hpp
	mkdir -p build
	$(CXX) -std=c++20 -O2 -Wall -Wextra -Ifdvss/include $< -o $@

test: check-shared build/field_arithmetic
	$(MAKE) -C fdvss test
	$(MAKE) -C fdpss test
	./build/field_arithmetic
	$(PYTHON) -B -m unittest discover -s tests -p 'test_*.py'

sanitize: check-shared
	$(MAKE) -C fdvss sanitize
	$(MAKE) -C fdpss sanitize
	mkdir -p build
	$(CXX) -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Ifdvss/include tests/field_arithmetic.cpp -o build/field_arithmetic_sanitize
	./build/field_arithmetic_sanitize

clean:
	$(MAKE) -C fdvss clean
	$(MAKE) -C fdpss clean
	rm -rf build
