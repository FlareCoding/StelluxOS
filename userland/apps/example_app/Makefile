# Directories for objects and binaries
OBJ_DIR ?= obj
BIN_DIR ?= bin

# Target binary
TARGET := $(BIN_DIR)/example_app

# Sources and objects
SRCS := $(wildcard *.cpp)
OBJS := $(addprefix $(OBJ_DIR)/, $(SRCS:.cpp=.o))

# Paths to libraries
LIB_DIR := ../../lib/stella/lib
INCLUDE_DIR := ../../lib/stella

# CXXFLAGS and LDFLAGS
CXXFLAGS += -I$(INCLUDE_DIR)
LDFLAGS += -L$(LIB_DIR) -lstella_ui

.PHONY: all clean

all: $(TARGET)

# Linking the target binary
$(TARGET): $(OBJS)
	@mkdir -p $(BIN_DIR)
	@echo "[LD] Linking $@"
	@$(LD) -o $@ $^ $(LDFLAGS)

# Compiling the source files into the object directory
$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(OBJ_DIR)
	@echo "[CXX] Compiling $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	@echo "Cleaning $(OBJ_DIR) and $(BIN_DIR)..."
	@rm -rf $(OBJ_DIR) $(BIN_DIR)