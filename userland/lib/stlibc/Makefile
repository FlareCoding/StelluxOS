CXX ?= g++
AR  ?= ar
RM  ?= rm

# Build configuration
CXXFLAGS += -std=c++17 -Wall -Wextra -fPIC
CXXFLAGS += -I./include

# Directory structure
SRCDIR := src
INCDIR := include
OBJDIR := obj

# Find all source files recursively
SRCS := $(shell find $(SRCDIR) -name '*.cpp')
OBJS := $(patsubst $(SRCDIR)/%.cpp,$(OBJDIR)/%.o,$(SRCS))
LIB  := libstlibc.a

# Create object directory structure
OBJ_DIRS := $(sort $(dir $(OBJS)))

.PHONY: all clean

all: $(LIB)

$(LIB): $(OBJS)
	@echo "[AR] Creating library $@ from $^"
	@$(AR) rcs $@ $^

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJ_DIRS): | $(OBJDIR)
	mkdir -p $@

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp | $(OBJ_DIRS)
	@echo "[CXX] Compiling $<"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	$(RM) -rf $(OBJDIR) $(LIB)
