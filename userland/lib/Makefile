#
# Stellux Userland - Libraries
#

LIB_DIRS := libstlx

all:
	@for lib in $(LIB_DIRS); do \
		$(MAKE) -C $$lib ARCH=$(ARCH) USERLAND_ROOT=$(USERLAND_ROOT) V=$(V); \
	done

clean:
	@for lib in $(LIB_DIRS); do \
		$(MAKE) -C $$lib clean ARCH=$(ARCH) USERLAND_ROOT=$(USERLAND_ROOT); \
	done

.PHONY: all clean
