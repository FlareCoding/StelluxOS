# Builds the Qt-free Sheen modules once, then links and runs each *.test.cpp on
# the build machine for `make host-test`.

HOST_CXX ?= c++

SRC_DIR    := ../src
PERF_DIR   := $(SRC_DIR)/extensions/perf_binaries
BUILD_DIR  := ../build/host
OBJECT_DIR := $(BUILD_DIR)/objects

SOURCES := $(SRC_DIR)/app/workspace_store.cpp \
           $(SRC_DIR)/workspace/event_stream.cpp \
           $(SRC_DIR)/extensions/embedded/embedded_protocol.cpp \
           $(SRC_DIR)/extensions/workspace_paths.cpp \
           $(SRC_DIR)/extensions/python/python_traceback.cpp \
           $(SRC_DIR)/extensions/c_run/compiler_diagnostics.cpp \
           $(PERF_DIR)/elf_model.cpp
HEADERS := $(SRC_DIR)/app/workspace_store.h \
           $(SRC_DIR)/workspace/event_stream.h \
           $(SRC_DIR)/extensions/embedded/embedded_protocol.h \
           $(SRC_DIR)/extensions/workspace_paths.h \
           $(SRC_DIR)/extensions/python/python_traceback.h \
           $(SRC_DIR)/extensions/c_run/compiler_diagnostics.h \
           $(PERF_DIR)/elf_model.h

OBJECTS := $(patsubst $(SRC_DIR)/%.cpp,$(OBJECT_DIR)/%.o,$(SOURCES))
TESTS   := $(patsubst %.test.cpp,$(BUILD_DIR)/%,$(wildcard *.test.cpp))

CXXFLAGS := -std=c++20 -O2 -g -Wall -Wextra -Werror -I$(SRC_DIR)

run: $(TESTS)
	@for test in $(TESTS); do echo "[RUN] $$test"; $$test || exit 1; done

$(OBJECT_DIR)/%.o: $(SRC_DIR)/%.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	@echo "[CXX] $< (host)"
	@$(HOST_CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/%: %.test.cpp $(OBJECTS) $(HEADERS)
	@mkdir -p $(BUILD_DIR)
	@echo "[CXX] $< (host)"
	@$(HOST_CXX) $(CXXFLAGS) $< $(OBJECTS) -o $@

clean:
	@rm -rf $(BUILD_DIR)

.SECONDARY: $(OBJECTS)
.PHONY: run clean
