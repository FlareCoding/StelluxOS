CXX ?= g++
AR  ?= ar
RM  ?= rm

SRCS := stlibc.cpp
OBJS := $(SRCS:.cpp=.o)
LIB  := libstlibc.a

all: $(LIB)

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -I./include/ -c $< -o $@

clean:
	$(RM) -f $(OBJS) $(LIB)
