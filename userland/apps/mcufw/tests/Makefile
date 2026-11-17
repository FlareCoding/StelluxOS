#
# Stellux mcufw Host Tests
#
# Builds `SOURCES` once, then links and runs each *.test.cpp with them. The top-level `make host-test` runs this.
#

HOST_CC  ?= cc
HOST_CXX ?= c++

SRC_DIR     := ../src
BUILD_DIR   := ../build/host
OBJECT_DIR  := $(BUILD_DIR)/objects
ZLIB_DIR    := ../../../lib/libz/zlib
BEARSSL_DIR := ../../../lib/libbearssl/bearssl
SOURCES     := $(SRC_DIR)/properties.cpp $(SRC_DIR)/files.cpp $(SRC_DIR)/fqbn.cpp $(SRC_DIR)/recipes.cpp \
               $(SRC_DIR)/versions.cpp $(SRC_DIR)/cores.cpp $(SRC_DIR)/tools.cpp $(SRC_DIR)/boards.cpp \
               $(SRC_DIR)/requirements.cpp $(SRC_DIR)/report.cpp $(SRC_DIR)/arguments.cpp \
               $(SRC_DIR)/boards_command.cpp $(SRC_DIR)/file_lock.cpp $(SRC_DIR)/json.cpp $(SRC_DIR)/inflate.cpp \
               $(SRC_DIR)/zip.cpp $(SRC_DIR)/digest.cpp $(SRC_DIR)/indexes.cpp $(SRC_DIR)/install_plan.cpp \
               $(SRC_DIR)/libraries.cpp $(SRC_DIR)/hooks.cpp \
               $(SRC_DIR)/sketch.cpp $(SRC_DIR)/build_plan.cpp $(SRC_DIR)/compiler_output.cpp
HEADERS     := $(SRC_DIR)/properties.hpp $(SRC_DIR)/files.hpp $(SRC_DIR)/fqbn.hpp $(SRC_DIR)/recipes.hpp \
               $(SRC_DIR)/versions.hpp $(SRC_DIR)/layout.hpp $(SRC_DIR)/cores.hpp $(SRC_DIR)/tools.hpp \
               $(SRC_DIR)/boards.hpp $(SRC_DIR)/requirements.hpp $(SRC_DIR)/failure.hpp $(SRC_DIR)/report.hpp \
               $(SRC_DIR)/arguments.hpp $(SRC_DIR)/commands.hpp $(SRC_DIR)/file_lock.hpp $(SRC_DIR)/json.hpp \
               $(SRC_DIR)/inflate.hpp $(SRC_DIR)/zip.hpp $(SRC_DIR)/digest.hpp $(SRC_DIR)/indexes.hpp \
               $(SRC_DIR)/installer.hpp $(SRC_DIR)/libraries.hpp $(SRC_DIR)/hooks.hpp \
               $(SRC_DIR)/sketch.hpp $(SRC_DIR)/build_plan.hpp $(SRC_DIR)/compiler_output.hpp $(SRC_DIR)/build.hpp
# The vendored C the modules call: all of zlib, and libbearssl's SHA-256
C_SOURCES   := $(wildcard $(ZLIB_DIR)/*.c) $(BEARSSL_DIR)/src/hash/sha2small.c \
               $(BEARSSL_DIR)/src/codec/dec32be.c $(BEARSSL_DIR)/src/codec/enc32be.c
OBJECTS     := $(patsubst $(SRC_DIR)/%.cpp,$(OBJECT_DIR)/%.o,$(SOURCES)) \
               $(patsubst %.c,$(OBJECT_DIR)/c/%.o,$(notdir $(C_SOURCES)))
TESTS       := $(patsubst %.test.cpp,$(BUILD_DIR)/%,$(wildcard *.test.cpp))
INCLUDE_DIR := $(BUILD_DIR)/include

CXXFLAGS := -std=c++20 -O2 -g -Wall -Wextra -Werror -I$(SRC_DIR) -I$(INCLUDE_DIR) -I$(ZLIB_DIR)
CFLAGS   := -O2 -DHAVE_UNISTD_H -DHAVE_STDARG_H -I$(BEARSSL_DIR)/inc -I$(BEARSSL_DIR)/src

vpath %.c $(ZLIB_DIR) $(BEARSSL_DIR)/src/hash $(BEARSSL_DIR)/src/codec

run: $(TESTS)
	@for test in $(TESTS); do echo "[RUN] $$test"; $$test || exit 1; done

# The sysroot installs libbearssl's headers under bearssl/, as the sources include them
$(INCLUDE_DIR)/bearssl:
	@mkdir -p $(INCLUDE_DIR)
	@ln -sfn $(abspath $(BEARSSL_DIR)/inc) $@

$(OBJECT_DIR)/%.o: $(SRC_DIR)/%.cpp $(HEADERS) | $(INCLUDE_DIR)/bearssl
	@mkdir -p $(OBJECT_DIR)
	@echo "[CXX] $< (host)"
	@$(HOST_CXX) $(CXXFLAGS) -c $< -o $@

$(OBJECT_DIR)/c/%.o: %.c
	@mkdir -p $(OBJECT_DIR)/c
	@echo "[CC]  $< (host)"
	@$(HOST_CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/%: %.test.cpp $(OBJECTS) $(HEADERS)
	@echo "[CXX] $< (host)"
	@$(HOST_CXX) $(CXXFLAGS) $< $(OBJECTS) -o $@

.SECONDARY: $(OBJECTS)
.PHONY: run
