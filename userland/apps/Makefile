#
# Stellux Userland - Applications
#

APP_DIRS := init hello shell ls

all:
	@for app in $(APP_DIRS); do \
		$(MAKE) -C $$app ARCH=$(ARCH) USERLAND_ROOT=$(USERLAND_ROOT) V=$(V); \
	done

clean:
	@for app in $(APP_DIRS); do \
		$(MAKE) -C $$app clean ARCH=$(ARCH) USERLAND_ROOT=$(USERLAND_ROOT); \
	done

.PHONY: all clean
