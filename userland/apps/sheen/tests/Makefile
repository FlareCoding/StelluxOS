# Builds the Qt-free Sheen modules and vendored SQLite once, then links and runs
# each *.test.cpp on the build machine for `make host-test`.

HOST_CC  ?= cc
HOST_CXX ?= c++

SRC_DIR    := ../src
PERF_DIR   := $(SRC_DIR)/extensions/perf_binaries
BUILD_DIR  := ../build/host
OBJECT_DIR := $(BUILD_DIR)/objects
SQLITE_DIR := ../../../lib/libsqlite3/sqlite

SOURCES := $(SRC_DIR)/app/workspace_store.cpp \
           $(SRC_DIR)/workspace/event_stream.cpp \
           $(SRC_DIR)/extensions/embedded/embedded_protocol.cpp \
           $(SRC_DIR)/extensions/workspace_paths.cpp \
           $(SRC_DIR)/extensions/python/python_traceback.cpp \
           $(SRC_DIR)/extensions/c_run/compiler_diagnostics.cpp \
           $(PERF_DIR)/elf_model.cpp \
           $(PERF_DIR)/disasm_mapper.cpp \
           $(PERF_DIR)/ktrace_model.cpp
HEADERS := $(SRC_DIR)/app/workspace_store.h \
           $(SRC_DIR)/workspace/event_stream.h \
           $(SRC_DIR)/extensions/embedded/embedded_protocol.h \
           $(SRC_DIR)/extensions/workspace_paths.h \
           $(SRC_DIR)/extensions/python/python_traceback.h \
           $(SRC_DIR)/extensions/c_run/compiler_diagnostics.h \
           $(PERF_DIR)/elf_model.h \
           $(PERF_DIR)/disasm_mapper.h \
           $(PERF_DIR)/ktrace_model.h

CXX_OBJECTS := $(patsubst $(SRC_DIR)/%.cpp,$(OBJECT_DIR)/%.o,$(SOURCES))
C_OBJECTS   := $(OBJECT_DIR)/c/sqlite3.o
OBJECTS     := $(CXX_OBJECTS) $(C_OBJECTS)
TESTS       := $(patsubst %.test.cpp,$(BUILD_DIR)/%,$(wildcard *.test.cpp))

CXXFLAGS := -std=c++20 -O2 -g -Wall -Wextra -Werror -I$(SRC_DIR) -I$(SQLITE_DIR)

SQLITE_CFLAGS := -O2 -DSQLITE_DQS=0 -DSQLITE_THREADSAFE=0 -DSQLITE_DEFAULT_MEMSTATUS=0 \
                 -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
                 -DSQLITE_MAX_EXPR_DEPTH=0 -DSQLITE_OMIT_DECLTYPE -DSQLITE_OMIT_DEPRECATED \
                 -DSQLITE_OMIT_PROGRESS_CALLBACK -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_USE_ALLOCA \
                 -DSQLITE_STRICT_SUBTYPE=1 -DSQLITE_OMIT_LOAD_EXTENSION

run: $(TESTS)
	@for test in $(TESTS); do echo "[RUN] $$test"; $$test || exit 1; done

$(OBJECT_DIR)/%.o: $(SRC_DIR)/%.cpp $(HEADERS)
	@mkdir -p $(dir $@)
	@echo "[CXX] $< (host)"
	@$(HOST_CXX) $(CXXFLAGS) -c $< -o $@

$(OBJECT_DIR)/c/sqlite3.o: $(SQLITE_DIR)/sqlite3.c $(SQLITE_DIR)/sqlite3.h
	@mkdir -p $(OBJECT_DIR)/c
	@echo "[CC]  $< (host)"
	@$(HOST_CC) $(SQLITE_CFLAGS) -c $< -o $@

$(BUILD_DIR)/%: %.test.cpp $(OBJECTS) $(HEADERS)
	@mkdir -p $(BUILD_DIR)
	@echo "[CXX] $< (host)"
	@$(HOST_CXX) $(CXXFLAGS) $< $(OBJECTS) -o $@

clean:
	@rm -rf $(BUILD_DIR)

.SECONDARY: $(OBJECTS)
.PHONY: run clean
