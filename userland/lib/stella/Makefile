# Tools
CXX        := g++
AR         := ar
RM         := rm

# Directories
OBJ_DIR ?= obj
LIB_DIR ?= lib

# Target library
TARGET := $(LIB_DIR)/libstella_ui.a

# Find all C++ source files recursively and their corresponding object files
SRCS := $(shell find . -type f -name "*.cpp")
OBJS := $(addprefix $(OBJ_DIR)/, $(SRCS:.cpp=.o))

.PHONY: all clean

all: $(TARGET)

# Archive all object files into the static library
$(TARGET): $(OBJS)
	@mkdir -p $(LIB_DIR)
	@echo "[AR] Creating static library $@"
	@$(AR) rcs $@ $^

# Compile source files into object files under the object directory
$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "[CXX] Compiling $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	@echo "Cleaning $(OBJ_DIR) and $(LIB_DIR)..."
	@$(RM) -rf $(OBJ_DIR) $(TARGET)
