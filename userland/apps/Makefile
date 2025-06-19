# =========================
# Userland Applications Makefile  
# =========================
# Uses exported variables from parent Makefile

# Application directories
APP_DIRS := init

# Default target - build all applications
all: $(APP_DIRS)

# Build each application
$(APP_DIRS):
	@echo "Building application: $@"
	@$(MAKE) -C $@

# Clean all applications
clean:
	@echo "Cleaning applications..."
	@for app in $(APP_DIRS); do \
		echo "Cleaning $$app..."; \
		$(MAKE) -C $$app clean; \
	done

.PHONY: all clean $(APP_DIRS)
