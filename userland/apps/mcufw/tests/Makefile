#
# Stellux mcufw Host Tests
#
# Builds `SOURCES` once, then links and runs each *.test.cpp with them. The top-level `make host-test` runs this.
#

HOST_CXX ?= c++

SRC_DIR    := ../src
BUILD_DIR  := ../build/host
OBJECT_DIR := $(BUILD_DIR)/objects
SOURCES    := $(SRC_DIR)/properties.cpp $(SRC_DIR)/files.cpp $(SRC_DIR)/fqbn.cpp $(SRC_DIR)/recipes.cpp \
              $(SRC_DIR)/versions.cpp $(SRC_DIR)/cores.cpp $(SRC_DIR)/tools.cpp $(SRC_DIR)/boards.cpp \
              $(SRC_DIR)/requirements.cpp $(SRC_DIR)/report.cpp $(SRC_DIR)/arguments.cpp $(SRC_DIR)/boards_command.cpp
HEADERS    := $(SRC_DIR)/properties.hpp $(SRC_DIR)/files.hpp $(SRC_DIR)/fqbn.hpp $(SRC_DIR)/recipes.hpp \
              $(SRC_DIR)/versions.hpp $(SRC_DIR)/layout.hpp $(SRC_DIR)/cores.hpp $(SRC_DIR)/tools.hpp \
              $(SRC_DIR)/boards.hpp $(SRC_DIR)/requirements.hpp $(SRC_DIR)/failure.hpp $(SRC_DIR)/report.hpp \
              $(SRC_DIR)/arguments.hpp $(SRC_DIR)/commands.hpp
OBJECTS    := $(patsubst $(SRC_DIR)/%.cpp,$(OBJECT_DIR)/%.o,$(SOURCES))
TESTS      := $(patsubst %.test.cpp,$(BUILD_DIR)/%,$(wildcard *.test.cpp))

CXXFLAGS := -std=c++20 -O2 -g -Wall -Wextra -Werror -I$(SRC_DIR)

run: $(TESTS)
	@for test in $(TESTS); do echo "[RUN] $$test"; $$test || exit 1; done

$(OBJECT_DIR)/%.o: $(SRC_DIR)/%.cpp $(HEADERS)
	@mkdir -p $(OBJECT_DIR)
	@echo "[CXX] $< (host)"
	@$(HOST_CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/%: %.test.cpp $(OBJECTS) $(HEADERS)
	@echo "[CXX] $< (host)"
	@$(HOST_CXX) $(CXXFLAGS) $< $(OBJECTS) -o $@

.SECONDARY: $(OBJECTS)
.PHONY: run
