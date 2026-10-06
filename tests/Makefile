# Host-side unit tests for the dependency-free JSON parser.
#
# These run on the build machine, not on the console, so they need a plain
# host compiler (g++, clang++, or the MinGW toolchain). They are wired into the
# CI workflow so a broken parser fails the build before an .ovl is produced.
#
#   make -C tests run
#
# Notes:
#   - The workflow references this file, so keep the target names stable.
#   - Sanitizers need the pthread runtime on Linux; -pthread is included here
#     rather than relying on the compiler default.
#   - Only the JSON parser is tested here: it is the part that decides whether
#     the number shown on screen is correct, and it has no Switch dependency.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -O1 -g
LDFLAGS  ?=

# The devkitA64 cross build cannot use sanitizers, but the host build can. On a
# toolchain without them (e.g. a bare MinGW) the flags can be dropped with
#   make -C tests run SANITIZE=
SANITIZE ?= -fsanitize=address,undefined
ifneq ($(strip $(SANITIZE)),)
CXXFLAGS += $(SANITIZE)
LDFLAGS  += $(SANITIZE) -pthread
endif

INCLUDE  := -I../source

BUILD    := ./build

.PHONY: all run clean

all: $(BUILD)/test_json

$(BUILD)/test_json: test_json.cpp ../source/json.cpp ../source/json.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -o $@ test_json.cpp ../source/json.cpp $(LDFLAGS)

$(BUILD):
	@mkdir -p $(BUILD)

run: $(BUILD)/test_json
	@$(BUILD)/test_json

clean:
	@rm -rf $(BUILD)
