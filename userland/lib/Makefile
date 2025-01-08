# Tools
CXX        := g++
AS         := as
AR         := ar
RM         := rm

ASFLAGS    := --64 -g -O0 -nostdlib -fno-pie -fno-pic

# The objects and final library to produce
CRT_OBJS   := stellux_crt.o
CRT_LIB    := libstellux_crt.a

.PHONY: all clean

all: $(CRT_LIB)

%.o: %.S
	@echo "[AS ] Assembling $<"
	@$(AS) $(ASFLAGS) -c $< -o $@

$(CRT_LIB): $(CRT_OBJS)
	@echo "[AR ] Creating static library archive $<"
	@$(AR) rcs $(CRT_LIB) $(CRT_OBJS)

clean:
	$(RM) -f $(CRT_OBJS) $(CRT_LIB)