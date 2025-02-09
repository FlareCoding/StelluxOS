# List of library directories inside lib/
LIB_DIRS := $(shell find . -mindepth 1 -maxdepth 1 -type d)

.PHONY: all clean

all:
	@for dir in $(LIB_DIRS); do \
		echo "[BUILD] Building library in $$dir..."; \
		$(MAKE) -C $$dir || exit 1; \
	done

clean:
	@for dir in $(LIB_DIRS); do \
		echo "[CLEAN] Cleaning library in $$dir..."; \
		$(MAKE) -C $$dir clean || exit 1; \
	done