# Standalone Makefile for the host-side rebuild of src/modules/visual/blockoutline_core.{hpp,cpp}.
#
# This intentionally does NOT use xmake / Android NDK / the Preloader
# package — those need a live game. The goal here is "does the rebuilt
# core compile and behave correctly on a stock Linux distro".
#
# Usage:
#     make            # build host_test/blockoutline_test
#     make test       # build and run it (writes SVGs to host_test/build/)
#     make clean
#
# We expose the BlockOutlineAlgo binary under host_test/out so the
# project tree stays tidy.

CXX      ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Wshadow
INCLUDES := -I../include -I../src -I../src/modules/visual -I.

CORE_SRCS  := ../src/modules/visual/blockoutline_core.cpp
TEST_SRC   := blockoutline_test.cpp
SRCS       := $(CORE_SRCS) $(TEST_SRC)

BIN       := blockoutline_test
BUILDDIR  := build

.PHONY: all test clean

all: $(BIN)

$(BIN): $(SRCS) ../include/bedrocktools/sdk/Types.hpp ../src/modules/visual/blockoutline_core.hpp tinyjson.hpp
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $(SRCS)

test: $(BIN)
	@echo "--- running $(BIN) ---"
	@./$(BIN)
	@echo "--- svg paths written ---"
	@ls -la $(BUILDDIR)/*.svg 2>/dev/null || true
	@if command -v python3 >/dev/null && python3 -c 'import PIL' >/dev/null 2>&1; then \
	  echo "--- generating png previews via preview_png.py ---"; \
	  python3 preview_png.py; \
	else \
	  echo "(skipping png preview: python3 + Pillow not available)"; \
	fi

clean:
	rm -f $(BIN)
	rm -rf $(BUILDDIR)
