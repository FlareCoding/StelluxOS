# Directories for objects and binaries
OBJ_DIR ?= obj
BIN_DIR ?= bin

# Target binary
TARGET := $(BIN_DIR)/hello_world

# Sources and objects
SRCS := hello_world.cpp
OBJS := $(addprefix $(OBJ_DIR)/, $(SRCS:.cpp=.o))

.PHONY: all clean

all: $(TARGET)

# Linking the target binary
$(TARGET): $(OBJS)
	@mkdir -p $(BIN_DIR)
	@echo "[LD] Linking $@"
	@$(LD) $(LDFLAGS) -o $@ $^

# Compiling the source files into the object directory
$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(OBJ_DIR)
	@echo "[CXX] Compiling $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	@echo "Cleaning $(OBJ_DIR) and $(BIN_DIR)..."
	@rm -rf $(OBJ_DIR) $(BIN_DIR)