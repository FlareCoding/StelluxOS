#
# Stellux stlxdm Host Tests
#
# Builds each *.test.cpp with the sources in `SOURCES` by the build machine's own
# compiler and runs it. The top-level `make host-test` runs this.
#

HOST_CXX ?= c++

SRC_DIR   := ../src
BUILD_DIR := ../build/host
SOURCES   := $(SRC_DIR)/clipboard.cpp $(SRC_DIR)/damage.cpp $(SRC_DIR)/dnd.cpp $(SRC_DIR)/free_memory.cpp \
             $(SRC_DIR)/offer_intake.cpp \
             $(SRC_DIR)/shell/chord.cpp $(SRC_DIR)/shell/clipboard_search.cpp $(SRC_DIR)/shell/file_index.cpp \
             $(SRC_DIR)/shell/quick_calc.cpp $(SRC_DIR)/shell/quick_match.cpp
HEADERS   := $(SRC_DIR)/clipboard.hpp $(SRC_DIR)/damage.hpp $(SRC_DIR)/dnd.hpp $(SRC_DIR)/free_memory.hpp \
             $(SRC_DIR)/offer_intake.hpp \
             $(SRC_DIR)/shell/chord.hpp $(SRC_DIR)/shell/clipboard_search.hpp $(SRC_DIR)/shell/file_index.hpp \
             $(SRC_DIR)/shell/quick_calc.hpp $(SRC_DIR)/shell/quick_match.hpp \
             ../../../lib/libstlxwin/include/stlxwin/proto.h
TESTS     := $(patsubst %.test.cpp,$(BUILD_DIR)/%,$(wildcard *.test.cpp))

CXXFLAGS := -std=c++20 -O2 -g -Wall -Wextra -Werror -pthread -I$(SRC_DIR) -I../../../lib/libstlxwin/include

run: $(TESTS)
	@for test in $(TESTS); do echo "[RUN] $$test"; $$test || exit 1; done

$(BUILD_DIR)/%: %.test.cpp $(SOURCES) $(HEADERS)
	@mkdir -p $(BUILD_DIR)
	@echo "[CXX] $< (host)"
	@$(HOST_CXX) $(CXXFLAGS) $< $(SOURCES) -o $@

.PHONY: run
