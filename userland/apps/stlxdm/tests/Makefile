#
# Stellux stlxdm Host Tests
#
# Builds each *.test.cpp with the sources in `SOURCES` by the build machine's own
# compiler and runs it. The top-level `make host-test` runs this.
#

HOST_CXX ?= c++

SRC_DIR   := ../src
BUILD_DIR := ../build/host
SOURCES   := $(SRC_DIR)/damage.cpp $(SRC_DIR)/shell/chord.cpp
HEADERS   := $(SRC_DIR)/damage.hpp $(SRC_DIR)/shell/chord.hpp
TESTS     := $(patsubst %.test.cpp,$(BUILD_DIR)/%,$(wildcard *.test.cpp))

CXXFLAGS := -std=c++20 -O2 -g -Wall -Wextra -Werror -I$(SRC_DIR)

run: $(TESTS)
	@for test in $(TESTS); do echo "[RUN] $$test"; $$test || exit 1; done

$(BUILD_DIR)/%: %.test.cpp $(SOURCES) $(HEADERS)
	@mkdir -p $(BUILD_DIR)
	@echo "[CXX] $< (host)"
	@$(HOST_CXX) $(CXXFLAGS) $< $(SOURCES) -o $@

.PHONY: run
