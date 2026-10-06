# Host-side unit tests for the dependency-free JSON parser.
#
# These run on the build machine, not on the console, so they need a plain
# host compiler (g++, clang++, or the MinGW toolchain). They are wired into
# `ci.yml` so a broken parser fails the build before an .ovl is produced.
#
#   make -f tests/Makefile run

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined
INCLUDE  := -I../source

BUILD    := ./build

.PHONY: all run clean

all: $(BUILD)/test_json

$(BUILD)/test_json: test_json.cpp ../source/json.cpp ../source/json.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -o $@ test_json.cpp ../source/json.cpp

$(BUILD):
	@mkdir -p $(BUILD)

run: $(BUILD)/test_json
	@$(BUILD)/test_json

clean:
	@rm -rf $(BUILD)
